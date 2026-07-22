/**
 * @file slot_limit_config_template.h
 * @brief MultiSlotLimit 配置模板
 *
 * 使用方法:
 *   1. 将此文件复制到宿主项目中，重命名为 slot_limit_config.h
 *   2. 修改 SL_COUNT 和 SL_Id_e 为项目实际值
 *   3. 可选覆盖各项参数宏
 *   4. 确保 slot_limit_config.h 所在目录在 include 路径中优先于本目录
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

/* ========== 可选覆盖（默认值在 slot_limit.h 中定义） ========== */

// #define SL_DEBOUNCE_COUNT  5    // 去抖确认次数，默认 5
// #define SL_TASK_STACK_SIZE 512  // 任务栈大小(word)，默认 512
// #define SL_TASK_PRIORITY   6    // 任务优先级，默认 6

#endif
