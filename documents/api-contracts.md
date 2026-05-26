# CacheSystem API 契约

本文档记录公共 cache API 的行为契约。README 面向快速使用；这里面向维护者和库使用方判断边界、异常和并发语义。

## 通用约定

- 所有公共类型位于 `cache_system` namespace。
- `capacity == 0` 或 `max_weight == 0` 会抛出 `std::invalid_argument`。
- `get()` 返回 copied `std::optional<Value>`，因此要求 `Value` 可复制。
- `get_ref()` / `peek_ref()` 返回非 owning pointer，适用于 move-only `Value`。
- pointer 会被 mutation、eviction、erase、clear 或 cache 析构失效。
- `cache_stats` 使用 saturating arithmetic，计数溢出时不会回绕。

## `lru_cache`

- 不是 internally synchronized。
- `put()` 插入新 key 返回 `true`，更新已有 key 返回 `false`。
- `get()` / `get_ref()` 命中后更新 recency 和 hit counter；miss 更新 miss counter。
- `peek()` / `peek_ref()` 不更新 recency，也不更新 stats。
- Lookup 和 recency update 为 O(1) average case。

## `slru_cache`

- 不是 internally synchronized。
- 新 entry 进入 probationary segment。
- probationary 命中后提升到 protected segment。
- protected segment 超过 `protected_capacity` 时，把 protected LRU demote 回 probationary。
- 该策略用于降低 scan workload 对热点 entry 的污染。

## `tinylfu_cache`

- 不是 internally synchronized。
- 每次 `get()` 和 `put()` 都会记录 frequency。
- capacity 满时，candidate 只有在估计频率不低于 LRU victim 时才被接纳。
- 被拒绝的 candidate 不改变已有 entries，并增加 `cache_stats::rejections`。
- `reset_frequencies()` 只清空 Count-Min Sketch，不清空 cache values。

## `ttl_cache`

- 不是 internally synchronized。
- `put(key, value, ttl <= 0)` 不存储 value；若 key 已存在，则删除并记录 expiration。
- 过期索引用 min-heap 保存，使用 generation 过滤 stale expiration record。
- `purge_expired()` 清理当前已过期 entries。
- capacity pressure 下优先淘汰最早 `expires_at` 的 live entry。

## `weighted_lru_cache`

- 不是 internally synchronized。
- `Weigher` 返回 0 时按 weight 1 处理。
- value weight 大于 `max_weight` 时拒绝写入，并增加 `rejections`。
- 新插入失败时不得提前 eviction；只有 candidate 成功进入 index 后才进行容量调整。

## 并发 Wrappers

- `synchronized_lru_cache` 对整个 LRU cache 加锁，接口简单但竞争集中。
- `sharded_lru_cache` 把总 capacity 分给多个 shard，减小不同 key 的锁竞争。
- `sharded_lru_cache::clear()` 会按 shard 顺序清空所有 shards。
- `with_value()` 在持锁期间执行 callback；callback 必须保持短小，不能重入同一个 cache。
