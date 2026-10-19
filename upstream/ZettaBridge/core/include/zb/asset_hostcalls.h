#pragma once

#include <cstdint>

namespace zb {

// Host-call indices for the six AAsset* functions HostAssets serves (Phase 5 Task 7). Hand-written
// from tools/gen_stubs.py's android_asset_names() output (core/src/gen/hostcalls.inc), which is the
// generated source of truth: if gen_stubs.py's function list or ordering changes, update these to
// match. The comment in core/include/zb/host_gl.h ("Serves the GLES range 0-141 and leaves 142-160
// for HostAssets") documents the reserved range; all 18 AAsset*/AAssetDir* stubs in that range are
// served (Minecraft PE lists asset directories and reads assets through the 64-bit variants).
inline constexpr std::uint32_t ZB_ASSET_HC_AAssetDir_close = 142u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAssetDir_getNextFileName = 143u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAssetDir_rewind = 144u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAssetManager_fromJava = 145u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAssetManager_open = 146u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAssetManager_openDir = 147u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_close = 148u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_getBuffer = 149u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_getLength = 150u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_getLength64 = 151u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_getRemainingLength = 152u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_getRemainingLength64 = 153u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_isAllocated = 154u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_openFileDescriptor = 155u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_openFileDescriptor64 = 156u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_read = 157u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_seek = 158u;
inline constexpr std::uint32_t ZB_ASSET_HC_AAsset_seek64 = 159u;

}  // namespace zb
