# CacheSystem

CacheSystem 是一个 C++20 cache library，提供明确的 cache-policy contracts、可安装的 CMake exports、Release-only validation，以及可重复生成的 benchmark reports。公共 cache algorithms 都是 templates，因此保留在 headers 中；compiled source 只承载 non-template library surface。

## 功能范围

- `cache_system::lru_cache<Key, Value>`: capacity-bounded least-recently-used cache。
- `cache_system::slru_cache<Key, Value>`: 带 probationary 和 protected segments 的 segmented LRU cache。
- `cache_system::tinylfu_cache<Key, Value>`: TinyLFU-style admission cache，底层使用 LRU storage。
- `cache_system::ttl_cache<Key, Value>`: time-to-live cache，包含 min-heap expiration index 和 stale-record filtering。
- `cache_system::weighted_lru_cache<Key, Value>`: 根据 caller-defined value weight 而不是 item count 做 eviction 的 LRU cache。
- `cache_system::synchronized_lru_cache<Key, Value>`: 面向 shared access 的 mutex-protected LRU cache。
- `cache_system::sharded_lru_cache<Key, Value>`: 面向 concurrent workloads 的 sharded LRU cache。
- `cache_system::cache_stats`: hit、miss、eviction、expiration 和 mutation counters。
- `cache_system::config.hpp`: 用于 build 和 runtime reporting 的 version constants。
- CMake package export: `CacheSystem::cache_system`，支持 `add_subdirectory` 或安装后 `find_package` 使用。

## 目录结构

```text
include/cache_system/       Public cache templates 和 stats
source/cache_system/        Non-template implementation
cmake/                      Package config template
documents/                  API contracts、architecture 和 testing notes
tests/package_validation/   Installed-package validation project
test.cpp                    Optional standalone correctness 和 benchmark runner
results/                    Generated benchmark reports
CMakePresets.json           Standard Release 和 Debug-library configure presets
.clang-format               Formatting policy
.editorconfig               Editor defaults
```

## 作为依赖使用

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

对于 weight-based eviction：

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

## 设计说明

项目保留 policy implementations 的可读性，同时采用常见 production cache patterns：

- `unordered_map + list` 让 LRU lookup 和 recency updates 保持 O(1) average。
- SLRU 将 new entries 与 repeatedly-hit entries 分离，提高对 scan-heavy workloads 的抵抗力。
- TinyLFU admission 使用紧凑 Count-Min Sketch，在低频 candidates 淘汰热点 entries 前拒绝它们。
- TTL expiration 使用 heap index，使 purge cost 跟随 expired entries，而不是扫描整个 cache。
- Weighted eviction 建模 memory 或 business cost，而不仅是 item count。
- TTL capacity pressure 优先淘汰最早 `expires_at` 的 entry，而不是依赖 unordered container iteration order。
- Synchronized 和 sharded wrappers 展示两类常见 concurrency tradeoffs。
- 当 `Hash`、key copy 或 allocator operations 抛异常时，insert/update paths 保持 container invariants。
- Header-only template policies 让 generic cache code 可直接使用，不强行制造 `.cpp` boundaries。

## API 契约

- `lru_cache`、`ttl_cache` 和 `weighted_lru_cache` 不做 internal synchronization。Shared concurrent access 使用 `synchronized_lru_cache` 或 `sharded_lru_cache`。
- `get()` 更新 hit/miss counters，并可能更新 recency。它返回 copied `std::optional<Value>`。
- `get_ref()` 更新 hit/miss counters 和 recency，然后返回指向 stored value 的 non-owning pointer。该 pointer 会被 cache mutation、eviction、erase、clear 或析构失效。
- `peek_ref()` 返回 non-owning pointer，不改变 recency 或 stats。
- `slru_cache` 将 new entries 插入 probationary segment；命中后提升到 protected segment。如果 protected segment 超过 `protected_capacity`，其 LRU entry 会 demote 回 probationary。
- `tinylfu_cache` 在 `get()` 和 `put()` 时记录 frequency。Cache 满时，只有 candidate 的估计频率不低于 LRU victim 的估计频率才会被 admit；否则 write 被拒绝，并增加 `cache_stats::rejections`。
- `tinylfu_cache::reset_frequencies()` 清空 admission sketch，但不移除 cached values。
- `ttl_cache::put()` 在 `ttl <= duration::zero()` 时不存储 value；若 key 已存在，该 entry 会作为 immediately expired 被移除。
- `ttl_cache::clear()` 同时移除 stored entries 和 pending expiration records。
- `sharded_lru_cache` 将 constructor `capacity` 视为严格总容量；shards 会分摊该 capacity，不会放大总容量。
- `synchronized_lru_cache::with_value()` 和 `sharded_lru_cache::with_value()` 在持有相关 lock 时执行 callback；callbacks 必须短小，并且不能 re-enter 同一个 cache。
- `weighted_lru_cache` 拒绝 computed weight 超过 `max_weight` 的 values，并在 `cache_stats::rejections` 中记录 rejection。
- `cache_stats` aggregation 和 helper updates 使用 saturating arithmetic，避免 overflow wraparound。

## Benchmark

Benchmark 有意保留为根目录下可删除的 `test.cpp`。删除它不会影响 library target 或 installed package。

在 Release 下 configure、test 并运行 benchmark：

```powershell
cmake --preset release
cmake --build cmake-build-release
ctest --test-dir cmake-build-release --output-on-failure
.\cmake-build-release\cache_system_benchmark.exe
```

`test.cpp` 会先运行 correctness tests。若任一 correctness check 失败，它会在 `results/` 下写入报告，记录失败用例和原因，跳过 benchmark execution，并以 non-zero code 退出。

Reports 写入 `results/test-YYYY-MM-DD-HH-MM-SS.md`。Report metadata 也会记录请求展示格式 `YYYY/MM/DD-HH/MM/SS`；文件名使用短横线，因为 `/` 是 path separator。性能表包含 `group`、parameters、`ns/op`、`ops/s`、hit rate，以及基于同组最低 `ns/op` 的 per-group best comparison。

首次 benchmark 还会初始化 `results/performance-baseline.tsv`。后续 reports 会用当前 `ns/op` 对比该 baseline，记录 `delta ns/op`、`delta %`，并在 baseline comparison table 中解释 `ok`/`watch`/`new` 的判定。

Non-Release benchmark configuration 会 fail fast。Debug library-only 工作使用：

```powershell
cmake --preset debug-library
cmake --build cmake-build-debug
ctest --test-dir cmake-build-debug --output-on-failure
```

补充工程文档：

- [API 契约](documents/api-contracts.md)
- [架构与 invariants](documents/architecture.md)
- [测试与 Benchmark](documents/testing.md)
