#ifndef DUCK_COMPAT_H
#define DUCK_COMPAT_H

#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <string>
#include <stdexcept>


using Oid = uint32_t;
static constexpr Oid InvalidOid = 0;
using Relation = void *;
using BlockNumber = uint32_t;
using Size = size_t;

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;

inline void *palloc(size_t size)
{
    void *p = std::malloc(size);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}

inline void *palloc0(size_t size)
{
    void *p = std::calloc(1, size);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}

inline void pfree(void *ptr)
{
    std::free(ptr);
}

inline void *repalloc(void *ptr, Size size)
{
    void *p = std::realloc(ptr, size);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}

inline void *palloc_aligned(Size size, Size alignment, int /*flags*/)
{
    void *p = nullptr;
    if (posix_memalign(&p, alignment, size) != 0 || !p)
        throw std::bad_alloc();
    return p;
}

class BaseObject {
public:
    ~BaseObject() {}
    void *operator new(size_t size) { return palloc(size); }
    void operator delete(void *ptr) { pfree(ptr); }
};

#define mem_align_alloc(a, s)    palloc_aligned(s, a, 0)
#define mem_align_free(p)        pfree(p)

using MemoryContext = void *;
static constexpr MemoryContext CurrentMemoryContext = nullptr;
inline MemoryContext MemoryContextSwitchTo(MemoryContext ctx) { return ctx; }
inline void *MemoryContextAlloc(void * /*ctx*/, Size size) { return palloc(size); }
inline void *MemoryContextAllocZero(void * /*ctx*/, Size size) { return palloc0(size); }


#ifndef Assert
#ifdef USE_ASSERT_CHECKING
#define Assert(cond) do { if (!(cond)) { std::fprintf(stderr, "Assert failed: %s\n", #cond); std::abort(); } } while (0)
#else
#define Assert(cond) ((void)0)
#endif
#endif
#ifndef Assume
#define Assume(cond) ((void)0)
#endif

#ifndef MAXALIGN
#define MAXIMUM_ALIGNOF 8
#define MAXALIGN(LEN) (((std::size_t)(LEN) + (MAXIMUM_ALIGNOF - 1)) & ~((std::size_t)(MAXIMUM_ALIGNOF - 1)))
#endif
#ifndef TYPEALIGN
#define TYPEALIGN(ALIGNVAL, LEN) (((std::size_t)(LEN) + ((ALIGNVAL) - 1)) & ~((std::size_t)((ALIGNVAL) - 1)))
#endif

#ifndef FORCE_INLINE
#define FORCE_INLINE inline __attribute__((always_inline))
#endif

namespace vex {
namespace compat {

// Thread-local buffer to capture errmsg(...) formatted strings so ereport
// can use them when throwing / logging on the Duck side.
inline std::string& get_errmsg_buf() {
    thread_local std::string buf;
    return buf;
}

inline void store_errmsg(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    get_errmsg_buf() = buf;
}

} // namespace compat
} // namespace vex

enum {
    DEBUG5 = 10, DEBUG4 = 11, DEBUG3 = 12, DEBUG2 = 13, DEBUG1 = 14,
    LOG = 15, INFO = 17, NOTICE = 18, WARNING = 19, ERROR = 20,
    FATAL = 21, PANIC = 22,
};

enum {
    ERRCODE_INTERNAL_ERROR            = 1,
    ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE = 2,
    ERRCODE_LOG                       = 0,
};

/* errmsg stores formatted message into a thread-local buffer that ereport
 * picks up.  The comma-expression form (errcode(x), errmsg("fmt", ...)) is
 * preserved so PG-style callers compose naturally.
 * errcode/errdetail/errhint are no-ops on Duck — only the message string is
 * propagated. */
#define errcode(...)   ((void)0)
#define errmsg(fmt, ...) \
    (vex::compat::store_errmsg(fmt, ##__VA_ARGS__), "vex_err")
#define errmsg_internal(fmt, ...) errmsg(fmt, ##__VA_ARGS__)
#define errdetail(...) ((void)0)
#define errhint(...)   ((void)0)

/* ereport — PG-compatible error reporting.
 *
 *   PANIC  → fprintf + abort()     (kill everything, matches PG)
 *   FATAL  → fprintf + abort()     (kill current backend, abort on Duck)
 *   ERROR  → throw runtime_error   (recoverable, matches PG longjmp)
 *   lower  → fprintf(stderr)       (WARNING / NOTICE / LOG / DEBUG)
 */
#define ereport(level, ...) \
    do { \
        if ((level) >= PANIC) { \
            std::fprintf(stderr, "[vex] PANIC at %s:%d\n", __FILE__, __LINE__); \
            std::abort(); \
        } else if ((level) >= FATAL) { \
            std::fprintf(stderr, "[vex] FATAL at %s:%d\n", __FILE__, __LINE__); \
            std::abort(); \
        } else if ((level) >= ERROR) { \
            { __VA_ARGS__; } \
            auto &__vex_msg_ = vex::compat::get_errmsg_buf(); \
            std::string __vex_err_ = "[vex] ERROR: "; \
            __vex_err_ += __vex_msg_.empty() ? "unknown error" : __vex_msg_; \
            __vex_msg_.clear(); \
            throw std::runtime_error(__vex_err_); \
        } else { \
            { __VA_ARGS__; } \
            auto &__vex_msg_ = vex::compat::get_errmsg_buf(); \
            if (!__vex_msg_.empty()) { \
                std::fprintf(stderr, "[vex] %s\n", __vex_msg_.c_str()); \
                __vex_msg_.clear(); \
            } \
        } \
    } while (0)

/* elog — shorthand for ereport + errmsg_internal, mirrors PG's definition. */
#define elog(level, ...) \
    ereport(level, errmsg_internal(__VA_ARGS__))


#endif /* DUCK_COMPAT_H */
