#ifndef __SLOT_LIMIT_H_
#define __SLOT_LIMIT_H_

#include <stdint.h>

#include "slot_limit_config.h"

/* ========== 默认配置（宿主项目可在 slot_limit_config.h 中覆盖） ========== */

#ifndef SL_DEBOUNCE_COUNT
#define SL_DEBOUNCE_COUNT 5
#endif

#ifndef SL_TASK_STACK_SIZE
#define SL_TASK_STACK_SIZE 512
#endif

#ifndef SL_TASK_PRIORITY
#define SL_TASK_PRIORITY 6
#endif

/**
 * @file slot_limit.h
 * @brief MultiSlotLimit - 可复用的限位开关检测库
 *
 * 提供基于 FreeRTOS 的限位开关轮询去抖检测，通过配置表抽象硬件。
 * 宿主项目需提供:
 *   - slot_limit_config.h: 定义 SL_COUNT, SL_Id_e
 *   - sl_hw_table[]: 硬件配置表（GPIO端口/引脚/触发电平）
 */

/* ========== 类型定义 ========== */

/** @brief 限位状态 */
typedef enum
{
    SL_STATE_IDLE,    /**< 未触发 */
    SL_STATE_TRIGGER, /**< 已触发 */
    SL_STATE_INVALID  /**< 无效（ISR中调用或ID非法） */
} SL_State_e;

/** @brief 硬件配置结构体（由宿主项目在 sl_hw_table[] 中填充） */
typedef struct
{
    void *port;     /**< GPIO端口 (GPIO_TypeDef*) */
    uint16_t pin;   /**< GPIO引脚 */
    uint8_t active; /**< 触发电平 (1=HIGH, 0=LOW) */
} SL_HwConfig_t;

/* ========== 外部配置表（宿主项目定义） ========== */

/** @brief 硬件配置表，宿主项目必须定义，大小为 SL_COUNT */
extern const SL_HwConfig_t sl_hw_table[SL_COUNT];

/* ========== 公共 API ========== */

/**
 * @brief 初始化限位模块，创建 FreeRTOS 任务
 */
void SL_Init(void);

/**
 * @brief 开启限位检测
 * @param id 限位ID
 */
void SL_Open(SL_Id_e id);

/**
 * @brief 关闭限位检测
 * @param id 限位ID
 */
void SL_Close(SL_Id_e id);

/**
 * @brief 获取限位状态（带5ms去抖延时）
 * @param id 限位ID
 * @return SL_State_e
 */
SL_State_e SL_GetStatus(SL_Id_e id);

/**
 * @brief 获取限位状态（无去抖，原始GPIO读取）
 * @param id 限位ID
 * @return SL_State_e
 */
SL_State_e SL_GetStateNoDelay(SL_Id_e id);

/**
 * @brief 限位触发回调（__weak，宿主项目强覆盖以处理触发事件）
 * @param id 触发的限位ID
 */
void SL_TriggerExecute(SL_Id_e id);

/**
 * @brief SL_Open 初始化状态自定义（__weak）
 *
 * 当限位开启时，库会读取当前GPIO电平并设置初始状态机。
 * 默认逻辑：active电平 -> WAIT_TRIGGER，idle电平 -> OPEN。
 * 宿主项目可强覆盖此函数，为特定限位反转初始化逻辑。
 *
 * @param id 限位ID
 * @return 1=使用反转逻辑(先找idle边沿), 0=使用默认逻辑
 */
uint8_t SL_OpenCustomInit(SL_Id_e id);

#endif
