#pragma once

#ifndef CACHE_SYSTEM_VERSION_MAJOR
#define CACHE_SYSTEM_VERSION_MAJOR 0
#endif

#ifndef CACHE_SYSTEM_VERSION_MINOR
#define CACHE_SYSTEM_VERSION_MINOR 1
#endif

#ifndef CACHE_SYSTEM_VERSION_PATCH
#define CACHE_SYSTEM_VERSION_PATCH 0
#endif

#define CACHE_SYSTEM_DETAIL_STRINGIFY(value) #value
#define CACHE_SYSTEM_DETAIL_EXPAND_AND_STRINGIFY(value) CACHE_SYSTEM_DETAIL_STRINGIFY(value)

#ifndef CACHE_SYSTEM_VERSION_STRING
#define CACHE_SYSTEM_VERSION_STRING                                                                       \
    CACHE_SYSTEM_DETAIL_EXPAND_AND_STRINGIFY(CACHE_SYSTEM_VERSION_MAJOR) "."                              \
        CACHE_SYSTEM_DETAIL_EXPAND_AND_STRINGIFY(CACHE_SYSTEM_VERSION_MINOR) "."                          \
            CACHE_SYSTEM_DETAIL_EXPAND_AND_STRINGIFY(CACHE_SYSTEM_VERSION_PATCH)
#endif

namespace cache_system {

inline constexpr int version_major = CACHE_SYSTEM_VERSION_MAJOR;
inline constexpr int version_minor = CACHE_SYSTEM_VERSION_MINOR;
inline constexpr int version_patch = CACHE_SYSTEM_VERSION_PATCH;
inline constexpr const char* version_string = CACHE_SYSTEM_VERSION_STRING;

} // namespace cache_system
