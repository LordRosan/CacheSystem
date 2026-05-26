#if !defined(CACHE_SYSTEM_CORRECTNESS_ONLY) && !defined(NDEBUG)
#error "CacheSystem benchmark must be built in Release mode."
#endif

#include "cache_system/cache_system.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <list>
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

#ifndef CACHE_SYSTEM_RESULTS_DIR
#define CACHE_SYSTEM_RESULTS_DIR "results"
#endif

#if defined(CACHE_SYSTEM_TEST_ALL) || defined(CACHE_SYSTEM_TEST_UNIT) || !defined(CACHE_SYSTEM_CORRECTNESS_ONLY)
#define CACHE_SYSTEM_RUN_UNIT_TESTS 1
#endif

#if defined(CACHE_SYSTEM_TEST_ALL) || defined(CACHE_SYSTEM_TEST_STRESS) || !defined(CACHE_SYSTEM_CORRECTNESS_ONLY)
#define CACHE_SYSTEM_RUN_STRESS_TESTS 1
#endif

#if defined(CACHE_SYSTEM_TEST_ALL) || defined(CACHE_SYSTEM_TEST_RANDOMIZED) || !defined(CACHE_SYSTEM_CORRECTNESS_ONLY)
#define CACHE_SYSTEM_RUN_RANDOMIZED_TESTS 1
#endif

namespace {
#if !defined(CACHE_SYSTEM_CORRECTNESS_ONLY)
    using clock_type = std::chrono::steady_clock;
    constexpr std::size_t benchmark_sample_count = 7;

    [[nodiscard]] double median_value(std::vector<double> values) {
        if (values.empty()) {
            return 0.0;
        }

        std::sort(values.begin(), values.end());
        const std::size_t middle = values.size() / 2;
        if ((values.size() % 2) != 0) {
            return values[middle];
        }
        return (values[middle - 1] + values[middle]) / 2.0;
    }

    [[nodiscard]] double percentile_value(std::vector<double> values, std::size_t percentile) {
        if (values.empty()) {
            return 0.0;
        }

        std::sort(values.begin(), values.end());
        const std::size_t index = std::min(values.size() - 1, ((values.size() * percentile) + 99) / 100 - 1);
        return values[index];
    }

    struct benchmark_result {
        std::string group;
        std::string name;
        std::string parameters;
        std::size_t operations = 0;
        double total_ms = 0.0;
        double hit_rate = -1.0;
        std::vector<double> sample_ms;

        [[nodiscard]] double ns_per_op() const {
            return operations == 0 ? 0.0 : (median_ms() * 1'000'000.0) / static_cast<double>(operations);
        }

        [[nodiscard]] double ops_per_second() const {
            const double median = median_ms();
            return median <= 0.0 ? 0.0 : (static_cast<double>(operations) * 1000.0) / median;
        }

        [[nodiscard]] std::size_t sample_count() const noexcept {
            return sample_ms.empty() ? 1 : sample_ms.size();
        }

        [[nodiscard]] double median_ms() const {
            return sample_ms.empty() ? total_ms : median_value(sample_ms);
        }

        [[nodiscard]] double min_ms() const {
            if (sample_ms.empty()) {
                return total_ms;
            }
            return *std::min_element(sample_ms.begin(), sample_ms.end());
        }

        [[nodiscard]] double p95_ms() const {
            return sample_ms.empty() ? total_ms : percentile_value(sample_ms, 95);
        }

        [[nodiscard]] double min_ns_per_op() const {
            return operations == 0 ? 0.0 : (min_ms() * 1'000'000.0) / static_cast<double>(operations);
        }

        [[nodiscard]] double p95_ns_per_op() const {
            return operations == 0 ? 0.0 : (p95_ms() * 1'000'000.0) / static_cast<double>(operations);
        }

        [[nodiscard]] double stddev_ns_per_op() const {
            if (operations == 0 || sample_ms.size() <= 1) {
                return 0.0;
            }

            const double median = median_ms();
            double variance = 0.0;
            for (double sample: sample_ms) {
                const double delta = sample - median;
                variance += delta * delta;
            }
            variance /= static_cast<double>(sample_ms.size() - 1);
            return (std::sqrt(variance) * 1'000'000.0) / static_cast<double>(operations);
        }
    };
#endif

    struct correctness_check {
        std::string name;
        bool passed = false;
        std::string detail;
    };

#if !defined(CACHE_SYSTEM_CORRECTNESS_ONLY)
    template<typename T>
    void do_not_optimize(const T &value) {
        asm volatile("" : : "m"(value) : "memory");
    }

    void clobber_memory() {
        asm volatile("" : : : "memory");
    }

    template<typename Warmup, typename Work>
    benchmark_result run_case(
        std::string group,
        std::string name,
        std::string parameters,
        std::size_t operations,
        Warmup warmup,
        Work work,
        double hit_rate = -1.0) {
        warmup();
        std::vector<double> samples;
        samples.reserve(benchmark_sample_count);
        for (std::size_t sample = 0; sample < benchmark_sample_count; ++sample) {
            clobber_memory();
            const auto begin = clock_type::now();
            work();
            clobber_memory();
            const auto end = clock_type::now();
            const std::chrono::duration<double, std::milli> elapsed = end - begin;
            samples.push_back(elapsed.count());
        }
        const double median_ms = median_value(samples);
        return {std::move(group), std::move(name), std::move(parameters), operations, median_ms, hit_rate, std::move(samples)};
    }

    std::string timestamp_for_file() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t raw = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &raw);
        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%d-%H-%M-%S");
        return out.str();
    }

    std::string timestamp_pretty() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t raw = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &raw);
        std::ostringstream out;
        out << std::put_time(&tm, "%Y/%m/%d-%H/%M/%S");
        return out.str();
    }

#endif

    void require_correctness(bool condition, const char *message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    template<typename Func>
    correctness_check run_correctness_check(std::string name, Func &&func) {
        try {
            func();
            return {std::move(name), true, "通过"};
        } catch (const std::exception &error) {
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
        [[nodiscard]] std::size_t operator()(const std::string &value) const noexcept {
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

        explicit copy_throwing_key(int input = 0) noexcept : value(input) {
        }

        copy_throwing_key(const copy_throwing_key &other) : value(other.value) {
            if (throw_on_copy) {
                throw std::runtime_error("key copy failure");
            }
        }

        copy_throwing_key(copy_throwing_key &&other) noexcept = default;

        copy_throwing_key &operator=(const copy_throwing_key &) = default;

        copy_throwing_key &operator=(copy_throwing_key &&) noexcept = default;

        [[nodiscard]] friend bool operator==(const copy_throwing_key &left, const copy_throwing_key &right) noexcept {
            return left.value == right.value;
        }
    };

    struct copy_throwing_key_hash {
        [[nodiscard]] std::size_t operator()(const copy_throwing_key &key) const noexcept {
            return std::hash<int>{}(key.value);
        }
    };

    struct move_assign_throwing_value {
        int value = 0;
        static inline bool throw_on_move_assign = false;

        explicit move_assign_throwing_value(int input = 0) noexcept : value(input) {
        }

        move_assign_throwing_value(const move_assign_throwing_value &) = default;

        move_assign_throwing_value(move_assign_throwing_value &&) noexcept = default;

        move_assign_throwing_value &operator=(const move_assign_throwing_value &) = default;

        move_assign_throwing_value &operator=(move_assign_throwing_value &&other) {
            if (throw_on_move_assign) {
                throw std::runtime_error("value move assignment failure");
            }
            value = other.value;
            return *this;
        }
    };

    std::vector<correctness_check> run_correctness_tests() {
        std::vector<correctness_check> checks;

#if defined(CACHE_SYSTEM_RUN_UNIT_TESTS)
        checks.push_back(run_correctness_check("sharded_lru_cache exact capacity", [] {
            cache_system::sharded_lru_cache<int, int> cache(17, 16);
            for (int i = 0; i < 100; ++i) {
                cache.put(i, i);
            }

            require_correctness(cache.capacity() == 17, "sharded_lru_cache 必须报告用户请求的 capacity");
            require_correctness(cache.size() <= 17, "sharded_lru_cache 不得突破用户请求的总 capacity");
            require_correctness(cache.stats().evictions > 0, "sharded_lru_cache 超过 capacity 后必须产生 eviction");
        }));

        checks.push_back(run_correctness_check("cache constructors reject invalid capacity", [] {
            bool lru_rejected = false;
            bool ttl_rejected = false;
            bool slru_rejected = false;
            bool sharded_rejected = false;
            bool weighted_rejected = false;
            bool tinylfu_rejected = false;
            try {
                cache_system::lru_cache<int, int> cache(0);
            } catch (const std::invalid_argument &) {
                lru_rejected = true;
            }
            try {
                cache_system::ttl_cache<int, int, manual_clock> cache(0);
            } catch (const std::invalid_argument &) {
                ttl_rejected = true;
            }
            try {
                cache_system::slru_cache<int, int> cache(2, 3);
            } catch (const std::invalid_argument &) {
                slru_rejected = true;
            }
            try {
                cache_system::sharded_lru_cache<int, int> cache(0);
            } catch (const std::invalid_argument &) {
                sharded_rejected = true;
            }
            try {
                cache_system::weighted_lru_cache<int, int> cache(0);
            } catch (const std::invalid_argument &) {
                weighted_rejected = true;
            }
            try {
                cache_system::tinylfu_cache<int, int> cache(1, 0);
            } catch (const std::invalid_argument &) {
                tinylfu_rejected = true;
            }

            require_correctness(lru_rejected, "lru_cache 必须拒绝 capacity == 0");
            require_correctness(ttl_rejected, "ttl_cache 必须拒绝 capacity == 0");
            require_correctness(slru_rejected, "slru_cache 必须拒绝 protected_capacity > capacity");
            require_correctness(sharded_rejected, "sharded_lru_cache 必须拒绝 capacity == 0");
            require_correctness(weighted_rejected, "weighted_lru_cache 必须拒绝 max_weight == 0");
            require_correctness(tinylfu_rejected, "tinylfu_cache 必须拒绝 sketch_width == 0");
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
            cache_system::lru_cache<int, std::unique_ptr<int> > cache(2);
            cache.put(1, std::make_unique<int>(42));

            auto *value = cache.get_ref(1);
            require_correctness(value != nullptr && **value == 42, "lru_cache get_ref 必须支持 move-only Value");

            auto *peeked = cache.peek_ref(1);
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
            cache_system::slru_cache<int, std::unique_ptr<int> > cache(2);
            cache.put(1, std::make_unique<int>(42));

            auto *value = cache.get_ref(1);
            require_correctness(value != nullptr && **value == 42, "slru_cache get_ref 必须支持 move-only Value");
            require_correctness(cache.protected_size() == 1, "slru_cache get_ref 命中后必须更新 segment 状态");
        }));

        checks.push_back(run_correctness_check("tinylfu_cache rejects low-frequency scan", [] {
            cache_system::tinylfu_cache<int, int> cache(2, 128, 10'000);
            cache.put(1, 10);
            for (int i = 0; i < 16; ++i) {
                require_correctness(cache.get(1).value_or(0) == 10, "tinylfu_cache 必须保留热点 entry");
            }
            cache.put(2, 20);

            require_correctness(!cache.put(3, 30), "tinylfu_cache 应拒绝低频 scan candidate");
            require_correctness(cache.contains(1), "tinylfu_cache 不应让低频 candidate 淘汰热点 victim");
            require_correctness(cache.contains(2), "tinylfu_cache 拒绝 candidate 时不应改变现有 entry");
            require_correctness(cache.stats().rejections == 1, "tinylfu_cache 必须统计 admission rejections");
        }));

        checks.push_back(run_correctness_check("tinylfu_cache admits warmed candidate", [] {
            cache_system::tinylfu_cache<int, int> cache(1, 128, 10'000);
            cache.put(1, 10);
            for (int i = 0; i < 8; ++i) {
                (void) cache.get(2);
            }

            require_correctness(cache.put(2, 20), "tinylfu_cache 应接纳已被访问预热的 candidate");
            require_correctness(!cache.contains(1), "tinylfu_cache 接纳 candidate 后必须淘汰旧 victim");
            require_correctness(cache.get(2).value_or(0) == 20, "tinylfu_cache 接纳后的 candidate 必须可读");
        }));

        checks.push_back(run_correctness_check("tinylfu_cache reset frequencies", [] {
            cache_system::tinylfu_cache<int, int> cache(2, 128, 10'000);
            for (int i = 0; i < 8; ++i) {
                (void) cache.get(1);
            }
            require_correctness(cache.estimate_frequency(1) > 0, "tinylfu_cache 必须记录访问频率");
            cache.reset_frequencies();
            require_correctness(cache.estimate_frequency(1) == 0, "tinylfu_cache reset_frequencies 必须清空 sketch");
        }));

        checks.push_back(run_correctness_check("tinylfu_cache move-only get_ref", [] {
            cache_system::tinylfu_cache<int, std::unique_ptr<int> > cache(2);
            cache.put(1, std::make_unique<int>(42));

            auto *value = cache.get_ref(1);
            require_correctness(value != nullptr && **value == 42, "tinylfu_cache get_ref 必须支持 move-only Value");
        }));

        checks.push_back(run_correctness_check("lru_cache put exception rollback", [] {
            cache_system::lru_cache<int, int, fault_injecting_hash> cache(4);
            cache.put(1, 10);

            fault_injecting_hash::reset(2, 2);
            bool threw = false;
            try {
                cache.put(2, 20);
            } catch (const std::runtime_error &) {
                threw = true;
            }
            fault_injecting_hash::disable();

            require_correctness(threw, "lru_cache 必须传播 Hash 异常");
            require_correctness(cache.size() == 1, "lru_cache emplace 失败后不得留下未索引 list node");
            require_correctness(cache.get(1).value_or(0) == 10, "lru_cache emplace 失败后必须保留原有 entry");
            require_correctness(!cache.contains(2), "lru_cache emplace 失败后不得包含失败 key");
        }));

        checks.push_back(run_correctness_check("cache_stats saturation", [] {
            cache_system::cache_stats left;
            cache_system::cache_stats right;
            left.hits = std::numeric_limits<std::size_t>::max();
            left.misses = std::numeric_limits<std::size_t>::max() - 1;
            right.hits = 1;
            right.misses = 4;
            left += right;

            require_correctness(left.hits == std::numeric_limits<std::size_t>::max(), "cache_stats hits 必须饱和而非回绕");
            require_correctness(left.misses == std::numeric_limits<std::size_t>::max(), "cache_stats misses 必须饱和而非回绕");
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
            } catch (const std::runtime_error &) {
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
            } catch (const std::runtime_error &) {
                update_threw = true;
            }
            copy_throwing_key::throw_on_copy = false;

            require_correctness(update_threw, "ttl_cache 更新路径必须传播 expiration record copy 异常");
            require_correctness(cache.get(copy_throwing_key{2}).value_or(0) == 20, "ttl_cache 更新失败后必须保留旧 Value");
        }));

        checks.push_back(run_correctness_check("ttl_cache update assignment exception generation isolation", [] {
            cache_system::ttl_cache<int, move_assign_throwing_value, manual_clock> cache(4);
            manual_clock::current = manual_clock::time_point(manual_clock::duration(0));
            cache.put(1, move_assign_throwing_value{10}, manual_clock::duration(100));

            manual_clock::current = manual_clock::time_point(manual_clock::duration(10));
            move_assign_throwing_value::throw_on_move_assign = true;
            bool update_threw = false;
            try {
                cache.put(1, move_assign_throwing_value{20}, manual_clock::duration(1));
            } catch (const std::runtime_error &) {
                update_threw = true;
            }
            move_assign_throwing_value::throw_on_move_assign = false;

            require_correctness(update_threw, "ttl_cache 必须传播 Value assignment 异常");
            cache.put(1, move_assign_throwing_value{30}, manual_clock::duration(100));

            manual_clock::current = manual_clock::time_point(manual_clock::duration(12));
            auto *value = cache.get_ref(1);
            require_correctness(value != nullptr && value->value == 30,
                                "ttl_cache Value assignment 失败留下的 expiration record 不得污染后续 generation");
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
            } catch (const std::runtime_error &) {
                threw = true;
            }
            fault_injecting_hash::disable();

            require_correctness(threw, "weighted_lru_cache 必须传播 Hash 异常");
            require_correctness(cache.size() == 1, "weighted_lru_cache emplace 失败后不得留下未索引 list node");
            require_correctness(cache.current_weight() == 1, "weighted_lru_cache emplace 失败后 current_weight 必须回滚");
            require_correctness(cache.get(1).value_or(0) == 10, "weighted_lru_cache emplace 失败后必须保留原有 entry");
            require_correctness(!cache.contains(2), "weighted_lru_cache emplace 失败后不得包含失败 key");

            cache.put(2, 20);
            cache.put(3, 30);
            cache.put(4, 40);
            fault_injecting_hash::reset(5, 2);
            threw = false;
            try {
                cache.put(5, 50);
            } catch (const std::runtime_error &) {
                threw = true;
            }
            fault_injecting_hash::disable();

            require_correctness(threw, "weighted_lru_cache 满容量 emplace 失败必须传播 Hash 异常");
            require_correctness(cache.size() == 4, "weighted_lru_cache 满容量 emplace 失败不得提前 eviction");
            require_correctness(cache.current_weight() == 4, "weighted_lru_cache 满容量 emplace 失败不得改变 current_weight");
            require_correctness(cache.contains(1) && cache.contains(2) && cache.contains(3) && cache.contains(4),
                                "weighted_lru_cache 满容量 emplace 失败必须保留原有 entries");
            require_correctness(!cache.contains(5), "weighted_lru_cache 满容量 emplace 失败不得包含失败 key");
        }));

        checks.push_back(run_correctness_check("synchronized_lru_cache with_value", [] {
            cache_system::synchronized_lru_cache<int, std::string> cache(2);
            cache.put(1, "value");

            bool visited = cache.with_value(1, [](std::string &value) {
                value += "-updated";
            });

            require_correctness(visited, "synchronized_lru_cache with_value 必须访问已存在 entry");
            require_correctness(cache.get(1).value_or(std::string{}) == "value-updated",
                                "with_value mutation 必须写回缓存中的 Value");
        }));

        checks.push_back(run_correctness_check("sharded_lru_cache clear", [] {
            cache_system::sharded_lru_cache<int, int> cache(16, 4);
            for (int i = 0; i < 32; ++i) {
                cache.put(i, i);
            }

            cache.clear();
            require_correctness(cache.size() == 0, "sharded_lru_cache clear 必须清空所有 shards");
            require_correctness(!cache.contains(1), "sharded_lru_cache clear 后不得保留旧 key");
        }));
#endif

#if defined(CACHE_SYSTEM_RUN_STRESS_TESTS)
        checks.push_back(run_correctness_check("lru_cache sustained capacity stress", [] {
            constexpr std::size_t capacity = 256;
            cache_system::lru_cache<int, int> cache(capacity);
            for (int i = 0; i < 80'000; ++i) {
                cache.put(i % 1024, i);
                (void) cache.get((i * 17) % 1024);
                require_correctness(cache.size() <= capacity, "lru_cache stress 不得突破 capacity");
            }

            const auto stats = cache.stats();
            require_correctness(stats.inserts > 0, "lru_cache stress 必须产生 inserts");
            require_correctness(stats.evictions > 0, "lru_cache stress 必须产生 evictions");
            require_correctness(stats.hits + stats.misses > 0, "lru_cache stress 必须覆盖 lookup path");
        }));

        checks.push_back(run_correctness_check("sharded_lru_cache concurrent stress", [] {
            cache_system::sharded_lru_cache<int, int> cache(1024, 8);
            constexpr std::size_t thread_count = 4;
            constexpr std::size_t operations_per_thread = 30'000;
            std::atomic<std::size_t> failures = 0;
            std::vector<std::thread> threads;
            threads.reserve(thread_count);

            for (std::size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
                threads.emplace_back([&, thread_index] {
                    for (std::size_t i = 0; i < operations_per_thread; ++i) {
                        const int key = static_cast<int>((i + thread_index * 4099) % 4096);
                        if ((i % 5) == 0) {
                            cache.put(key, static_cast<int>(i));
                        } else {
                            (void) cache.get(key);
                        }
                        if (cache.size() > cache.capacity()) {
                            failures.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                });
            }

            for (auto &thread: threads) {
                thread.join();
            }

            require_correctness(failures.load(std::memory_order_relaxed) == 0, "sharded_lru_cache stress 不得突破 capacity");
            require_correctness(cache.size() <= cache.capacity(), "sharded_lru_cache stress 结束后 size 必须不超过 capacity");
            require_correctness(cache.stats().hits + cache.stats().misses > 0, "sharded_lru_cache stress 必须覆盖 lookup path");
        }));
#endif

#if defined(CACHE_SYSTEM_RUN_RANDOMIZED_TESTS)
        checks.push_back(run_correctness_check("lru_cache randomized model", [] {
            constexpr std::size_t capacity = 32;
            cache_system::lru_cache<int, int> cache(capacity);
            std::list<int> recency;
            struct model_entry {
                int value = 0;
                std::list<int>::iterator position{};
            };
            std::unordered_map<int, model_entry> model;
            std::mt19937 rng(0xCA11CE);
            std::uniform_int_distribution<int> action_dist(0, 99);
            std::uniform_int_distribution<int> key_dist(0, 127);

            auto touch_model = [&](int key) {
                auto found = model.find(key);
                require_correctness(found != model.end(), "model touch 必须命中已有 key");
                recency.erase(found->second.position);
                recency.push_front(key);
                found->second.position = recency.begin();
            };

            auto put_model = [&](int key, int value) {
                auto found = model.find(key);
                if (found != model.end()) {
                    found->second.value = value;
                    touch_model(key);
                    return;
                }

                recency.push_front(key);
                model.emplace(key, model_entry{value, recency.begin()});
                if (model.size() > capacity) {
                    const int victim = recency.back();
                    recency.pop_back();
                    model.erase(victim);
                }
            };

            auto erase_model = [&](int key) -> bool {
                auto found = model.find(key);
                if (found == model.end()) {
                    return false;
                }
                recency.erase(found->second.position);
                model.erase(found);
                return true;
            };

            for (int step = 0; step < 20'000; ++step) {
                const int action = action_dist(rng);
                const int key = key_dist(rng);
                if (action < 45) {
                    const int value = step * 3;
                    cache.put(key, value);
                    put_model(key, value);
                } else if (action < 85) {
                    const auto actual = cache.get(key);
                    const auto expected = model.find(key);
                    if (expected == model.end()) {
                        require_correctness(!actual.has_value(), "randomized model miss 必须与 lru_cache 一致");
                    } else {
                        require_correctness(actual.has_value() && *actual == expected->second.value,
                                            "randomized model hit value 必须与 lru_cache 一致");
                        touch_model(key);
                    }
                } else {
                    const bool actual = cache.erase(key);
                    const bool expected = erase_model(key);
                    require_correctness(actual == expected, "randomized model erase 结果必须与 lru_cache 一致");
                }

                require_correctness(cache.size() == model.size(), "randomized model size 必须与 lru_cache 一致");
                require_correctness(cache.size() <= capacity, "randomized model 不得突破 capacity");
            }
        }));
#endif

        return checks;
    }

    bool correctness_passed(const std::vector<correctness_check> &checks) {
        return std::all_of(checks.begin(), checks.end(), [](const correctness_check &check) {
            return check.passed;
        });
    }

#if defined(CACHE_SYSTEM_CORRECTNESS_ONLY)
    int run_correctness_main() {
        const auto checks = run_correctness_tests();
        for (const auto &check: checks) {
            std::cout << (check.passed ? "[pass] " : "[fail] ") << check.name << " - " << check.detail << '\n';
        }
        return correctness_passed(checks) ? 0 : 1;
    }
#endif

#if !defined(CACHE_SYSTEM_CORRECTNESS_ONLY)
    std::vector<int> shuffled_keys(std::size_t count, int offset = 0) {
        std::vector<int> keys(count);
        std::iota(keys.begin(), keys.end(), offset);
        std::mt19937 rng(0x5EED);
        std::shuffle(keys.begin(), keys.end(), rng);
        return keys;
    }

    benchmark_result unordered_map_hit_case(const std::vector<int> &keys) {
        std::unordered_map<int, int> map;
        map.reserve(keys.size());
        for (int key: keys) {
            map.emplace(key, key * 3);
        }
        auto body = [&] {
            std::uint64_t checksum = 0;
            for (int key: keys) {
                auto found = map.find(key);
                if (found != map.end()) {
                    checksum += static_cast<std::uint64_t>(found->second);
                }
            }
            do_not_optimize(checksum);
        };
        return run_case(
            "lookup hit path",
            "std::unordered_map hit lookup",
            "keys=65536; hit_rate=100%; operation=find",
            keys.size(),
            body,
            body,
            1.0);
    }

    benchmark_result lru_hit_case(const std::vector<int> &keys) {
        cache_system::lru_cache<int, int> cache(keys.size());
        for (int key: keys) {
            cache.put(key, key * 3);
        }
        auto body = [&] {
            std::uint64_t checksum = 0;
            for (int key: keys) {
                auto value = cache.get(key);
                if (value) {
                    checksum += static_cast<std::uint64_t>(*value);
                }
            }
            do_not_optimize(checksum);
        };
        return run_case(
            "lookup hit path",
            "lru_cache hit lookup",
            "capacity=65536; hit_rate=100%; updates=recency",
            keys.size(),
            body,
            body,
            1.0);
    }

    benchmark_result unordered_map_miss_case(const std::vector<int> &keys, const std::vector<int> &misses) {
        std::unordered_map<int, int> map;
        map.reserve(keys.size());
        for (int key: keys) {
            map.emplace(key, key * 3);
        }
        auto body = [&] {
            std::size_t missing = 0;
            for (int key: misses) {
                missing += map.find(key) == map.end() ? 1 : 0;
            }
            do_not_optimize(missing);
        };
        return run_case(
            "lookup miss path",
            "std::unordered_map miss lookup",
            "keys=65536; misses=65536; hit_rate=0%; operation=find",
            misses.size(),
            body,
            body,
            0.0);
    }

    benchmark_result lru_miss_case(const std::vector<int> &keys, const std::vector<int> &misses) {
        cache_system::lru_cache<int, int> cache(keys.size());
        for (int key: keys) {
            cache.put(key, key * 3);
        }
        auto body = [&] {
            std::size_t missing = 0;
            for (int key: misses) {
                missing += cache.get(key).has_value() ? 0 : 1;
            }
            do_not_optimize(missing);
        };
        return run_case(
            "lookup miss path",
            "lru_cache miss lookup",
            "capacity=65536; misses=65536; hit_rate=0%",
            misses.size(),
            body,
            body,
            0.0);
    }

    benchmark_result lru_mixed_case(const std::vector<int> &keys) {
        constexpr std::size_t operations = 200'000;
        cache_system::lru_cache<int, int> cache(keys.size() / 2);
        for (std::size_t i = 0; i < keys.size() / 2; ++i) {
            cache.put(keys[i], keys[i]);
        }
        std::vector<int> stream(operations);
        std::mt19937 rng(0xA11CE);
        std::uniform_int_distribution<int> key_dist(0, static_cast<int>(keys.size() * 2));
        for (int &key: stream) {
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
        auto result = run_case(
            "mixed read-write stream",
            "lru_cache 90/10 read-write stream",
            "capacity=32768; operations=200000; reads=90%; writes=10%; key_space=131072",
            operations,
            [] {
            },
            body);
        result.hit_rate = cache.stats().hit_rate();
        return result;
    }

    benchmark_result slru_mixed_case(const std::vector<int> &keys) {
        constexpr std::size_t operations = 200'000;
        const std::size_t capacity = keys.size() / 2;
        cache_system::slru_cache<int, int> cache(capacity);
        for (std::size_t i = 0; i < capacity; ++i) {
            cache.put(keys[i], keys[i]);
        }

        std::vector<int> stream(operations);
        std::mt19937 rng(0x51A7E);
        std::uniform_int_distribution<int> key_dist(0, static_cast<int>(keys.size() * 2));
        for (int &key: stream) {
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
        auto result = run_case(
            "mixed read-write stream",
            "slru_cache 90/10 read-write stream",
            "capacity=32768; operations=200000; reads=90%; writes=10%; key_space=131072",
            operations,
            [] {
            },
            body);
        result.hit_rate = cache.stats().hit_rate();
        return result;
    }

    benchmark_result tinylfu_scan_case(const std::vector<int> &keys) {
        constexpr std::size_t operations = 200'000;
        constexpr std::size_t hot_keys = 1024;
        cache_system::tinylfu_cache<int, int> cache(2048, 8192);
        for (std::size_t i = 0; i < hot_keys; ++i) {
            cache.put(keys[i], keys[i]);
        }
        for (int round = 0; round < 8; ++round) {
            for (std::size_t i = 0; i < hot_keys; ++i) {
                (void) cache.get(keys[i]);
            }
        }

        auto body = [&] {
            std::uint64_t checksum = 0;
            int scan_key = 1'000'000;
            for (std::size_t i = 0; i < operations; ++i) {
                if ((i % 4) == 0) {
                    cache.put(scan_key++, static_cast<int>(i));
                } else {
                    auto value = cache.get(keys[i % hot_keys]);
                    checksum += value.value_or(0);
                }
            }
            checksum += cache.stats().rejections;
            do_not_optimize(checksum);
        };
        auto result = run_case(
            "admission policy",
            "tinylfu_cache scan-resistant admission",
            "capacity=2048; hot_keys=1024; operations=200000; writes=25%; sketch_width=8192",
            operations,
            [] {
            },
            body);
        result.hit_rate = cache.stats().hit_rate();
        return result;
    }

    benchmark_result ttl_expired_case(const std::vector<int> &keys) {
        cache_system::ttl_cache<int, int> cache(keys.size());
        for (int key: keys) {
            cache.put(key, key, std::chrono::nanoseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto body = [&] {
            std::size_t misses = 0;
            for (int key: keys) {
                misses += cache.get(key).has_value() ? 0 : 1;
            }
            do_not_optimize(misses);
        };
        auto result = run_case(
            "expiration path",
            "ttl_cache expired miss path",
            "capacity=65536; ttl=1ns; sleep=1ms; expected_hit_rate=0%",
            keys.size(),
            [] {
            },
            body);
        result.hit_rate = cache.stats().hit_rate();
        return result;
    }

    benchmark_result weighted_lru_case(const std::vector<int> &keys) {
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
        auto result = run_case(
            "weighted eviction",
            "weighted_lru_cache byte-budget stream",
            "max_weight=262144B; operations=120000; reads=75%; writes=25%; value=24..215B",
            operations,
            [] {
            },
            body);
        result.hit_rate = cache.stats().hit_rate();
        return result;
    }

    template<typename Measure>
    benchmark_result run_measured_case(
        std::string group,
        std::string name,
        std::string parameters,
        std::size_t operations,
        Measure measure,
        double hit_rate = -1.0) {
        std::vector<double> samples;
        samples.reserve(benchmark_sample_count);
        for (std::size_t sample = 0; sample < benchmark_sample_count; ++sample) {
            clobber_memory();
            samples.push_back(measure());
            clobber_memory();
        }
        const double median_ms = median_value(samples);
        return {std::move(group), std::move(name), std::move(parameters), operations, median_ms, hit_rate, std::move(samples)};
    }

    template<typename Cache>
    benchmark_result threaded_cache_case(std::string name, Cache &cache, const std::vector<int> &keys) {
        const std::size_t threads = std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
        constexpr std::size_t operations_per_thread = 80'000;
        auto measure = [&] {
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
            for (auto &thread: workers) {
                thread.join();
            }
            const auto end = clock_type::now();
            const std::chrono::duration<double, std::milli> elapsed = end - begin;
            return elapsed.count();
        };
        auto result = run_measured_case(
            "concurrent cache access",
            std::move(name),
            "threads=" + std::to_string(threads) + "; operations/thread=80000; reads=15/16; writes=1/16",
            threads * operations_per_thread,
            measure);
        result.hit_rate = cache.stats().hit_rate();
        return result;
    }

    std::unordered_map<std::string, double> load_baseline(const std::filesystem::path &path) {
        std::unordered_map<std::string, double> baseline;
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            const std::size_t delimiter = line.find('\t');
            if (delimiter == std::string::npos) {
                continue;
            }
            try {
                baseline.emplace(line.substr(0, delimiter), std::stod(line.substr(delimiter + 1)));
            } catch (...) {
            }
        }
        return baseline;
    }

    void save_baseline(const std::filesystem::path &path, const std::vector<benchmark_result> &results) {
        std::ofstream output(path);
        for (const auto &result: results) {
            output << result.name << '\t' << std::fixed << std::setprecision(4) << result.ns_per_op() << '\n';
        }
    }

    std::vector<const benchmark_result *> best_results_by_group(const std::vector<benchmark_result> &results) {
        std::unordered_map<std::string, std::size_t> positions;
        std::vector<const benchmark_result *> best;
        for (const auto &result: results) {
            const auto [position, inserted] = positions.emplace(result.group, best.size());
            if (inserted) {
                best.push_back(&result);
                continue;
            }

            const double candidate_ns = result.ns_per_op();
            const double current_ns = best[position->second]->ns_per_op();
            if (candidate_ns > 0.0 && (current_ns <= 0.0 || candidate_ns < current_ns)) {
                best[position->second] = &result;
            }
        }
        return best;
    }

    std::unordered_map<std::string, double>
    best_ns_by_group(const std::vector<const benchmark_result *> &best_results) {
        std::unordered_map<std::string, double> best;
        for (const benchmark_result *result: best_results) {
            best.emplace(result->group, result->ns_per_op());
        }
        return best;
    }

    std::string format_hit_rate(double hit_rate) {
        if (hit_rate < 0.0) {
            return "-";
        }

        std::ostringstream output;
        output << std::fixed << std::setprecision(2) << hit_rate * 100.0 << "%";
        return output.str();
    }

    std::string baseline_remark(double ratio, double delta_ns) {
        if (delta_ns <= 0.0) {
            return "当前结果未慢于 baseline。";
        }
        if (ratio <= 1.15) {
            return "相对变化未超过 15% 阈值。";
        }
        if (delta_ns <= 1.0) {
            return "相对变化较大但绝对差值不超过 1ns，视为正常抖动。";
        }
        return "相对退化超过 15%，且绝对退化超过 1ns，建议重复运行确认。";
    }

    void write_baseline_comparison(
        std::ofstream &report,
        const std::filesystem::path &results_dir,
        const std::vector<benchmark_result> &results) {
        if (results.empty()) {
            return;
        }

        const auto baseline_path = results_dir / "performance-baseline.tsv";
        auto baseline = load_baseline(baseline_path);

        report << "\n## 性能基线对比\n\n";
        if (baseline.empty()) {
            save_baseline(baseline_path, results);
            report << "- Baseline: 不存在，已使用本次结果初始化 `" << baseline_path.generic_string() << "`。\n";
            return;
        }

        report << "| 用例 | baseline median ns/op | current median ns/op | Δ ns/op | Δ % | 状态 | 备注 |\n";
        report << "| --- | ---: | ---: | ---: | ---: | --- | --- |\n";
        for (const auto &result: results) {
            const auto found = baseline.find(result.name);
            if (found == baseline.end() || found->second <= 0.0) {
                report << "| " << result.name << " | - | " << std::fixed << std::setprecision(2) << result.ns_per_op()
                        << " | - | - | new | baseline 中没有该用例。 |\n";
                continue;
            }
            const double ratio = result.ns_per_op() / found->second;
            const double delta = result.ns_per_op() - found->second;
            const double change_percent = (ratio - 1.0) * 100.0;
            const char *status = ratio > 1.15 && delta > 1.0 ? "watch" : "ok";
            report << "| " << result.name
                    << " | " << std::fixed << std::setprecision(2) << found->second
                    << " | " << std::fixed << std::setprecision(2) << result.ns_per_op()
                    << " | " << std::fixed << std::setprecision(2) << delta
                    << " | " << std::fixed << std::setprecision(1) << change_percent << "%"
                    << " | " << status
                    << " | " << baseline_remark(ratio, delta) << " |\n";
        }
    }

    void write_report(
        const std::vector<correctness_check> &checks,
        const std::vector<benchmark_result> &results,
        const cache_system::cache_stats *stats) {
        std::filesystem::path results_dir = CACHE_SYSTEM_RESULTS_DIR;
        std::filesystem::create_directories(results_dir);
        const auto file_stamp = timestamp_for_file();
        const auto report_path = results_dir / ("test-" + file_stamp + ".md");
        const bool passed = correctness_passed(checks);

        std::ofstream report(report_path);
        report << "# CacheSystem 正确性与性能测试报告\n\n";
        report << "## 元数据\n\n";
        report << "- Library: CacheSystem " << cache_system::version() << '\n';
        report << "- Build: Release (`NDEBUG` defined)\n";
        report << "- Timestamp: " << timestamp_pretty() << '\n';
        report << "- Random seed: 0x5EED / 0xA11CE / 0x51A7E\n";
        report << "- Benchmark samples: " << benchmark_sample_count << '\n';
        report << "- Result file: " << report_path.generic_string() << "\n\n";

        report << "## 正确性测试\n\n";
        report << "| 用例 | 状态 | 详情 |\n";
        report << "| --- | --- | --- |\n";
        for (const auto &check: checks) {
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
        const auto group_bests = best_results_by_group(results);
        const auto group_best_ns = best_ns_by_group(group_bests);

        report << "`相对组内 best` 使用同一 `组别` 中最低 `ns/op` 作为 1.00x，比较指标只看 `ns/op`；`命中率` 用于解释 cache 语义下的性能结果。\n\n";
        report << "| 组别 | best 用例 | best median ns/op | 比较指标 |\n";
        report << "| --- | --- | ---: | --- |\n";
        for (const benchmark_result *best: group_bests) {
            report << "| " << best->group
                    << " | " << best->name
                    << " | " << std::fixed << std::setprecision(2) << best->ns_per_op()
                    << " | ns/op |\n";
        }

        report << "\n| 组别 | 用例 | 测试参数 | 操作次数/样本 | 样本数 | median ms | min ns/op | median ns/op | p95 ns/op | stddev ns/op | ops/s | 命中率 | 相对组内 best |\n";
        report << "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n";
        for (const auto &result: results) {
            const auto best = group_best_ns.find(result.group);
            const double relative_best = best != group_best_ns.end() && best->second > 0.0 && result.ns_per_op() > 0.0
                                             ? result.ns_per_op() / best->second
                                             : 0.0;
            report << "| " << result.group
                    << " | " << result.name
                    << " | " << result.parameters
                    << " | " << result.operations
                    << " | " << result.sample_count()
                    << " | " << std::fixed << std::setprecision(3) << result.median_ms()
                    << " | " << std::fixed << std::setprecision(2) << result.min_ns_per_op()
                    << " | " << std::fixed << std::setprecision(2) << result.ns_per_op()
                    << " | " << std::fixed << std::setprecision(2) << result.p95_ns_per_op()
                    << " | " << std::fixed << std::setprecision(2) << result.stddev_ns_per_op()
                    << " | " << std::fixed << std::setprecision(0) << result.ops_per_second()
                    << " | " << format_hit_rate(result.hit_rate)
                    << " | " << std::fixed << std::setprecision(2) << relative_best << "x"
                    << " |\n";
        }

        write_baseline_comparison(report, results_dir, results);

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
        report << "- hit、miss、mixed、SLRU、TinyLFU admission、TTL-expiration、weighted eviction 和 concurrent paths 分别测量。\n";
        report << "- 每个 benchmark case 采集多个样本，报告使用 median 作为 baseline 对比和 `相对组内 best` 的主指标。\n";
        report << "- random streams 在计时前生成，并使用固定 seed 保证可重复。\n";
        report << "- 对照组使用 `std::unordered_map`，只比较语义相近的 lookup path。\n";
        report << "- `do_not_optimize` 和 compiler memory barrier 用于降低编译器过度优化风险。\n";

        std::cout << "Wrote " << report_path << '\n';
    }
#endif
} // namespace

int main() {
#if defined(CACHE_SYSTEM_CORRECTNESS_ONLY)
    return run_correctness_main();
#else
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
    results.push_back(tinylfu_scan_case(keys));
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
#endif
}
