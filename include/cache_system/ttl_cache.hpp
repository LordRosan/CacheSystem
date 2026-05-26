#pragma once

#include "cache_system/cache_stats.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cache_system {
    template<
        typename Key,
        typename Value,
        typename Clock = std::chrono::steady_clock,
        typename Hash = std::hash<Key>,
        typename KeyEqual = std::equal_to<Key> >
    class ttl_cache {
    public:
        using key_type = Key;
        using mapped_type = Value;
        using clock_type = Clock;
        using duration = typename Clock::duration;
        using time_point = typename Clock::time_point;

        explicit ttl_cache(std::size_t capacity, Hash hash = Hash{}, KeyEqual equal = KeyEqual{})
            : capacity_(capacity), items_(0, std::move(hash), std::move(equal)) {
            if (capacity_ == 0) {
                throw std::invalid_argument("cache_system::ttl_cache capacity must be greater than zero");
            }
            items_.reserve(capacity_);
        }

        bool put(Key key, Value value, duration ttl) {
            purge_expired();
            if (ttl <= duration::zero()) {
                const std::size_t removed = items_.erase(key);
                cache_stats::add_to(stats_.expirations, removed);
                compact_expiration_index_if_needed();
                return false;
            }
            const auto expires_at = Clock::now() + ttl;
            auto found = items_.find(key);
            if (found != items_.end()) {
                const std::uint64_t generation = next_generation();
                expirations_.push(expiration_record{expires_at, found->first, generation});
                try {
                    found->second.value = std::move(value);
                } catch (...) {
                    generation_ = generation;
                    throw;
                }
                found->second.expires_at = expires_at;
                found->second.generation = generation;
                generation_ = generation;
                compact_expiration_index_if_needed();
                cache_stats::increment(stats_.updates);
                return false;
            }

            if (items_.size() >= capacity_) {
                purge_expired();
            }
            if (items_.size() >= capacity_) {
                evict_earliest_expiration();
            }

            const std::uint64_t generation = next_generation();
            auto [inserted, _] = items_.emplace(std::move(key), entry{std::move(value), expires_at, generation});
            try {
                expirations_.push(expiration_record{expires_at, inserted->first, inserted->second.generation});
            } catch (...) {
                items_.erase(inserted);
                throw;
            }
            generation_ = generation;
            compact_expiration_index_if_needed();
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
            purge_expired();
            auto found = items_.find(key);
            if (found == items_.end()) {
                cache_stats::increment(stats_.misses);
                return nullptr;
            }
            cache_stats::increment(stats_.hits);
            return &found->second.value;
        }

        [[nodiscard]] Value *peek_ref(const Key &key) {
            auto found = items_.find(key);
            if (found == items_.end() || found->second.expires_at <= Clock::now()) {
                return nullptr;
            }
            return &found->second.value;
        }

        [[nodiscard]] const Value *peek_ref(const Key &key) const {
            auto found = items_.find(key);
            if (found == items_.end() || found->second.expires_at <= Clock::now()) {
                return nullptr;
            }
            return &found->second.value;
        }

        bool try_get(const Key &key, Value &out) {
            auto value = get(key);
            if (!value) {
                return false;
            }
            out = std::move(*value);
            return true;
        }

        std::size_t purge_expired() {
            const auto now = Clock::now();
            std::size_t removed = 0;
            while (!expirations_.empty() && expirations_.top().expires_at <= now) {
                const expiration_record record = std::move(expirations_.top());
                expirations_.pop();
                auto found = items_.find(record.key);
                if (found != items_.end()
                    && found->second.generation == record.generation
                    && found->second.expires_at <= now) {
                    items_.erase(found);
                    ++removed;
                }
            }
            cache_stats::add_to(stats_.expirations, removed);
            return removed;
        }

        bool erase(const Key &key) {
            const auto removed = items_.erase(key);
            if (removed != 0) {
                cache_stats::increment(stats_.erases);
                compact_expiration_index_if_needed();
            }
            return removed != 0;
        }

        void clear() noexcept {
            items_.clear();
            expirations_ = {};
            generation_ = 0;
        }

        [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
        [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
        [[nodiscard]] cache_stats stats() const noexcept { return stats_; }

    private:
        struct entry {
            Value value;
            time_point expires_at;
            std::uint64_t generation = 0;
        };

        struct expiration_record {
            time_point expires_at;
            Key key;
            std::uint64_t generation = 0;

            [[nodiscard]] bool operator>(const expiration_record &other) const noexcept {
                return expires_at > other.expires_at;
            }
        };

        std::size_t capacity_;
        std::unordered_map<Key, entry, Hash, KeyEqual> items_;
        std::priority_queue<expiration_record, std::vector<expiration_record>, std::greater<> > expirations_;
        std::uint64_t generation_ = 0;
        cache_stats stats_;

        [[nodiscard]] std::uint64_t next_generation() const {
            if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("cache_system::ttl_cache generation overflow");
            }
            return generation_ + 1;
        }

        void compact_expiration_index_if_needed() {
            std::size_t threshold = capacity_;
            for (int i = 0; i < 4; ++i) {
                threshold = cache_stats::saturating_add(threshold, items_.size());
            }
            if (expirations_.size() <= threshold) {
                return;
            }

            std::priority_queue<expiration_record, std::vector<expiration_record>, std::greater<> > compacted;
            for (const auto &[key, item]: items_) {
                compacted.push(expiration_record{item.expires_at, key, item.generation});
            }
            expirations_ = std::move(compacted);
        }

        void evict_earliest_expiration() {
            while (!expirations_.empty()) {
                const expiration_record record = std::move(expirations_.top());
                expirations_.pop();
                auto found = items_.find(record.key);
                if (found != items_.end() && found->second.generation == record.generation) {
                    items_.erase(found);
                    cache_stats::increment(stats_.evictions);
                    return;
                }
            }

            if (!items_.empty()) {
                items_.erase(items_.begin());
                cache_stats::increment(stats_.evictions);
            }
        }
    };
} // namespace cache_system
