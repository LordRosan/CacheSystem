#pragma once

#include "cache_system/cache_stats.hpp"

#include <algorithm>
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
class slru_cache {
public:
    using key_type = Key;
    using mapped_type = Value;
    using hash_type = Hash;
    using key_equal_type = KeyEqual;

    explicit slru_cache(
        std::size_t capacity,
        std::size_t protected_capacity = 0,
        Hash hash = Hash{},
        KeyEqual equal = KeyEqual{})
        : capacity_(capacity),
          protected_capacity_(normalize_protected_capacity(capacity, protected_capacity)),
          index_(0, std::move(hash), std::move(equal)) {
        if (capacity_ == 0) {
            throw std::invalid_argument("cache_system::slru_cache capacity must be greater than zero");
        }
        index_.reserve(reserve_size_for_capacity(capacity_));
    }

    bool put(Key key, Value value) {
        auto found = index_.find(key);
        if (found != index_.end()) {
            found->second.iterator->value = std::move(value);
            promote_or_touch(found);
            cache_stats::increment(stats_.updates);
            return false;
        }

        probationary_.push_front(entry{std::move(key), std::move(value)});
        auto inserted_node = probationary_.begin();
        try {
            index_.emplace(inserted_node->key, index_entry{segment::probationary, inserted_node});
        } catch (...) {
            probationary_.erase(inserted_node);
            throw;
        }

        enforce_capacity();
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
        promote_or_touch(found);
        cache_stats::increment(stats_.hits);
        return &found->second.iterator->value;
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
        return &found->second.iterator->value;
    }

    [[nodiscard]] const Value* peek_ref(const Key& key) const {
        auto found = index_.find(key);
        if (found == index_.end()) {
            return nullptr;
        }
        return &found->second.iterator->value;
    }

    bool erase(const Key& key) {
        auto found = index_.find(key);
        if (found == index_.end()) {
            return false;
        }
        erase_index_entry(found);
        cache_stats::increment(stats_.erases);
        return true;
    }

    void clear() noexcept {
        probationary_.clear();
        protected_.clear();
        index_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return probationary_.size() + protected_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] std::size_t protected_capacity() const noexcept {
        return protected_capacity_;
    }

    [[nodiscard]] std::size_t probationary_size() const noexcept {
        return probationary_.size();
    }

    [[nodiscard]] std::size_t protected_size() const noexcept {
        return protected_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] cache_stats stats() const noexcept {
        return stats_;
    }

private:
    struct entry {
        Key key;
        Value value;
    };

    using list_type = std::list<entry>;
    using iterator = typename list_type::iterator;

    enum class segment {
        probationary,
        protected_segment,
    };

    struct index_entry {
        segment owner = segment::probationary;
        iterator iterator{};
    };

    using index_type = std::unordered_map<Key, index_entry, Hash, KeyEqual>;
    using index_iterator = typename index_type::iterator;

    [[nodiscard]] static std::size_t reserve_size_for_capacity(std::size_t capacity) noexcept {
        return capacity == std::numeric_limits<std::size_t>::max() ? capacity : capacity + 1;
    }

    [[nodiscard]] static std::size_t normalize_protected_capacity(
        std::size_t capacity,
        std::size_t requested) {
        if (capacity == 0) {
            return 0;
        }
        if (requested == 0) {
            return std::max<std::size_t>(1, capacity - (capacity / 5));
        }
        if (requested > capacity) {
            throw std::invalid_argument("cache_system::slru_cache protected_capacity cannot exceed capacity");
        }
        return requested;
    }

    void promote_or_touch(index_iterator found) {
        if (found->second.owner == segment::protected_segment) {
            protected_.splice(protected_.begin(), protected_, found->second.iterator);
            found->second.iterator = protected_.begin();
            return;
        }

        protected_.splice(protected_.begin(), probationary_, found->second.iterator);
        found->second.owner = segment::protected_segment;
        found->second.iterator = protected_.begin();
        if (protected_.size() > protected_capacity_) {
            demote_protected_lru();
        }
    }

    void demote_protected_lru() {
        auto last = std::prev(protected_.end());
        auto found = index_.find(last->key);
        probationary_.splice(probationary_.begin(), protected_, last);
        if (found != index_.end()) {
            found->second.owner = segment::probationary;
            found->second.iterator = probationary_.begin();
        }
    }

    void enforce_capacity() {
        while (size() > capacity_) {
            if (!probationary_.empty()) {
                evict_probationary_lru();
                continue;
            }
            demote_protected_lru();
            evict_probationary_lru();
        }
    }

    void evict_probationary_lru() {
        auto last = std::prev(probationary_.end());
        auto found = index_.find(last->key);
        if (found != index_.end()) {
            index_.erase(found);
        }
        probationary_.erase(last);
        cache_stats::increment(stats_.evictions);
    }

    void erase_index_entry(index_iterator found) {
        if (found->second.owner == segment::protected_segment) {
            protected_.erase(found->second.iterator);
        } else {
            probationary_.erase(found->second.iterator);
        }
        index_.erase(found);
    }

    std::size_t capacity_;
    std::size_t protected_capacity_;
    list_type probationary_;
    list_type protected_;
    index_type index_;
    cache_stats stats_;
};

} // namespace cache_system
