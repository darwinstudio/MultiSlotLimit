#ifndef __SLOT_LIMIT_H_
#define __SLOT_LIMIT_H_

#include "stm32f4xx_hal.h"
#include <stdint.h>

/* 库通过 HAL_TIM_RegisterCallback 注册定时器溢出回调，依赖 Cube HAL 的
 * 弱回调注册机制。宿主必须在 stm32f4xx_hal_conf.h 中开启此宏，否则
 * HAL_TIM_RegisterCallback 为空宏，定时器静默失效。 */
#ifndef USE_HAL_TIM_REGISTER_CALLBACKS
#error                                                                         \
    "MultiSlotLimit requires USE_HAL_TIM_REGISTER_CALLBACKS=1 in stm32f4xx_hal_conf.h (needed by HAL_TIM_RegisterCallback)"
#endif

#include "slot_limit_config.h"

/* ========== 默认配置（宿主项目可在 slot_limit_config.h 中覆盖） ========== */

/* 硬件定时器句柄：宿主用 STM32CubeMX 配好 PSC，使 ARR = SL_TIMER_PERIOD_US 时
 * 溢出周期 = SL_TIMER_PERIOD_US 微秒。本库负责设置
 * ARR、注册溢出回调并启动定时器。 */
#ifndef SL_TIM_HANDLE
#error                                                                         \
    "SL_TIM_HANDLE must be defined by the host project (e.g. #define SL_TIM_HANDLE htim2)"
#endif

/* 定时器溢出周期（微秒）：同时用作
 *   1) __HAL_TIM_SET_AUTORELOAD(&SL_TIM_HANDLE, SL_TIMER_PERIOD_US) 的 ARR 值
 *   2) 去抖累加步长 stable += SL_TIMER_PERIOD_US
 * 更改扫描频率只需改这一个宏（ARR 与步长同步变化）。 */
#ifndef SL_TIMER_PERIOD_US
#define SL_TIMER_PERIOD_US 1000
#endif

/* 去抖稳定时间（微秒）：某限位连续一致的累计时间 >= 此值即确认状态切换。
 * 取代旧的"计次"模型 SL_DEBOUNCE_COUNT，与定时器周期解耦。 */
#ifndef SL_STABLE_TIME_US
#define SL_STABLE_TIME_US 3000
#endif

#ifndef SL_TASK_STACK_SIZE
#define SL_TASK_STACK_SIZE 512
#endif

#ifndef SL_TASK_PRIORITY
#define SL_TASK_PRIORITY 6
#endif

/* stable / 周期 / 稳定时间均以微秒为单位，用 uint16_t 承载，上限 65535us。
 * 编译期拦截超限配置，避免静默溢出。 */
_Static_assert(SL_TIMER_PERIOD_US <= 65535,
               "SL_TIMER_PERIOD_US exceeds uint16_t range");
_Static_assert(SL_STABLE_TIME_US <= 65535,
               "SL_STABLE_TIME_US exceeds uint16_t range");
_Static_assert(SL_TIMER_PERIOD_US > 0, "SL_TIMER_PERIOD_US must be > 0");

/**
 * @file slot_limit.h
 * @brief MultiSlotLimit - 可复用的限位开关检测库
 *
 * 提供基于 FreeRTOS + 硬件定时器的限位开关去抖检测，通过配置表抽象硬件。
 * 宿主项目需提供:
 *   - slot_limit_config.h: 定义 SL_COUNT, SL_Id_e, SL_TIM_HANDLE,
 *                          SL_TIMER_PERIOD_US, SL_STABLE_TIME_US
 *   - sl_hw_table[]: 硬件配置表（GPIO端口/引脚/触发电平）
 *
 * 检测由单个硬件定时器以 SL_TIMER_PERIOD_US 为周期在溢出中断(ISR)中扫描，
 * 按"连续稳定时间"模型去抖；确认触发后通知任务执行 SL_TriggerExecute。
 */

/* ========== 类型定义 ========== */

/** @brief 限位状态 */
typedef enum {
  SL_STATE_IDLE,    /**< 未触发 */
  SL_STATE_TRIGGER, /**< 已触发 */
  SL_STATE_INVALID  /**< 无效（ISR中调用或ID非法） */
} SL_State_e;

/** @brief 硬件配置结构体（由宿主项目在 sl_hw_table[] 中填充） */
typedef struct {
  GPIO_TypeDef *port; /**< GPIO端口 (GPIO_TypeDef*) */
  uint16_t pin;       /**< GPIO引脚 */
  uint8_t active;     /**< 触发电平 (1=HIGH, 0=LOW) */
} SL_HwConfig_t;

/* ========== 外部配置表（宿主项目定义） ========== */

/** @brief 硬件配置表，宿主项目必须定义，大小为 SL_COUNT */
extern const SL_HwConfig_t sl_hw_table[SL_COUNT];

/* ========== 公共 API ========== */

void SL_Init(void);
void SL_Open(SL_Id_e id);
void SL_Close(SL_Id_e id);
SL_State_e SL_GetStatus(SL_Id_e id);
SL_State_e SL_GetStateNoDelay(SL_Id_e id);
void SL_TriggerExecute(SL_Id_e id);
void SL_HardStop(SL_Id_e id);
uint8_t SL_OpenCustomInit(SL_Id_e id);

#endif
