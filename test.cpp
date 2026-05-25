#ifndef NDEBUG
#error "CacheSystem benchmark must be built in Release mode."
#endif

#include "cache_system/cache_system.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifndef CACHE_SYSTEM_RESULTS_DIR
#define CACHE_SYSTEM_RESULTS_DIR "results"
#endif

namespace {

using clock_type = std::chrono::steady_clock;

struct benchmark_result {
    std::string name;
    std::size_t operations = 0;
    double total_ms = 0.0;
    std::string notes;

    [[nodiscard]] double ns_per_op() const noexcept {
        return operations == 0 ? 0.0 : (total_ms * 1'000'000.0) / static_cast<double>(operations);
    }
};

struct correctness_check {
    std::string name;
    bool passed = false;
    std::string detail;
};

template <typename T>
void do_not_optimize(const T& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "m"(value) : "memory");
#elif defined(_MSC_VER)
    _ReadWriteBarrier();
    (void)value;
#else
    const volatile auto* sink = &value;
    (void)sink;
#endif
}

void clobber_memory() {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : : "memory");
#elif defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
}

template <typename Warmup, typename Work>
benchmark_result run_case(std::string name, std::size_t operations, Warmup warmup, Work work, std::string notes = {}) {
    warmup();
    clobber_memory();
    const auto begin = clock_type::now();
    work();
    clobber_memory();
    const auto end = clock_type::now();
    const std::chrono::duration<double, std::milli> elapsed = end - begin;
    return {std::move(name), operations, elapsed.count(), std::move(notes)};
}

std::string timestamp_for_file() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &raw);
#else
    localtime_r(&raw, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d-%H-%M-%S");
    return out.str();
}

std::string timestamp_pretty() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t raw = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &raw);
#else
    localtime_r(&raw, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y/%m/%d-%H/%M/%S");
    return out.str();
}

std::string bar(double value, double best) {
    if (value <= 0.0 || best <= 0.0) {
        return {};
    }
    const auto width = static_cast<std::size_t>(std::clamp(best / value, 0.05, 1.0) * 32.0);
    return std::string(width, '#');
}

void require_correctness(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Func>
correctness_check run_correctness_check(std::string name, Func&& func) {
    try {
        func();
        return {std::move(name), true, "通过"};
    } catch (const std::exception& error) {
        return {std::move(name), false, error.what()};
    } catch (...) {
        return {std::move(name), false, "捕获到未知异常"};
    }
}

struct manual_clock {
    using rep = long long;
    using period = std::milli;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<manual_clock>;
    static constexpr bool is_steady = true;

    static time_point now() noexcept {
        return current;
    }

    static time_point current;
};

manual_clock::time_point manual_clock::current{};

struct string_weight {
    [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
        return value.size();
    }
};

struct fault_injecting_hash {
    static inline int key_to_throw = 0;
    static inline int matching_calls = 0;
    static inline int throw_on_matching_call = 0;

    [[nodiscard]] std::size_t operator()(int key) const {
        if (throw_on_matching_call != 0 && key == key_to_throw) {
            ++matching_calls;
            if (matching_calls == throw_on_matching_call) {
                throw std::runtime_error("hash failure");
            }
        }
        return std::hash<int>{}(key);
    }

    static void reset(int key, int call_index) noexcept {
        key_to_throw = key;
        matching_calls = 0;
        throw_on_matching_call = call_index;
    }

    static void disable() noexcept {
        reset(0, 0);
    }
};

struct copy_throwing_key {
    int value = 0;
    static inline bool throw_on_copy = false;

    explicit copy_throwing_key(int input = 0) noexcept : value(input) {}

    copy_throwing_key(const copy_throwing_key& other) : value(other.value) {
        if (throw_on_copy) {
            throw std::runtime_error("key copy failure");
        }
    }

    copy_throwing_key(copy_throwing_key&& other) noexcept = default;
    copy_throwing_key& operator=(const copy_throwing_key&) = default;
    copy_throwing_key& operator=(copy_throwing_key&&) noexcept = default;

    [[nodiscard]] friend bool operator==(const copy_throwing_key& left, const copy_throwing_key& right) noexcept {
        return left.value == right.value;
    }
};

struct copy_throwing_key_hash {
    [[nodiscard]] std::size_t operator()(const copy_throwing_key& key) const noexcept {
        return std::hash<int>{}(key.value);
    }
};

std::vector<correctness_check> run_correctness_tests() {
    std::vector<correctness_check> checks;

    checks.push_back(run_correctness_check("sharded_lru_cache exact capacity", [] {
        cache_system::sharded_lru_cache<int, int> cache(17, 16);
        for (int i = 0; i < 100; ++i) {
            cache.put(i, i);
        }

        require_correctness(cache.capacity() == 17, "sharded_lru_cache 必须报告用户请求的 capacity");
        require_correctness(cache.size() <= 17, "sharded_lru_cache 不得突破用户请求的总 capacity");
        require_correctness(cache.stats().evictions > 0, "sharded_lru_cache 超过 capacity 后必须产生 eviction");
    }));

    checks.push_back(run_correctness_check("ttl_cache stale expiration records", [] {
        cache_system::ttl_cache<int, int, manual_clock> cache(8);
        manual_clock::current = manual_clock::time_point(manual_clock::duration(0));
        cache.put(1, 10, manual_clock::duration(10));

        manual_clock::current = manual_clock::time_point(manual_clock::duration(5));
        cache.put(1, 20, manual_clock::duration(100));

        manual_clock::current = manual_clock::time_point(manual_clock::duration(11));
        auto value = cache.get(1);
        require_correctness(value.has_value() && *value == 20, "ttl_cache 更新后必须忽略旧的 expiration record");
    }));

    checks.push_back(run_correctness_check("ttl_cache clear expiration index", [] {
        cache_system::ttl_cache<int, int, manual_clock> cache(8);
        manual_clock::current = manual_clock::time_point(manual_clock::duration(0));
        for (int i = 0; i < 8; ++i) {
            cache.put(i, i, manual_clock::duration(1000));
        }

        cache.clear();
        require_correctness(cache.size() == 0, "ttl_cache clear 必须移除所有 entry");
        manual_clock::current = manual_clock::time_point(manual_clock::duration(2000));
        require_correctness(cache.purge_expired() == 0, "ttl_cache clear 必须移除 pending expiration record");
    }));

    checks.push_back(run_correctness_check("ttl_cache capacity evicts earliest expiration", [] {
        cache_system::ttl_cache<int, int, manual_clock> cache(2);
        manual_clock::current = manual_clock::time_point(manual_clock::duration(0));
        cache.put(1, 10, manual_clock::duration(100));
        cache.put(2, 20, manual_clock::duration(200));
        cache.put(3, 30, manual_clock::duration(300));

        require_correctness(!cache.get(1).has_value(), "ttl_cache 满容量时应优先淘汰最早 expires_at 的 entry");
        require_correctness(cache.get(2).value_or(0) == 20, "ttl_cache 不应淘汰较晚 expires_at 的 entry");
        require_correctness(cache.get(3).value_or(0) == 30, "ttl_cache 新写入 entry 必须可读");
    }));

    checks.push_back(run_correctness_check("lru_cache move-only get_ref", [] {
        cache_system::lru_cache<int, std::unique_ptr<int>> cache(2);
        cache.put(1, std::make_unique<int>(42));

        auto* value = cache.get_ref(1);
        require_correctness(value != nullptr && **value == 42, "lru_cache get_ref 必须支持 move-only Value");

        auto* peeked = cache.peek_ref(1);
        require_correctness(peeked != nullptr && **peeked == 42, "lru_cache peek_ref 必须支持 move-only Value");
    }));

    checks.push_back(run_correctness_check("slru_cache segmented promotion", [] {
        cache_system::slru_cache<int, int> cache(3, 2);
        cache.put(1, 10);
        cache.put(2, 20);

        require_correctness(cache.probationary_size() == 2, "slru_cache 新 entry 必须进入 probationary segment");
        require_correctness(cache.get(1).value_or(0) == 10, "slru_cache 必须读取已存在 entry");
        require_correctness(cache.protected_size() == 1, "slru_cache 命中 probationary 后必须提升到 protected segment");

        cache.put(3, 30);
        cache.put(4, 40);
        require_correctness(cache.contains(1), "slru_cache protected entry 不应被 scan 型新写入优先淘汰");
        require_correctness(!cache.contains(2), "slru_cache capacity pressure 应淘汰 probationary LRU entry");
        require_correctness(cache.size() == 3, "slru_cache 不得突破 capacity");
    }));

    checks.push_back(run_correctness_check("slru_cache move-only get_ref", [] {
        cache_system::slru_cache<int, std::unique_ptr<int>> cache(2);
        cache.put(1, std::make_unique<int>(42));

        auto* value = cache.get_ref(1);
        require_correctness(value != nullptr && **value == 42, "slru_cache get_ref 必须支持 move-only Value");
        require_correctness(cache.protected_size() == 1, "slru_cache get_ref 命中后必须更新 segment 状态");
    }));

    checks.push_back(run_correctness_check("lru_cache put exception rollback", [] {
        cache_system::lru_cache<int, int, fault_injecting_hash> cache(4);
        cache.put(1, 10);

        fault_injecting_hash::reset(2, 2);
        bool threw = false;
        try {
            cache.put(2, 20);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        fault_injecting_hash::disable();

        require_correctness(threw, "lru_cache 必须传播 Hash 异常");
        require_correctness(cache.size() == 1, "lru_cache emplace 失败后不得留下未索引 list node");
        require_correctness(cache.get(1).value_or(0) == 10, "lru_cache emplace 失败后必须保留原有 entry");
        require_correctness(!cache.contains(2), "lru_cache emplace 失败后不得包含失败 key");
    }));

    checks.push_back(run_correctness_check("ttl_cache non-positive ttl", [] {
        cache_system::ttl_cache<int, int, manual_clock> cache(4);
        manual_clock::current = manual_clock::time_point(manual_clock::duration(0));

        require_correctness(!cache.put(1, 10, manual_clock::duration(0)), "ttl_cache ttl == 0 不应插入 entry");
        require_correctness(cache.size() == 0, "ttl_cache ttl == 0 后 size 必须保持不变");

        cache.put(2, 20, manual_clock::duration(100));
        require_correctness(!cache.put(2, 30, manual_clock::duration(-1)), "ttl_cache ttl < 0 应立即过期已有 entry");
        require_correctness(!cache.get(2).has_value(), "ttl_cache ttl < 0 后已有 entry 必须不可读");
    }));

    checks.push_back(run_correctness_check("ttl_cache expiration index exception rollback", [] {
        cache_system::ttl_cache<copy_throwing_key, int, manual_clock, copy_throwing_key_hash> cache(4);
        manual_clock::current = manual_clock::time_point(manual_clock::duration(0));

        copy_throwing_key::throw_on_copy = true;
        bool insert_threw = false;
        try {
            cache.put(copy_throwing_key{1}, 10, manual_clock::duration(100));
        } catch (const std::runtime_error&) {
            insert_threw = true;
        }
        copy_throwing_key::throw_on_copy = false;

        require_correctness(insert_threw, "ttl_cache 必须传播 expiration record copy 异常");
        require_correctness(cache.size() == 0, "ttl_cache expiration push 失败后必须回滚新 entry");

        cache.put(copy_throwing_key{2}, 20, manual_clock::duration(100));
        copy_throwing_key::throw_on_copy = true;
        bool update_threw = false;
        try {
            cache.put(copy_throwing_key{2}, 30, manual_clock::duration(200));
        } catch (const std::runtime_error&) {
            update_threw = true;
        }
        copy_throwing_key::throw_on_copy = false;

        require_correctness(update_threw, "ttl_cache 更新路径必须传播 expiration record copy 异常");
        require_correctness(cache.get(copy_throwing_key{2}).value_or(0) == 20, "ttl_cache 更新失败后必须保留旧 Value");
    }));

    checks.push_back(run_correctness_check("weighted_lru_cache overweight rejection", [] {
        cache_system::weighted_lru_cache<int, std::string, string_weight> cache(8);
        require_correctness(!cache.put(1, std::string(64, 'x')), "weighted_lru_cache 必须拒绝 overweight value");
        require_correctness(!cache.contains(1), "weighted_lru_cache 不得保留 overweight value");
        require_correctness(cache.stats().rejections == 1, "weighted_lru_cache 必须统计 rejections");
    }));

    checks.push_back(run_correctness_check("weighted_lru_cache put exception rollback", [] {
        cache_system::weighted_lru_cache<int, int, cache_system::unit_weigher<int>, fault_injecting_hash> cache(4);
        cache.put(1, 10);

        fault_injecting_hash::reset(2, 2);
        bool threw = false;
        try {
            cache.put(2, 20);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        fault_injecting_hash::disable();

        require_correctness(threw, "weighted_lru_cache 必须传播 Hash 异常");
        require_correctness(cache.size() == 1, "weighted_lru_cache emplace 失败后不得留下未索引 list node");
        require_correctness(cache.current_weight() == 1, "weighted_lru_cache emplace 失败后 current_weight 必须回滚");
        require_correctness(cache.get(1).value_or(0) == 10, "weighted_lru_cache emplace 失败后必须保留原有 entry");
        require_correctness(!cache.contains(2), "weighted_lru_cache emplace 失败后不得包含失败 key");
    }));

    checks.push_back(run_correctness_check("synchronized_lru_cache with_value", [] {
        cache_system::synchronized_lru_cache<int, std::string> cache(2);
        cache.put(1, "value");

        bool visited = cache.with_value(1, [](std::string& value) {
            value += "-updated";
        });

        require_correctness(visited, "synchronized_lru_cache with_value 必须访问已存在 entry");
        require_correctness(cache.get(1).value_or(std::string{}) == "value-updated", "with_value mutation 必须写回缓存中的 Value");
    }));

    return checks;
}

bool correctness_passed(const std::vector<correctness_check>& checks) {
    return std::all_of(checks.begin(), checks.end(), [](const correctness_check& check) {
        return check.passed;
    });
}

std::vector<int> shuffled_keys(std::size_t count, int offset = 0) {
    std::vector<int> keys(count);
    std::iota(keys.begin(), keys.end(), offset);
    std::mt19937 rng(0x5EED);
    std::shuffle(keys.begin(), keys.end(), rng);
    return keys;
}

benchmark_result unordered_map_hit_case(const std::vector<int>& keys) {
    std::unordered_map<int, int> map;
    map.reserve(keys.size());
    for (int key : keys) {
        map.emplace(key, key * 3);
    }
    auto body = [&] {
        std::uint64_t checksum = 0;
        for (int key : keys) {
            auto found = map.find(key);
            if (found != map.end()) {
                checksum += static_cast<std::uint64_t>(found->second);
            }
        }
        do_not_optimize(checksum);
    };
    return run_case("std::unordered_map hit lookup", keys.size(), body, body, "hit path 对照组");
}

benchmark_result lru_hit_case(const std::vector<int>& keys) {
    cache_system::lru_cache<int, int> cache(keys.size());
    for (int key : keys) {
        cache.put(key, key * 3);
    }
    auto body = [&] {
        std::uint64_t checksum = 0;
        for (int key : keys) {
            auto value = cache.get(key);
            if (value) {
                checksum += static_cast<std::uint64_t>(*value);
            }
        }
        do_not_optimize(checksum);
    };
    return run_case("lru_cache hit lookup", keys.size(), body, body, "更新 recency order");
}

benchmark_result unordered_map_miss_case(const std::vector<int>& keys, const std::vector<int>& misses) {
    std::unordered_map<int, int> map;
    map.reserve(keys.size());
    for (int key : keys) {
        map.emplace(key, key * 3);
    }
    auto body = [&] {
        std::size_t missing = 0;
        for (int key : misses) {
            missing += map.find(key) == map.end() ? 1 : 0;
        }
        do_not_optimize(missing);
    };
    return run_case("std::unordered_map miss lookup", misses.size(), body, body, "miss path 对照组");
}

benchmark_result lru_miss_case(const std::vector<int>& keys, const std::vector<int>& misses) {
    cache_system::lru_cache<int, int> cache(keys.size());
    for (int key : keys) {
        cache.put(key, key * 3);
    }
    auto body = [&] {
        std::size_t missing = 0;
        for (int key : misses) {
            missing += cache.get(key).has_value() ? 0 : 1;
        }
        do_not_optimize(missing);
    };
    return run_case("lru_cache miss lookup", misses.size(), body, body, "记录 misses");
}

benchmark_result lru_mixed_case(const std::vector<int>& keys) {
    constexpr std::size_t operations = 200'000;
    cache_system::lru_cache<int, int> cache(keys.size() / 2);
    for (std::size_t i = 0; i < keys.size() / 2; ++i) {
        cache.put(keys[i], keys[i]);
    }
    std::vector<int> stream(operations);
    std::mt19937 rng(0xA11CE);
    std::uniform_int_distribution<int> key_dist(0, static_cast<int>(keys.size() * 2));
    for (int& key : stream) {
        key = key_dist(rng);
    }
    auto body = [&] {
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < stream.size(); ++i) {
            if ((i % 10) == 0) {
                cache.put(stream[i], stream[i] * 7);
            } else {
                auto value = cache.get(stream[i]);
                checksum += value.value_or(0);
            }
        }
        do_not_optimize(checksum);
    };
    return run_case("lru_cache 90/10 read-write stream", operations, [] {}, body, "预生成 random stream");
}

benchmark_result slru_mixed_case(const std::vector<int>& keys) {
    constexpr std::size_t operations = 200'000;
    const std::size_t capacity = keys.size() / 2;
    cache_system::slru_cache<int, int> cache(capacity);
    for (std::size_t i = 0; i < capacity; ++i) {
        cache.put(keys[i], keys[i]);
    }

    std::vector<int> stream(operations);
    std::mt19937 rng(0x51A7E);
    std::uniform_int_distribution<int> key_dist(0, static_cast<int>(keys.size() * 2));
    for (int& key : stream) {
        key = key_dist(rng);
    }

    auto body = [&] {
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < stream.size(); ++i) {
            if ((i % 10) == 0) {
                cache.put(stream[i], stream[i] * 7);
            } else {
                auto value = cache.get(stream[i]);
                checksum += value.value_or(0);
            }
        }
        do_not_optimize(checksum);
    };
    return run_case("slru_cache 90/10 read-write stream", operations, [] {}, body, "probationary/protected segments");
}

benchmark_result ttl_expired_case(const std::vector<int>& keys) {
    cache_system::ttl_cache<int, int> cache(keys.size());
    for (int key : keys) {
        cache.put(key, key, std::chrono::nanoseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto body = [&] {
        std::size_t misses = 0;
        for (int key : keys) {
            misses += cache.get(key).has_value() ? 0 : 1;
        }
        do_not_optimize(misses);
    };
    return run_case("ttl_cache expired miss path", keys.size(), [] {}, body, "读取时清理 expired entries");
}

benchmark_result weighted_lru_case(const std::vector<int>& keys) {
    constexpr std::size_t operations = 120'000;
    cache_system::weighted_lru_cache<int, std::string, string_weight> cache(256 * 1024);
    std::vector<std::string> payloads;
    payloads.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        payloads.emplace_back(24 + (i % 192), static_cast<char>('a' + (i % 26)));
    }

    auto body = [&] {
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < operations; ++i) {
            const std::size_t index = i % keys.size();
            if ((i % 4) == 0) {
                cache.put(keys[index], payloads[index]);
            } else {
                auto value = cache.get(keys[index]);
                checksum += value ? value->size() : 0;
            }
        }
        do_not_optimize(checksum);
    };
    return run_case("weighted_lru_cache byte-budget stream", operations, [] {}, body, "按 value weight 淘汰");
}

template <typename Cache>
benchmark_result threaded_cache_case(std::string name, Cache& cache, const std::vector<int>& keys) {
    const std::size_t threads = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    constexpr std::size_t operations_per_thread = 80'000;
    std::atomic<std::size_t> ready = 0;
    std::atomic<bool> start = false;
    auto worker = [&](std::size_t thread_index) {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < operations_per_thread; ++i) {
            const int key = keys[(i + thread_index * 997) % keys.size()];
            if ((i % 16) == 0) {
                cache.put(key, key + static_cast<int>(i));
            } else {
                checksum += cache.get(key).value_or(0);
            }
        }
        do_not_optimize(checksum);
    };

    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        workers.emplace_back(worker, i);
    }
    while (ready.load(std::memory_order_acquire) != threads) {
        std::this_thread::yield();
    }
    const auto begin = clock_type::now();
    start.store(true, std::memory_order_release);
    for (auto& thread : workers) {
        thread.join();
    }
    const auto end = clock_type::now();
    const std::chrono::duration<double, std::milli> elapsed = end - begin;
    return {std::move(name), threads * operations_per_thread, elapsed.count(), std::to_string(threads) + " threads"};
}

void write_report(
    const std::vector<correctness_check>& checks,
    const std::vector<benchmark_result>& results,
    const cache_system::cache_stats* stats) {
    std::filesystem::path results_dir = CACHE_SYSTEM_RESULTS_DIR;
    std::filesystem::create_directories(results_dir);
    const auto file_stamp = timestamp_for_file();
    const auto report_path = results_dir / ("test-" + file_stamp + ".md");

    const bool passed = correctness_passed(checks);
    double best = results.empty() ? 0.0 : results.front().ns_per_op();
    for (const auto& result : results) {
        if (result.ns_per_op() > 0.0) {
            best = std::min(best, result.ns_per_op());
        }
    }

    std::ofstream report(report_path);
    report << "# CacheSystem 正确性与性能测试报告\n\n";
    report << "## 元数据\n\n";
    report << "- Library: CacheSystem " << cache_system::version() << '\n';
    report << "- Build: Release (`NDEBUG` defined)\n";
    report << "- Timestamp: " << timestamp_pretty() << '\n';
    report << "- Random seed: 0x5EED / 0xA11CE\n";
    report << "- Result file: " << report_path.generic_string() << "\n\n";

    report << "## 正确性测试\n\n";
    report << "| 用例 | 状态 | 详情 |\n";
    report << "| --- | --- | --- |\n";
    for (const auto& check : checks) {
        report << "| " << check.name
               << " | " << (check.passed ? "pass" : "fail")
               << " | " << check.detail << " |\n";
    }

    if (!passed) {
        report << "\n## 性能测试结果\n\n";
        report << "- 状态: skipped\n";
        report << "- 原因: correctness tests 未全部通过；为避免产生无效性能数据，已终止后续 benchmark。\n";
        std::cout << "Wrote " << report_path << '\n';
        return;
    }

    report << "\n## 性能测试结果\n\n";
    report << "| 用例 | 操作次数 | 总耗时 ms | ns/op | 图示 | 备注 |\n";
    report << "| --- | ---: | ---: | ---: | --- | --- |\n";
    for (const auto& result : results) {
        report << "| " << result.name
               << " | " << result.operations
               << " | " << std::fixed << std::setprecision(3) << result.total_ms
               << " | " << std::fixed << std::setprecision(2) << result.ns_per_op()
               << " | `" << bar(result.ns_per_op(), best) << "`"
               << " | " << result.notes << " |\n";
    }

    if (stats != nullptr) {
        report << "\n## cache_stats 快照\n\n";
        report << "- Hits: " << stats->hits << '\n';
        report << "- Misses: " << stats->misses << '\n';
        report << "- Hit rate: " << std::fixed << std::setprecision(4) << stats->hit_rate() << '\n';
        report << "- Inserts: " << stats->inserts << '\n';
        report << "- Updates: " << stats->updates << '\n';
        report << "- Evictions: " << stats->evictions << '\n';
        report << "- Expirations: " << stats->expirations << '\n';
        report << "- Rejections: " << stats->rejections << "\n\n";
    }

    report << "## 测试方法\n\n";
    report << "- 正确性测试在 benchmark 前执行；任何失败都会终止 benchmark。\n";
    report << "- hit、miss、mixed、SLRU、TTL-expiration、weighted eviction 和 concurrent paths 分别测量。\n";
    report << "- random streams 在计时前生成，并使用固定 seed 保证可重复。\n";
    report << "- 对照组使用 `std::unordered_map`，只比较语义相近的 lookup path。\n";
    report << "- `do_not_optimize` 和 compiler memory barrier 用于降低编译器过度优化风险。\n";

    std::cout << "Wrote " << report_path << '\n';
}

} // namespace

int main() {
    const auto correctness = run_correctness_tests();
    if (!correctness_passed(correctness)) {
        write_report(correctness, {}, nullptr);
        return 1;
    }

    const auto keys = shuffled_keys(65'536);
    const auto misses = shuffled_keys(65'536, 1'000'000);

    std::vector<benchmark_result> results;
    results.push_back(unordered_map_hit_case(keys));
    results.push_back(lru_hit_case(keys));
    results.push_back(unordered_map_miss_case(keys, misses));
    results.push_back(lru_miss_case(keys, misses));
    results.push_back(lru_mixed_case(keys));
    results.push_back(slru_mixed_case(keys));
    results.push_back(ttl_expired_case(keys));
    results.push_back(weighted_lru_case(keys));

    cache_system::synchronized_lru_cache<int, int> synchronized_cache(32'768);
    cache_system::sharded_lru_cache<int, int> sharded_cache(32'768, 16);
    for (std::size_t i = 0; i < 32'768; ++i) {
        synchronized_cache.put(keys[i], keys[i]);
        sharded_cache.put(keys[i], keys[i]);
    }
    results.push_back(threaded_cache_case("synchronized_lru_cache concurrent 15/1", synchronized_cache, keys));
    results.push_back(threaded_cache_case("sharded_lru_cache concurrent 15/1", sharded_cache, keys));

    const auto stats = sharded_cache.stats();
    write_report(correctness, results, &stats);
}
