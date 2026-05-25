#pragma once

#include "cache_system/cache_stats.hpp"

#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace cache_system {

template <
    typename Key,
    typename Value,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
class lru_cache {
public:
    using key_type = Key;
    using mapped_type = Value;
    using hash_type = Hash;
    using key_equal_type = KeyEqual;

    explicit lru_cache(std::size_t capacity, Hash hash = Hash{}, KeyEqual equal = KeyEqual{})
        : capacity_(capacity),
          items_(),
          index_(0, std::move(hash), std::move(equal)) {
        if (capacity_ == 0) {
            throw std::invalid_argument("cache_system::lru_cache capacity must be greater than zero");
        }
        index_.reserve(reserve_size_for_capacity(capacity_));
    }

    bool put(Key key, Value value) {
        auto found = index_.find(key);
        if (found != index_.end()) {
            found->second->value = std::move(value);
            items_.splice(items_.begin(), items_, found->second);
            found->second = items_.begin();
            cache_stats::increment(stats_.updates);
            return false;
        }

        items_.push_front(entry{std::move(key), std::move(value)});
        auto inserted_node = items_.begin();
        try {
            index_.emplace(inserted_node->key, inserted_node);
        } catch (...) {
            items_.erase(inserted_node);
            throw;
        }
        if (items_.size() > capacity_) {
            evict_lru();
        }
        cache_stats::increment(stats_.inserts);
        return true;
    }

    [[nodiscard]] std::optional<Value> get(const Key& key) {
        Value* value = get_ref(key);
        if (value == nullptr) {
            return std::nullopt;
        }
        return *value;
    }

    [[nodiscard]] Value* get_ref(const Key& key) {
        auto found = index_.find(key);
        if (found == index_.end()) {
            cache_stats::increment(stats_.misses);
            return nullptr;
        }
        items_.splice(items_.begin(), items_, found->second);
        found->second = items_.begin();
        cache_stats::increment(stats_.hits);
        return &found->second->value;
    }

    bool try_get(const Key& key, Value& out) {
        auto value = get(key);
        if (!value) {
            return false;
        }
        out = std::move(*value);
        return true;
    }

    [[nodiscard]] bool contains(const Key& key) const {
        return index_.find(key) != index_.end();
    }

    [[nodiscard]] std::optional<Value> peek(const Key& key) const {
        const Value* value = peek_ref(key);
        if (value == nullptr) {
            return std::nullopt;
        }
        return *value;
    }

    [[nodiscard]] Value* peek_ref(const Key& key) {
        auto found = index_.find(key);
        if (found == index_.end()) {
            return nullptr;
        }
        return &found->second->value;
    }

    [[nodiscard]] const Value* peek_ref(const Key& key) const {
        auto found = index_.find(key);
        if (found == index_.end()) {
            return nullptr;
        }
        return &found->second->value;
    }

    bool erase(const Key& key) {
        auto found = index_.find(key);
        if (found == index_.end()) {
            return false;
        }
        items_.erase(found->second);
        index_.erase(found);
        cache_stats::increment(stats_.erases);
        return true;
    }

    void clear() noexcept {
        items_.clear();
        index_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] cache_stats stats() const noexcept { return stats_; }

private:
    struct entry {
        Key key;
        Value value;
    };

    [[nodiscard]] static std::size_t reserve_size_for_capacity(std::size_t capacity) noexcept {
        return capacity == std::numeric_limits<std::size_t>::max() ? capacity : capacity + 1;
    }

    void evict_lru() {
        auto last = std::prev(items_.end());
        index_.erase(last->key);
        items_.erase(last);
        cache_stats::increment(stats_.evictions);
    }

    std::size_t capacity_;
    std::list<entry> items_;
    std::unordered_map<Key, typename std::list<entry>::iterator, Hash, KeyEqual> index_;
    cache_stats stats_;
};

} // namespace cache_system
