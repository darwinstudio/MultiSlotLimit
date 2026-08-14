/**
 * @file slot_limit.c
 * @brief MultiSlotLimit - 限位开关检测库实现
 *
 * 基于 FreeRTOS + 单个硬件定时器的限位检测：
 *   - 定时器以 SL_TIMER_PERIOD_US 为周期在溢出中断(ISR)中扫描所有限位；
 *   - 去抖采用"连续稳定时间"模型：电平一致则 stable += 周期，否则清 0；
 *     stable >= SL_STABLE_TIME_US 即确认一次状态切换；
 *   - 确认触发后由 ISR 通知任务，任务上下文执行 SL_TriggerExecute。
 *
 * 并发模型（无临界区）：
 *   - sl_vars（stable / machine）的唯一写者是定时器 ISR；
 *   - SL_Open / SL_Close 仅置 sl_cmd 命令位，由 ISR 消费并应用；
 *   - sl_trig_pending 位图由 ISR 置位、任务消费。
 *   以上均为单 bit 纯写 / ISR 独占写，F407 上安全，无需 taskENTER_CRITICAL。
 */
#include "slot_limit.h"

#include "FreeRTOS.h"
#include "main.h"
#include "task.h"
#include "tim.h"
#ifdef SL_USE_EASYLOGGER
#define LOG_TAG "slot_limit"
#include "elog.h"
#endif

/** @brief 内部状态机 */
typedef enum {
    SL_MACHINE_CLOSE, /**< 检测已关闭 */
    SL_MACHINE_OPEN, /**< 等待 idle 电平（去抖确认中） */
    SL_MACHINE_WAIT_TRIGGER, /**< 等待 active 电平（去抖确认中） */
    SL_MACHINE_NUMS
} SL_Machine_e;

/** @brief 每个限位的运行时变量 */
typedef struct {
    volatile uint16_t stable; /**< 连续一致的累计时间(us) */
    volatile SL_Machine_e machine; /**< 状态机 */
} SL_Vars_t;

/** @brief 命令位（SL_Open/Close 置位，ISR 消费） */
typedef struct {
    volatile uint8_t open; /**< 请求开启 */
    volatile uint8_t close; /**< 请求关闭 */
} SL_CmdBit_t;

static volatile SL_Vars_t sl_vars[SL_COUNT];
static volatile SL_CmdBit_t sl_cmd[SL_COUNT];
static volatile uint8_t sl_trig_pending[SL_COUNT]; /**< ISR 置位，任务消费 */
static TaskHandle_t sl_handle = NULL;

/**
 * @brief 读取 GPIO 引脚电平（ISR 安全，直接读 IDR）
 * @param id 限位ID
 * @return 1=HIGH, 0=LOW
 */
static inline uint8_t sl_read_pin(SL_Id_e id) {
    return (sl_hw_table[id].port->IDR & sl_hw_table[id].pin) ? 1u : 0u;
}

/**
 * @brief 判断当前电平是否为触发电平
 * @param id 限位ID
 * @return 1=active, 0=idle
 */
static inline uint8_t sl_is_active(SL_Id_e id) {
    return sl_read_pin(id) == sl_hw_table[id].active;
}

/**
 * @brief 应用一次开启（原 SL_Open 内部逻辑，供 ISR 调用，不阻塞）
 * @param id 限位ID
 */
static void sl_do_open(SL_Id_e id) {
    sl_vars[id].stable = 0;

    uint8_t is_active = sl_is_active(id);
    uint8_t reverse   = SL_OpenCustomInit(id);

    if (is_active) {
        sl_vars[id].machine = reverse ? SL_MACHINE_OPEN : SL_MACHINE_WAIT_TRIGGER;
    } else {
        sl_vars[id].machine = reverse ? SL_MACHINE_WAIT_TRIGGER : SL_MACHINE_OPEN;
    }
}

/**
 * @brief 定时器溢出回调（ISR 上下文）
 *
 * 消费 SL_Open/Close 命令，扫描所有已开启限位做时间去抖；
 * 确认触发后用 FromISR 通知任务执行 SL_TriggerExecute。
 */
static void SL_TimerCallback(TIM_HandleTypeDef* htim) {
    if (htim->Instance != SL_TIM_HANDLE.Instance) {
        return;
    }

    for (SL_Id_e id = (SL_Id_e) 0; id < SL_COUNT; id++) {
        /* 消费命令位 */
        if (sl_cmd[id].open) {
            sl_cmd[id].open = 0;
            sl_do_open(id);
        }
        if (sl_cmd[id].close) {
            sl_cmd[id].close    = 0;
            sl_vars[id].machine = SL_MACHINE_CLOSE;
            sl_vars[id].stable  = 0;
        }

        if (sl_vars[id].machine == SL_MACHINE_CLOSE) {
            continue;
        }

        /* 确定期望电平：OPEN 阶段期望 idle，WAIT_TRIGGER 阶段期望 active */
        uint8_t expect_active  = (sl_vars[id].machine == SL_MACHINE_WAIT_TRIGGER);
        uint8_t current_active = sl_is_active(id);

        /* 时间去抖：一致则累加，不一致则清零 */
        if (current_active == expect_active) {
            sl_vars[id].stable += (uint16_t) SL_TIMER_PERIOD_US;
        } else {
            sl_vars[id].stable = 0;
        }

        /* 状态切换确认 */
        if (sl_vars[id].stable >= (uint16_t) SL_STABLE_TIME_US) {
            sl_vars[id].stable = 0;

            if (sl_vars[id].machine == SL_MACHINE_OPEN) {
                sl_vars[id].machine = SL_MACHINE_WAIT_TRIGGER;
            } else if (sl_vars[id].machine == SL_MACHINE_WAIT_TRIGGER) {
                sl_vars[id].machine = SL_MACHINE_CLOSE;
                SL_HardStop(id); /* 硬停：ISR 上下文立即执行，早于任务回调 */
                sl_trig_pending[id]                   = 1;
                BaseType_t higher_priority_task_woken = pdFALSE;
                vTaskNotifyGiveFromISR(sl_handle, &higher_priority_task_woken);
                portYIELD_FROM_ISR(higher_priority_task_woken);
            }
        }
    }
}

/**
 * @brief 任务入口函数（触发执行器）
 */
static void sl_task_entry(void* pvParameters) {
    (void) pvParameters;

    for (;;) {
        /* 被 ISR 唤醒后，处理所有待触发的限位 */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        for (SL_Id_e id = (SL_Id_e) 0; id < SL_COUNT; id++) {
            if (sl_trig_pending[id]) {
                sl_trig_pending[id] = 0;
                SL_TriggerExecute(id);
            }
        }
    }
}

/**
 * @brief 获取限位状态（无去抖，原始GPIO读取）
 * @param id 限位ID
 * @return SL_State_e
 */
SL_State_e SL_GetStateNoDelay(SL_Id_e id) {
    if (id >= SL_COUNT) {
        return SL_STATE_INVALID;
    }

    return sl_is_active(id) ? SL_STATE_TRIGGER : SL_STATE_IDLE;
}

/**
 * @brief 获取限位状态（带5ms去抖延时，任务上下文安全）
 * @param id 限位ID
 * @return SL_State_e
 * @note 本函数会 vTaskDelay(5ms)，禁止在中断中调用（会返回 SL_STATE_INVALID）。
 */
SL_State_e SL_GetStatus(SL_Id_e id) {
    if (id >= SL_COUNT || xPortIsInsideInterrupt()) {
        return SL_STATE_INVALID;
    }

    /* 第一次读取 */
    uint8_t first = sl_is_active(id);

    /* 延时 5ms 跳过机械抖动 */
    vTaskDelay(pdMS_TO_TICKS(5));

    /* 第二次读取 */
    uint8_t second = sl_is_active(id);

    /* 两次一致则确认状态 */
    if (first == second) {
        return first ? SL_STATE_TRIGGER : SL_STATE_IDLE;
    }
#ifdef SL_USE_EASYLOGGER
    log_w("The reading status of limit %d(driver) is inconsistent twice.", id);
#endif
    /* 不一致则视为未触发 */
    return SL_STATE_IDLE;
}

/**
 * @brief 开启限位检测
 * @param id 限位ID
 */
void SL_Open(SL_Id_e id) {
    if (id < SL_COUNT) {
        sl_cmd[id].open = 1; /* ISR 消费并应用，不直接改 sl_vars */
#ifdef SL_USE_EASYLOGGER
        log_i("Limit %d(driver) has been activated for detection", id);
#endif
    }
}

/**
 * @brief 关闭限位检测
 * @param id 限位ID
 */
void SL_Close(SL_Id_e id) {
    if (id < SL_COUNT) {
        sl_cmd[id].close = 1;
#ifdef SL_USE_EASYLOGGER
        log_i("Limit %d(driver) has been closed for detection", id);
#endif
    }
}

/**
 * @brief 初始化限位模块，创建 FreeRTOS 任务并启动扫描定时器
 */
void SL_Init(void) {
    static StackType_t sl_stack[SL_TASK_STACK_SIZE];
    static StaticTask_t sl_tcb;

    sl_handle =
        xTaskCreateStatic(sl_task_entry, "slot_limit", SL_TASK_STACK_SIZE, NULL, SL_TASK_PRIORITY, sl_stack, &sl_tcb);
    if (sl_handle == NULL) {
#ifdef SL_USE_EASYLOGGER
        log_e("The limit detection task create fail!");
#endif
        return; /* 任务创建失败（静态内存不足），不启动定时器，避免 ISR 通知空句柄 */
    }

    /* 配置并启动扫描定时器：ARR = SL_TIMER_PERIOD_US（PSC 由 CubeMX 配好） */
    __HAL_TIM_SET_AUTORELOAD(&SL_TIM_HANDLE, (uint16_t) SL_TIMER_PERIOD_US);
    HAL_TIM_RegisterCallback(&SL_TIM_HANDLE, HAL_TIM_PERIOD_ELAPSED_CB_ID, SL_TimerCallback);
    HAL_TIM_Base_Start_IT(&SL_TIM_HANDLE);
#ifdef SL_USE_EASYLOGGER
    log_i("The limit detection task has been initiated.");
#endif
}

/* ========== __weak 回调默认实现 ========== */

/**
 * @brief 限位触发回调（__weak，宿主项目强覆盖以处理触发事件）
 * @param id 触发的限位ID
 */
__weak void SL_TriggerExecute(SL_Id_e id) {
    (void) id;
}

/**
 * @brief 限位硬停回调（__weak，宿主项目强覆盖以立即停止电机）
 *
 * 在定时器 ISR 确认触发时立即调用，早于任务上下文的 SL_TriggerExecute。
 * 运行在 ISR 上下文：禁止阻塞，禁止调用非 FromISR 的 FreeRTOS API。
 *
 * @param id 触发的限位ID
 */
__weak void SL_HardStop(SL_Id_e id) {
    (void) id;
}

/**
 * @brief SL_Open 初始化状态自定义（__weak）
 *
 * 当限位开启时，库会读取当前GPIO电平并设置初始状态机。
 * 默认逻辑：active电平 -> WAIT_TRIGGER，idle电平 -> OPEN。
 * 宿主项目可强覆盖此函数，为特定限位反转初始化逻辑。
 *
 * @param id 限位ID
 * @return 1=使用反转逻辑(先找idle边沿), 0=使用默认逻辑
 * @note 本函数经 sl_do_open 由定时器 ISR 调用，运行在 ISR 上下文：禁止阻塞。
 */
__weak uint8_t SL_OpenCustomInit(SL_Id_e id) {
    (void) id;
    return 0;
}
