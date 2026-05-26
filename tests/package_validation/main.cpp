#include <cache_system/cache_system.hpp>

#include <string>

int main() {
    cache_system::lru_cache<int, std::string> cache(2);
    cache.put(1, "one");
    cache.put(2, "two");

    cache_system::tinylfu_cache<int, int> admission_cache(2);
    admission_cache.put(1, 10);

    const auto value = cache.get(1);
    return value.has_value() && *value == "one" && cache_system::version() != nullptr ? 0 : 1;
}
