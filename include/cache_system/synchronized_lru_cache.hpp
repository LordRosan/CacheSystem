#pragma once

#include "cache_system/lru_cache.hpp"

#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>

namespace cache_system {
    template<
        typename Key,
        typename Value,
        typename Hash = std::hash<Key>,
        typename KeyEqual = std::equal_to<Key> >
    class synchronized_lru_cache {
    public:
        using key_type = Key;
        using mapped_type = Value;

        explicit synchronized_lru_cache(std::size_t capacity, Hash hash = Hash{}, KeyEqual equal = KeyEqual{})
            : cache_(capacity, std::move(hash), std::move(equal)) {
        }

        bool put(Key key, Value value) {
            std::unique_lock lock(mutex_);
            return cache_.put(std::move(key), std::move(value));
        }

        [[nodiscard]] std::optional<Value> get(const Key &key) {
            std::unique_lock lock(mutex_);
            return cache_.get(key);
        }

        bool try_get(const Key &key, Value &out) {
            std::unique_lock lock(mutex_);
            return cache_.try_get(key, out);
        }

        template<typename Func>
        bool with_value(const Key &key, Func &&func) {
            std::unique_lock lock(mutex_);
            Value *value = cache_.get_ref(key);
            if (value == nullptr) {
                return false;
            }
            std::forward<Func>(func)(*value);
            return true;
        }

        bool erase(const Key &key) {
            std::unique_lock lock(mutex_);
            return cache_.erase(key);
        }

        void clear() {
            std::unique_lock lock(mutex_);
            cache_.clear();
        }

        [[nodiscard]] bool contains(const Key &key) const {
            std::shared_lock lock(mutex_);
            return cache_.contains(key);
        }

        [[nodiscard]] std::size_t size() const {
            std::shared_lock lock(mutex_);
            return cache_.size();
        }

        [[nodiscard]] std::size_t capacity() const {
            std::shared_lock lock(mutex_);
            return cache_.capacity();
        }

        [[nodiscard]] cache_stats stats() const {
            std::shared_lock lock(mutex_);
            return cache_.stats();
        }

    private:
        mutable std::shared_mutex mutex_;
        lru_cache<Key, Value, Hash, KeyEqual> cache_;
    };
} // namespace cache_system
