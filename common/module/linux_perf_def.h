/**
 * Copyright (c) 2026 VexDB-THU
 */

#ifndef LINUX_PERF_DEF_H
#define LINUX_PERF_DEF_H

#if !defined(PG_VEXDB_TARGET_DUCK)
#include "c.h"
#else
#include <cstdint>
#include <cstddef>
using uint32 = std::uint32_t;
using int64 = std::int64_t;
#endif

#ifndef ENABLE_HARDWARE_COUNTERS
#define ENABLE_HARDWARE_COUNTERS 0
#endif

enum class PerfEventType : uint32 {
    CPU_CYCLE = 0,
    INSTR_COUNT,
    CACHE_MISS,
    CACHE_HIT
};

constexpr size_t PERF_EVENT_TYPE_COUNT = ENABLE_HARDWARE_COUNTERS ? 4 : 2;

#if PERF_USAGE_EVENT
namespace internal {
inline void perf_to_str(const int64 *perfs, char *buf)
{
    *buf = '\0';
}
} /* namespace internal */
#endif

#endif /* LINUX_PERF_DEF_H */
