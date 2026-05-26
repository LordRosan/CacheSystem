#pragma once

#include "cache_system/lru_cache.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cache_system {
    template<
        typename Key,
        typename Value,
        typename Hash = std::hash<Key>,
        typename KeyEqual = std::equal_to<Key> >
    class sharded_lru_cache {
    public:
        using key_type = Key;
        using mapped_type = Value;

        explicit sharded_lru_cache(
            std::size_t capacity,
            std::size_t shard_count = 0,
            Hash hash = Hash{},
            KeyEqual equal = KeyEqual{})
            : hash_(std::move(hash)), capacity_(capacity) {
            if (capacity == 0) {
                throw std::invalid_argument("cache_system::sharded_lru_cache capacity must be greater than zero");
            }
            if (shard_count == 0) {
                shard_count = 16;
            }
            shard_count = std::max<std::size_t>(1, std::min(shard_count, capacity));
            const std::size_t base_capacity = capacity / shard_count;
            const std::size_t extra_capacity = capacity % shard_count;
            shards_.reserve(shard_count);
            for (std::size_t i = 0; i < shard_count; ++i) {
                shards_.push_back(std::make_unique<shard>(base_capacity + (i < extra_capacity ? 1 : 0), hash_, equal));
            }
        }

        bool put(Key key, Value value) {
            shard &target = select(key);
            std::unique_lock lock(target.mutex);
            return target.cache.put(std::move(key), std::move(value));
        }

        [[nodiscard]] std::optional<Value> get(const Key &key) {
            shard &target = select(key);
            std::unique_lock lock(target.mutex);
            return target.cache.get(key);
        }

        bool try_get(const Key &key, Value &out) {
            auto value = get(key);
            if (!value) {
                return false;
            }
            out = std::move(*value);
            return true;
        }

        template<typename Func>
        bool with_value(const Key &key, Func &&func) {
            shard &target = select(key);
            std::unique_lock lock(target.mutex);
            Value *value = target.cache.get_ref(key);
            if (value == nullptr) {
                return false;
            }
            std::forward<Func>(func)(*value);
            return true;
        }

        bool erase(const Key &key) {
            shard &target = select(key);
            std::unique_lock lock(target.mutex);
            return target.cache.erase(key);
        }

        [[nodiscard]] bool contains(const Key &key) const {
            const shard &target = select(key);
            std::shared_lock lock(target.mutex);
            return target.cache.contains(key);
        }

        [[nodiscard]] std::size_t size() const {
            std::size_t total = 0;
            for (const auto &item: shards_) {
                std::shared_lock lock(item->mutex);
                total = cache_stats::saturating_add(total, item->cache.size());
            }
            return total;
        }

        [[nodiscard]] std::size_t shard_count() const noexcept {
            return shards_.size();
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            return capacity_;
        }

        [[nodiscard]] cache_stats stats() const {
            cache_stats total;
            for (const auto &item: shards_) {
                std::shared_lock lock(item->mutex);
                total += item->cache.stats();
            }
            return total;
        }

    private:
        struct shard {
            shard(std::size_t capacity, Hash hash, KeyEqual equal)
                : cache(capacity, std::move(hash), std::move(equal)) {
            }

            mutable std::shared_mutex mutex;
            lru_cache<Key, Value, Hash, KeyEqual> cache;
        };

        [[nodiscard]] shard &select(const Key &key) {
            return *shards_[shard_index(key)];
        }

        [[nodiscard]] const shard &select(const Key &key) const {
            return *shards_[shard_index(key)];
        }

        [[nodiscard]] std::size_t shard_index(const Key &key) const {
            return hash_(key) % shards_.size();
        }

        Hash hash_;
        std::size_t capacity_;
        std::vector<std::unique_ptr<shard> > shards_;
    };
} // namespace cache_system
