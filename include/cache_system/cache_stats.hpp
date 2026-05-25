#pragma once

#include "cache_system/config.hpp"

#include <cstddef>
#include <limits>

namespace cache_system {

struct cache_stats {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t inserts = 0;
    std::size_t updates = 0;
    std::size_t erases = 0;
    std::size_t evictions = 0;
    std::size_t expirations = 0;
    std::size_t rejections = 0;

    [[nodiscard]] std::size_t lookups() const noexcept {
        return saturating_add(hits, misses);
    }

    [[nodiscard]] double hit_rate() const noexcept {
        const std::size_t total = lookups();
        return total == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(total);
    }

    static void increment(std::size_t& counter) noexcept {
        if (counter != std::numeric_limits<std::size_t>::max()) {
            ++counter;
        }
    }

    static void add_to(std::size_t& counter, std::size_t value) noexcept {
        counter = saturating_add(counter, value);
    }

    cache_stats& operator+=(const cache_stats& other) noexcept {
        hits = saturating_add(hits, other.hits);
        misses = saturating_add(misses, other.misses);
        inserts = saturating_add(inserts, other.inserts);
        updates = saturating_add(updates, other.updates);
        erases = saturating_add(erases, other.erases);
        evictions = saturating_add(evictions, other.evictions);
        expirations = saturating_add(expirations, other.expirations);
        rejections = saturating_add(rejections, other.rejections);
        return *this;
    }

    [[nodiscard]] static constexpr std::size_t saturating_add(std::size_t left, std::size_t right) noexcept {
        return left > std::numeric_limits<std::size_t>::max() - right
            ? std::numeric_limits<std::size_t>::max()
            : left + right;
    }
};

[[nodiscard]] const char* version() noexcept;

} // namespace cache_system
