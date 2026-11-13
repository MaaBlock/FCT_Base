#ifndef FCT_BASE_THIRDPARTY_H
#define FCT_BASE_THIRDPARTY_H

#include "TracyImports.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef __ANDROID__
#define FCT_USE_ANDROID
#define FCT_ANDROID
#endif

#if defined(_DEBUG) || defined(DEBUG)
#define FCT_DEBUG_MODE
#define FCT_DEBUG
#endif

#ifndef FORCE_INLINE
#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE inline __attribute__((always_inline))
#else
#define FORCE_INLINE inline
#endif
#endif

#ifndef FCT_FORCE_INLINE
#define FCT_FORCE_INLINE FORCE_INLINE
#endif

#endif // FCT_BASE_THIRDPARTY_H
