#pragma once

#include <cstdint>

namespace zb {

// Host-call indices for the ANativeActivity_* helpers of android/native_activity.h. Hand-written
// from the generated tools/gen_stubs.py order (core/src/gen/hostcalls.inc), which is the source of
// truth: the range is appended last, after GLES 1.1, so nothing established moves. If the
// generated order changes, update these to match.
inline constexpr std::uint32_t ZB_NATIVE_ACTIVITY_HC_finish = 579u;
inline constexpr std::uint32_t ZB_NATIVE_ACTIVITY_HC_hideSoftInput = 580u;
inline constexpr std::uint32_t ZB_NATIVE_ACTIVITY_HC_setWindowFlags = 581u;
inline constexpr std::uint32_t ZB_NATIVE_ACTIVITY_HC_setWindowFormat = 582u;
inline constexpr std::uint32_t ZB_NATIVE_ACTIVITY_HC_showSoftInput = 583u;

inline constexpr std::uint32_t ZB_NATIVE_ACTIVITY_HC_FIRST = 579u;
inline constexpr std::uint32_t ZB_NATIVE_ACTIVITY_HC_LAST = 583u;

}  // namespace zb
