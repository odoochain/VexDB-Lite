#ifndef HALF_H
#define HALF_H

#include <cstdint>

/* half 类型固定为 uint16（位模式）。
 *
 * 早期版本按编译 flag 在 uint16 / _Float16 / float16_t 之间切换，
 * 但 per-TU 不一致会导致同一符号的不同 mangle → 链接失败或 ABI 错配。
 * 现在 common/ 内所有代码统一用 uint16 位模式表示；转换通过 halfutils.h
 * 的 half_to_float / float_to_half 显式完成。
 *
 * F16C_SUPPORT 宏保留：per-ISA 的距离计算 TU 在 -mf16c 下编译时仍可
 * 通过该宏决定是否走 _mm_cvtph_ps 硬件路径，但这不影响 half 类型本身。
 *
 * 注：arm_neon 路径在 Duck 端不需要（HALF/INT8 是死代码），故下式不保留
 * FLT16_SUPPORT 分支。 */
#if defined(__F16C__)
#define F16C_SUPPORT
#endif

using half = uint16;
constexpr float HALF_MAX = 65504;

#endif /* HALF_H */
