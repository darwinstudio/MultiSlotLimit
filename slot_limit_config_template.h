/**
 * @file slot_limit_config_template.h
 * @brief MultiSlotLimit 配置模板
 *
 * 使用方法:
 *   1. 将此文件复制到宿主项目中，重命名为 slot_limit_config.h
 *   2. 修改 SL_COUNT 和 SL_Id_e 为项目实际值
 *   3. 定义 SL_TIM_HANDLE / SL_TIMER_PERIOD_US / SL_STABLE_TIME_US
 *   4. 可选覆盖各项参数宏
 *   5. 确保 slot_limit_config.h 所在目录在 include 路径中优先于本目录
 */

#ifndef __SLOT_LIMIT_CONFIG_H_
#define __SLOT_LIMIT_CONFIG_H_

/* ========== 必须定义 ========== */

/** @brief 限位开关数量 */
#ifndef SL_COUNT
#define SL_COUNT 7
#endif

/**
 * @brief 限位开关 ID 枚举
 *
 * 宿主项目必须定义此枚举。以下为示例，请根据实际硬件修改。
 * SL_COUNT 必须等于枚举中的有效 ID 数量。
 *
 * 示例:
 *   typedef enum {
 *       LIMIT_ID_0,
 *       LIMIT_ID_1,
 *       LIMIT_ID_NUMS  // = SL_COUNT
 *   } SL_Id_e;
 */

/* ========== 定时器配置（必须定义） ========== */

/** @brief 扫描用硬件定时器句柄（CubeMX 生成的全局变量名）
 *
 * 宿主需在 CubeMX 中配置该 TIM 的预分频(PSC)，使 ARR = SL_TIMER_PERIOD_US 时
 * 溢出周期恰为 SL_TIMER_PERIOD_US 微秒（例如 PSC 配到 100us 对应的分频值）。
 * 库会调用 __HAL_TIM_SET_AUTORELOAD 设置 ARR 并注册溢出回调、启动定时器。
 */
// #define SL_TIM_HANDLE  htim2

/** @brief 定时器溢出周期（微秒），同时用作 ARR 值与去抖累加步长。
 *  更改扫描频率只需改此宏（ARR 与步长同步变化）。默认 100。
 */
// #define SL_TIMER_PERIOD_US 100

/** @brief 去抖稳定时间（微秒）：连续一致的累计时间 >= 此值即确认状态切换。
 *  取代旧的"计次"SL_DEBOUNCE_COUNT，与定时器周期解耦。默认 300。
 */
// #define SL_STABLE_TIME_US  300

/* ========== 可选覆盖（默认值在 slot_limit.h 中定义） ========== */

// #define SL_TASK_STACK_SIZE 512  // 任务栈大小(word)，默认 512
// #define SL_TASK_PRIORITY   6    // 任务优先级，默认 6

#endif
