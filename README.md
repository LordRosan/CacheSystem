# CacheSystem

CacheSystem is a compact C++20 cache library with explicit cache-policy contracts, installable CMake exports, release-only validation, and deterministic benchmark reports. The public cache algorithms are templates, so they stay in headers; compiled source is reserved for non-template library surface.

## What It Provides

- `cache_system::lru_cache<Key, Value>`: capacity-bounded least-recently-used cache.
- `cache_system::slru_cache<Key, Value>`: segmented LRU cache with probationary and protected segments.
- `cache_system::ttl_cache<Key, Value>`: time-to-live cache with a min-heap expiration index and stale-record filtering.
- `cache_system::weighted_lru_cache<Key, Value>`: LRU cache that evicts by caller-defined value weight instead of item count.
- `cache_system::synchronized_lru_cache<Key, Value>`: mutex-protected LRU cache for shared access.
- `cache_system::sharded_lru_cache<Key, Value>`: sharded LRU cache for concurrent workloads.
- `cache_system::cache_stats`: hit, miss, eviction, expiration, and mutation counters.
- `cache_system::config.hpp`: version constants for build and runtime reporting.
- CMake package export under `CacheSystem::cache_system` for `add_subdirectory` or installed `find_package` use.

## Layout

```text
include/cache_system/       Public cache templates and stats
source/cache_system/        Non-template implementation
cmake/                      Package config template
test.cpp                    Optional standalone correctness and benchmark runner
results/                    Generated benchmark reports
CMakePresets.json           Standard Release and Debug-library configure presets
.clang-format               Formatting policy
.editorconfig               Editor defaults
```

## Use As A Dependency

```cmake
add_subdirectory(path/to/CacheSystem)
target_link_libraries(your_target PRIVATE CacheSystem::cache_system)
```

```cpp
#include <cache_system/cache_system.hpp>

cache_system::lru_cache<int, std::string> cache(1024);
cache.put(7, "value");
auto value = cache.get(7);
```

For weight-based eviction:

```cpp
#include <cache_system/cache_system.hpp>

struct string_weight {
    std::size_t operator()(const std::string& value) const noexcept {
        return value.size();
    }
};

cache_system::weighted_lru_cache<int, std::string, string_weight> weighted(1024 * 1024);
weighted.put(1, "payload");
```

## Design Notes

The project keeps policy implementations readable while adopting common production cache patterns:

- `unordered_map + list` gives LRU O(1) lookup and recency updates.
- SLRU separates new entries from repeatedly-hit entries, improving resistance to scan-heavy workloads.
- TTL expiration uses a heap index so purge cost follows expired entries rather than scanning the whole cache.
- Weighted eviction models memory or business cost instead of only item count.
- TTL capacity pressure evicts the earliest `expires_at` entry instead of relying on unordered container iteration order.
- Synchronized and sharded wrappers show two common concurrency tradeoffs.
- Insert/update paths preserve container invariants when `Hash`, key copy, or allocator operations throw.
- Header-only template policies keep generic cache code usable without forcing artificial `.cpp` boundaries.

## API Contracts

- `lru_cache`, `ttl_cache`, and `weighted_lru_cache` are not internally synchronized. Use `synchronized_lru_cache` or `sharded_lru_cache` for shared concurrent access.
- `get()` updates hit/miss counters and may update recency. It returns a copied `std::optional<Value>`.
- `get_ref()` updates hit/miss counters and recency, then returns a non-owning pointer to the stored value. The pointer is invalidated by cache mutation, eviction, erase, clear, or destruction.
- `peek_ref()` returns a non-owning pointer without changing recency or stats.
- `slru_cache` inserts new entries into the probationary segment; a hit promotes the entry into the protected segment. If the protected segment exceeds `protected_capacity`, its LRU entry is demoted back to probationary.
- `ttl_cache::put()` with `ttl <= duration::zero()` does not store the value; if the key already exists, the entry is removed as immediately expired.
- `ttl_cache::clear()` removes both stored entries and pending expiration records.
- `sharded_lru_cache` treats the constructor `capacity` as a strict total capacity; shards divide that capacity without inflating it.
- `synchronized_lru_cache::with_value()` and `sharded_lru_cache::with_value()` execute the callback while holding the relevant lock; callbacks must stay short and must not re-enter the same cache.
- `weighted_lru_cache` rejects values whose computed weight exceeds `max_weight` and records the rejection in `cache_stats::rejections`.
- `cache_stats` aggregation and helper updates use saturating arithmetic instead of wrapping on overflow.

## Benchmark

The benchmark is intentionally a removable root-level `test.cpp`. Deleting it does not affect the library target or installed package.

Configure and run the benchmark in Release:

```powershell
cmake --preset release
cmake --build --preset release
.\cmake-build-release\cache_system_benchmark.exe
```

`test.cpp` runs correctness tests first. If any correctness check fails, it writes a report under `results/`, records the failing case and reason, skips benchmark execution, and exits with a non-zero code.

Reports are written to `results/test-YYYY-MM-DD-HH-MM-SS.md`. The report metadata also records the requested display form `YYYY/MM/DD-HH/MM/SS`; the filename uses dashes because `/` is a path separator on Windows.

Non-Release benchmark configuration fails fast. For Debug library-only work, use:

```powershell
cmake --preset debug-library
cmake --build --preset debug-library
```
