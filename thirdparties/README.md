# Third-Party Libraries

## Boost 1.91 (headers only, trimmed)

Only the files transitively required by `boost::unordered::concurrent_flat_map`,
`boost::lockfree::queue`, and `boost::preprocessor` are kept.
(~300 files, ~1.6 MB, from the original ~16,000 files / 189 MB).

## Changes from Upstream

### 1. `boost/unordered/detail/foa/concurrent_table.hpp`

**Problem:** `concurrent_flat_map` uses a per-process `static std::atomic<std::size_t>
thread_counter` to assign each thread a mutex shard via `++thread_counter % N`.
In PG's fork model, all parallel workers inherit the same counter value,
causing every worker to contend on the same rw_spinlock shard — defeating
the 128-way sharding.

**Fix (3 changes):**

a) Line ~1129: Replace shard assignment mechanism
```cpp
// Before:
thread_local auto id=(++thread_counter)%mutexes.size();
// After:
static auto id=getpid()%mutexes.size();
```

b) Line ~1975: Delete static member declaration
```cpp
// Before:  static std::atomic<std::size_t> thread_counter;
// After:   (removed)
```

c) Lines ~1979-1980: Delete out-of-class definition
```cpp
// Before:  template<...> std::atomic<std::size_t> concurrent_table<...>::thread_counter={};
// After:   (removed)
```

d) Add `#include <unistd.h>` for `getpid()`.

## Build Integration

`CMakeLists.txt` adds `${CMAKE_SOURCE_DIR}/thirdparties` to the include path
**before** system paths, so the vendored Boost always takes precedence.

## License

Boost Software License 1.0 — see `boost/LICENSE_1_0.txt`.
