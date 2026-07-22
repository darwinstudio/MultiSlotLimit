/**
 * @file slot_limit.c
 * @brief MultiSlotLimit - 限位开关检测库实现
 *
 * 基于 FreeRTOS 任务的轮询去抖限位检测。
 * 通过 sl_hw_table[] 配置表实现硬件抽象，零直接硬件引用。
 */

#include "slot_limit.h"
#include "FreeRTOS.h"
#include "main.h"
#include "task.h"

/* ========== 内部类型 ========== */

/** @brief 内部状态机 */
typedef enum
{
    SL_MACHINE_CLOSE,        /**< 检测已关闭 */
    SL_MACHINE_OPEN,         /**< 等待 idle 电平（去抖确认中） */
    SL_MACHINE_WAIT_TRIGGER, /**< 等待 active 电平（去抖确认中） */
    SL_MACHINE_NUMS
} SL_Machine_e;

/** @brief 每个限位的运行时变量 */
typedef struct
{
    volatile uint8_t cnt;          /**< 去抖计数器 */
    volatile SL_Machine_e machine; /**< 状态机 */
} SL_Vars_t;

/* ========== 内部变量 ========== */

static volatile SL_Vars_t sl_vars[SL_COUNT];
static TaskHandle_t sl_handle = NULL;

/* ========== 内部辅助函数 ========== */

/**
 * @brief 读取 GPIO 引脚电平
 * @param id 限位ID
 * @return 1=HIGH, 0=LOW
 */
static inline uint8_t sl_read_pin(SL_Id_e id)
{
    return (uint8_t)HAL_GPIO_ReadPin((GPIO_TypeDef *)sl_hw_table[id].port, sl_hw_table[id].pin);
}

/**
 * @brief 判断当前电平是否为触发电平
 * @param id 限位ID
 * @return 1=active, 0=idle
 */
static inline uint8_t sl_is_active(SL_Id_e id)
{
    return sl_read_pin(id) == sl_hw_table[id].active;
}

/**
 * @brief 判断是否有任何限位处于开启状态
 * @return 1=有, 0=无
 */
static uint8_t sl_any_active(void)
{
    uint8_t active = 0;
    taskENTER_CRITICAL();
    for (uint8_t i = 0; i < (uint8_t)SL_COUNT; i++)
    {
        if (sl_vars[i].machine != SL_MACHINE_CLOSE)
        {
            active = 1;
            break;
        }
    }
    taskEXIT_CRITICAL();
    return active;
}

/**
 * @brief 轮询检测所有已开启的限位
 */
static void sl_poll_check(void)
{
    for (SL_Id_e id = (SL_Id_e)0; id < SL_COUNT; id++)
    {
        taskENTER_CRITICAL();

        uint8_t need_trigger = 0;

        if (sl_vars[id].machine == SL_MACHINE_CLOSE)
        {
            taskEXIT_CRITICAL();
            continue;
        }

        /* 确定期望电平：OPEN 阶段期望 idle，WAIT_TRIGGER 阶段期望 active */
        uint8_t expect_active = (sl_vars[id].machine == SL_MACHINE_WAIT_TRIGGER);

        uint8_t current_active = sl_is_active(id);

        /* 去抖计数 */
        if (current_active == expect_active)
        {
            sl_vars[id].cnt++;
        }
        else
        {
            sl_vars[id].cnt = 0;
        }

        /* 状态切换确认 */
        if (sl_vars[id].cnt >= SL_DEBOUNCE_COUNT)
        {
            if (sl_vars[id].machine == SL_MACHINE_OPEN)
            {
                sl_vars[id].machine = SL_MACHINE_WAIT_TRIGGER;
                sl_vars[id].cnt = 0;
            }
            else if (sl_vars[id].machine == SL_MACHINE_WAIT_TRIGGER)
            {
                sl_vars[id].machine = SL_MACHINE_CLOSE;
                sl_vars[id].cnt = 0;
                need_trigger = 1;
            }
        }

        taskEXIT_CRITICAL();

        if (need_trigger)
        {
            SL_TriggerExecute(id);
        }
    }
}

/**
 * @brief 任务入口函数
 */
static void sl_task_entry(void *pvParameters)
{
    (void)pvParameters;

    for (;;)
    {
        if (sl_any_active())
        {
            sl_poll_check();
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        else
        {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

/* ========== 公共 API 实现 ========== */

SL_State_e SL_GetStateNoDelay(SL_Id_e id)
{
    if (id >= SL_COUNT)
    {
        return SL_STATE_INVALID;
    }

    return sl_is_active(id) ? SL_STATE_TRIGGER : SL_STATE_IDLE;
}

SL_State_e SL_GetStatus(SL_Id_e id)
{
    if (id >= SL_COUNT || xPortIsInsideInterrupt())
    {
        return SL_STATE_INVALID;
    }

    /* 第一次读取 */
    uint8_t first = sl_is_active(id);

    /* 延时 5ms 跳过机械抖动 */
    vTaskDelay(pdMS_TO_TICKS(5));

    /* 第二次读取 */
    uint8_t second = sl_is_active(id);

    /* 两次一致则确认状态 */
    if (first == second)
    {
        return first ? SL_STATE_TRIGGER : SL_STATE_IDLE;
    }

    /* 不一致则视为未触发 */
    return SL_STATE_IDLE;
}

void SL_Open(SL_Id_e id)
{
    if (id >= SL_COUNT)
    {
        return;
    }

    taskENTER_CRITICAL();

    sl_vars[id].cnt = 0;

    uint8_t is_active = sl_is_active(id);
    uint8_t reverse = SL_OpenCustomInit(id);

    if (is_active)
    {
        sl_vars[id].machine = reverse ? SL_MACHINE_OPEN : SL_MACHINE_WAIT_TRIGGER;
    }
    else
    {
        sl_vars[id].machine = reverse ? SL_MACHINE_WAIT_TRIGGER : SL_MACHINE_OPEN;
    }

    if (sl_handle != NULL)
    {
        xTaskNotifyGive(sl_handle);
    }

    taskEXIT_CRITICAL();
}

void SL_Close(SL_Id_e id)
{
    if (id >= SL_COUNT)
    {
        return;
    }

    taskENTER_CRITICAL();
    sl_vars[id].machine = SL_MACHINE_CLOSE;
    taskEXIT_CRITICAL();
}

void SL_Init(void)
{
    static StackType_t sl_stack[SL_TASK_STACK_SIZE];
    static StaticTask_t sl_tcb;

    sl_handle =
        xTaskCreateStatic(sl_task_entry, "slot_limit", SL_TASK_STACK_SIZE, NULL, SL_TASK_PRIORITY, sl_stack, &sl_tcb);
}

/* ========== __weak 回调默认实现 ========== */

__weak void SL_TriggerExecute(SL_Id_e id)
{
    (void)id;
}

__weak uint8_t SL_OpenCustomInit(SL_Id_e id)
{
    (void)id;
    return 0;
}
