# CacheSystem 架构

## 目标

CacheSystem 是一个面向 cache policy 学习与工程使用的库。它保留 LRU、SLRU、TinyLFU、TTL 和 weighted eviction 的核心机制，同时把异常安全、容量约束、stats 和并发包装作为工程约束处理。

## 模块结构

- `lru_cache`: `std::list + std::unordered_map` 实现 O(1) average lookup 和 recency update。
- `slru_cache`: probationary/protected 双 segment，增强 scan resistance。
- `tinylfu_cache`: Count-Min Sketch admission + LRU storage。
- `ttl_cache`: hash table 存 values，min-heap 管 expiration records。
- `weighted_lru_cache`: 按 value weight 而非 entry count 做 eviction。
- `synchronized_lru_cache`: 单锁同步 wrapper。
- `sharded_lru_cache`: 多 shard 并发 wrapper。

## 核心不变量

### List/Map caches

- `index_` 中每个 key 指向 list 中的 live node。
- list 中不存在没有 index entry 的 live node。
- eviction 必须同时删除 list node 和 index entry。
- mutation 失败时不得留下未索引 node。

### `slru_cache`

- 每个 index entry 标记自己属于 probationary 或 protected segment。
- `probationary_.size() + protected_.size() == index_.size()`。
- protected 超限时只 demote，不直接丢弃热点 entry。

### `tinylfu_cache`

- `counters_` 大小固定为 `sketch_width * sketch_depth`。
- 每个 row 使用不同 salt 混合 hash。
- counter 饱和在 `uint8_t::max()`；达到 reset threshold 后整体 aging。
- admission rejection 不改变 existing entries。

### `ttl_cache`

- `items_` 是 truth source。
- `expirations_` 可包含 stale records，必须用 generation 过滤。
- `clear()` 必须同时清空 items、expiration heap 和 generation。

### Concurrency

- `synchronized_lru_cache` 用一个 `shared_mutex` 保护底层 cache。
- `sharded_lru_cache` 每个 shard 独立持有 mutex 和 LRU cache。
- shard routing 不应持有全局互斥锁，否则会抵消 sharding 的主要收益。

## 失败路径设计

- 构造参数错误使用 `std::invalid_argument`。
- Hash、Key copy、allocator 等异常继续向上传播。
- 关键插入路径应保证失败后 cache 结构仍满足 list/map invariants。
- correctness tests 覆盖 hash exception、copy exception、overweight rejection、expired miss、move-only value 等路径。

## 复杂度

| 模块 | 关键操作 | 复杂度 |
| --- | --- | --- |
| `lru_cache` | get/put/erase | O(1) average |
| `slru_cache` | get/put/erase | O(1) average |
| `tinylfu_cache` | frequency update | O(sketch_depth) |
| `ttl_cache` | purge expired | O(expired * log pending_records) |
| `weighted_lru_cache` | put with eviction | O(evicted_count) |
| `sharded_lru_cache` | get/put | O(1) average after shard routing |
