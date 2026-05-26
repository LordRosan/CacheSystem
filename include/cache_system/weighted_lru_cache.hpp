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

template <typename Value>
struct unit_weigher {
    [[nodiscard]] std::size_t operator()(const Value&) const noexcept {
        return 1;
    }
};

template <
    typename Key,
    typename Value,
    typename Weigher = unit_weigher<Value>,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>>
class weighted_lru_cache {
public:
    using key_type = Key;
    using mapped_type = Value;
    using weigher_type = Weigher;

    explicit weighted_lru_cache(
        std::size_t max_weight,
        Weigher weigher = Weigher{},
        Hash hash = Hash{},
        KeyEqual equal = KeyEqual{})
        : max_weight_(max_weight),
          weigher_(std::move(weigher)),
          items_(),
          index_(0, std::move(hash), std::move(equal)) {
        if (max_weight_ == 0) {
            throw std::invalid_argument("cache_system::weighted_lru_cache max_weight must be greater than zero");
        }
    }

    bool put(Key key, Value value) {
        const std::size_t weight = normalize_weight(weigher_(value));
        if (weight > max_weight_) {
            erase(key);
            cache_stats::increment(stats_.rejections);
            return false;
        }

        auto found = index_.find(key);
        if (found != index_.end()) {
            const std::size_t previous_weight = found->second->weight;
            const std::size_t retained_weight = current_weight_ - previous_weight;
            if (retained_weight > std::numeric_limits<std::size_t>::max() - weight) {
                throw std::overflow_error("cache_system::weighted_lru_cache weight overflow");
            }
            found->second->value = std::move(value);
            found->second->weight = weight;
            current_weight_ = retained_weight + weight;
            items_.splice(items_.begin(), items_, found->second);
            found->second = items_.begin();
            evict_until_within_limit();
            cache_stats::increment(stats_.updates);
            return false;
        }

        index_.reserve(reserve_size_for_next_insert());
        items_.push_front(entry{std::move(key), std::move(value), weight});
        auto inserted_node = items_.begin();
        try {
            index_.emplace(inserted_node->key, inserted_node);
        } catch (...) {
            items_.erase(inserted_node);
            throw;
        }
        evict_until_fits(weight);
        current_weight_ += weight;
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

    bool erase(const Key& key) {
        auto found = index_.find(key);
        if (found == index_.end()) {
            return false;
        }
        current_weight_ -= found->second->weight;
        items_.erase(found->second);
        index_.erase(found);
        cache_stats::increment(stats_.erases);
        return true;
    }

    void clear() noexcept {
        items_.clear();
        index_.clear();
        current_weight_ = 0;
    }

    [[nodiscard]] bool contains(const Key& key) const {
        return index_.find(key) != index_.end();
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

    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] std::size_t max_weight() const noexcept { return max_weight_; }
    [[nodiscard]] std::size_t current_weight() const noexcept { return current_weight_; }
    [[nodiscard]] cache_stats stats() const noexcept { return stats_; }

private:
    struct entry {
        Key key;
        Value value;
        std::size_t weight = 1;
    };

    [[nodiscard]] static std::size_t normalize_weight(std::size_t weight) noexcept {
        return weight == 0 ? 1 : weight;
    }

    [[nodiscard]] std::size_t reserve_size_for_next_insert() const noexcept {
        return index_.size() == std::numeric_limits<std::size_t>::max() ? index_.size() : index_.size() + 1;
    }

    void evict_until_fits(std::size_t incoming_weight) {
        while (!items_.empty() && current_weight_ > max_weight_ - incoming_weight) {
            evict_one();
        }
    }

    void evict_until_within_limit() {
        while (!items_.empty() && current_weight_ > max_weight_) {
            evict_one();
        }
    }

    void evict_one() {
        auto last = std::prev(items_.end());
        const std::size_t removed_weight = last->weight;
        index_.erase(last->key);
        current_weight_ -= removed_weight;
        items_.erase(last);
        cache_stats::increment(stats_.evictions);
    }

    std::size_t max_weight_;
    std::size_t current_weight_ = 0;
    Weigher weigher_;
    std::list<entry> items_;
    std::unordered_map<Key, typename std::list<entry>::iterator, Hash, KeyEqual> index_;
    cache_stats stats_;
};

} // namespace cache_system
