#pragma once

#include "cache_system/cache_stats.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <list>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cache_system {
    template<
        typename Key,
        typename Value,
        typename Hash = std::hash<Key>,
        typename KeyEqual = std::equal_to<Key> >
    class tinylfu_cache {
    public:
        using key_type = Key;
        using mapped_type = Value;
        using hash_type = Hash;
        using key_equal_type = KeyEqual;

        explicit tinylfu_cache(
            std::size_t capacity,
            std::size_t sketch_width = 4096,
            std::size_t reset_threshold = 0,
            Hash hash = Hash{},
            KeyEqual equal = KeyEqual{})
            : capacity_(capacity),
              sketch_width_(sketch_width),
              reset_threshold_(reset_threshold == 0 ? default_reset_threshold(capacity) : reset_threshold),
              counters_(counter_count(sketch_width), 0),
              hash_(std::move(hash)),
              index_(0, hash_, std::move(equal)) {
            if (capacity_ == 0) {
                throw std::invalid_argument("cache_system::tinylfu_cache capacity must be greater than zero");
            }
            if (reset_threshold_ == 0) {
                throw std::invalid_argument("cache_system::tinylfu_cache reset_threshold must be greater than zero");
            }
            index_.reserve(reserve_size_for_capacity(capacity_));
        }

        bool put(Key key, Value value) {
            record_access(key);
            auto found = index_.find(key);
            if (found != index_.end()) {
                found->second->value = std::move(value);
                items_.splice(items_.begin(), items_, found->second);
                found->second = items_.begin();
                cache_stats::increment(stats_.updates);
                return false;
            }

            if (items_.size() >= capacity_ && !admit(key)) {
                cache_stats::increment(stats_.rejections);
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

        [[nodiscard]] std::optional<Value> get(const Key &key) {
            Value *value = get_ref(key);
            if (value == nullptr) {
                return std::nullopt;
            }
            return *value;
        }

        [[nodiscard]] Value *get_ref(const Key &key) {
            record_access(key);
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

        bool try_get(const Key &key, Value &out) {
            auto value = get(key);
            if (!value) {
                return false;
            }
            out = std::move(*value);
            return true;
        }

        [[nodiscard]] bool contains(const Key &key) const {
            return index_.find(key) != index_.end();
        }

        [[nodiscard]] std::optional<Value> peek(const Key &key) const {
            const Value *value = peek_ref(key);
            if (value == nullptr) {
                return std::nullopt;
            }
            return *value;
        }

        [[nodiscard]] Value *peek_ref(const Key &key) {
            auto found = index_.find(key);
            if (found == index_.end()) {
                return nullptr;
            }
            return &found->second->value;
        }

        [[nodiscard]] const Value *peek_ref(const Key &key) const {
            auto found = index_.find(key);
            if (found == index_.end()) {
                return nullptr;
            }
            return &found->second->value;
        }

        bool erase(const Key &key) {
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

        void reset_frequencies() noexcept {
            std::fill(counters_.begin(), counters_.end(), 0);
            sample_count_ = 0;
        }

        [[nodiscard]] std::uint8_t estimate_frequency(const Key &key) const {
            const std::size_t base_hash = hash_(key);
            std::uint8_t result = std::numeric_limits<std::uint8_t>::max();
            for (std::size_t row = 0; row < sketch_depth; ++row) {
                result = std::min(result, counter_at(row, base_hash));
            }
            return result;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return items_.size();
        }

        [[nodiscard]] std::size_t capacity() const noexcept {
            return capacity_;
        }

        [[nodiscard]] std::size_t sketch_width() const noexcept {
            return sketch_width_;
        }

        [[nodiscard]] std::size_t reset_threshold() const noexcept {
            return reset_threshold_;
        }

        [[nodiscard]] bool empty() const noexcept {
            return items_.empty();
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
        static constexpr std::size_t sketch_depth = 4;
        static constexpr std::array<std::uint64_t, sketch_depth> row_salts = {
            0x9e3779b97f4a7c15ULL,
            0xbf58476d1ce4e5b9ULL,
            0x94d049bb133111ebULL,
            0xd6e8feb86659fd93ULL,
        };

        [[nodiscard]] static std::size_t default_reset_threshold(std::size_t capacity) noexcept {
            if (capacity > std::numeric_limits<std::size_t>::max() / 10) {
                return std::numeric_limits<std::size_t>::max();
            }
            return std::max<std::size_t>(capacity * 10, 1);
        }

        [[nodiscard]] static std::size_t counter_count(std::size_t sketch_width) {
            if (sketch_width == 0) {
                throw std::invalid_argument("cache_system::tinylfu_cache sketch_width must be greater than zero");
            }
            if (sketch_width > std::numeric_limits<std::size_t>::max() / sketch_depth) {
                throw std::length_error("cache_system::tinylfu_cache sketch_width is too large");
            }
            return sketch_width * sketch_depth;
        }

        [[nodiscard]] static std::size_t reserve_size_for_capacity(std::size_t capacity) noexcept {
            return capacity == std::numeric_limits<std::size_t>::max() ? capacity : capacity + 1;
        }

        [[nodiscard]] static std::size_t mix_hash(std::size_t value, std::uint64_t salt) noexcept {
            std::uint64_t mixed = static_cast<std::uint64_t>(value) + salt;
            mixed ^= mixed >> 30;
            mixed *= 0xbf58476d1ce4e5b9ULL;
            mixed ^= mixed >> 27;
            mixed *= 0x94d049bb133111ebULL;
            mixed ^= mixed >> 31;
            return static_cast<std::size_t>(mixed);
        }

        [[nodiscard]] std::size_t counter_index(std::size_t row, std::size_t base_hash) const noexcept {
            return row * sketch_width_ + (mix_hash(base_hash, row_salts[row]) % sketch_width_);
        }

        [[nodiscard]] std::uint8_t counter_at(std::size_t row, std::size_t base_hash) const noexcept {
            return counters_[counter_index(row, base_hash)];
        }

        void record_access(const Key &key) {
            const std::size_t base_hash = hash_(key);
            for (std::size_t row = 0; row < sketch_depth; ++row) {
                std::uint8_t &counter = counters_[counter_index(row, base_hash)];
                if (counter != std::numeric_limits<std::uint8_t>::max()) {
                    ++counter;
                }
            }

            cache_stats::increment(sample_count_);
            if (sample_count_ >= reset_threshold_) {
                age_frequencies();
            }
        }

        void age_frequencies() noexcept {
            for (std::uint8_t &counter: counters_) {
                counter >>= 1;
            }
            sample_count_ = 0;
        }

        [[nodiscard]] bool admit(const Key &key) const {
            const std::uint8_t candidate_frequency = estimate_frequency(key);
            const std::uint8_t victim_frequency = estimate_frequency(items_.back().key);
            return candidate_frequency >= victim_frequency;
        }

        void evict_lru() {
            auto last = std::prev(items_.end());
            index_.erase(last->key);
            items_.erase(last);
            cache_stats::increment(stats_.evictions);
        }

        std::size_t capacity_;
        std::size_t sketch_width_;
        std::size_t reset_threshold_;
        std::vector<std::uint8_t> counters_;
        std::size_t sample_count_ = 0;
        list_type items_;
        Hash hash_;
        std::unordered_map<Key, typename list_type::iterator, Hash, KeyEqual> index_;
        cache_stats stats_;
    };
} // namespace cache_system
