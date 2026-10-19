#include "zb/host_assets.h"

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <string>

#include "zb/asset_hostcalls.h"
#include "zb/guest_memory.h"
#include "zb/host_jni.h"
#include "zb/log.h"

namespace zb {

namespace {

constexpr std::uint64_t kMaxFilenameLength = 4096;

// Reads a NUL-terminated guest string, page by page like HostJni::Impl::read_string. ok is false
// for an unreadable address or a string longer than kMaxFilenameLength.
std::string read_guest_string(GuestMemory& memory, std::uint32_t address, bool& ok) {
    ok = false;
    if (address == 0) return {};
    std::string text;
    std::uint64_t cursor = address;
    while (cursor < kGuestSpaceSize && text.size() <= kMaxFilenameLength) {
        const std::uint64_t chunk = kPageSize - (cursor & kPageMask);
        const std::uint8_t* bytes = memory.host_ptr(static_cast<std::uint32_t>(cursor), chunk, kPageRead);
        if (bytes == nullptr) return {};
        const auto* end = static_cast<const std::uint8_t*>(std::memchr(bytes, 0, chunk));
        if (end != nullptr) {
            text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(end - bytes));
            ok = true;
            return text;
        }
        text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(chunk));
        cursor += chunk;
    }
    return {};
}

}  // namespace

std::uint64_t HostAssets::require_manager(std::uint32_t handle) const {
    return managers_.get(handle).value_or(0);
}

std::uint64_t HostAssets::require_asset(std::uint32_t handle) const {
    return assets_.get(handle).value_or(0);
}

std::uint32_t HostAssets::guest_malloc(std::uint32_t size) {
    // Guest malloc runs guest code on this thread; HostAssets holds no lock here.
    GuestCall args;
    args.regs = {size, 0, 0, 0};
    const auto allocated = runtime_.call_on_current(runtime_.service_api().malloc_fn, args);
    return allocated ? allocated->r0 : 0;
}

void HostAssets::guest_free(std::uint32_t address) {
    if (address == 0) return;
    GuestCall args;
    args.regs = {address, 0, 0, 0};
    runtime_.call_on_current(runtime_.service_api().free_fn, args);
}

std::uint32_t HostAssets::asset_buffer(std::uint64_t asset) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto cached = asset_buffers_.find(asset);
        if (cached != asset_buffers_.end()) return cached->second;
    }

    const std::int64_t length = backend_.length(asset);
    const void* host = backend_.buffer(asset);
    if (host == nullptr || length <= 0) {
        log("AAsset_getBuffer: the asset has no buffer (length %lld)", static_cast<long long>(length));
        return 0;
    }
    const auto size = static_cast<std::uint64_t>(length);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size > kMaxBufferedBytes || buffered_bytes_ + size > kMaxBufferedBytes) {
            log("AAsset_getBuffer: %llu bytes exceed the buffer budget", static_cast<unsigned long long>(size));
            return 0;
        }
    }
    const std::uint32_t allocated = guest_malloc(static_cast<std::uint32_t>(size));
    if (allocated == 0) {
        log("AAsset_getBuffer: the guest allocator refused %llu bytes",
            static_cast<unsigned long long>(size));
        return 0;
    }
    std::uint8_t* destination = runtime_.memory().host_ptr(allocated, size, kPageRead | kPageWrite);
    if (destination == nullptr) {
        log("AAsset_getBuffer: the guest allocator returned an unusable buffer");
        guest_free(allocated);
        return 0;
    }
    std::memcpy(destination, host, size);
    std::uint32_t published = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto [entry, inserted] = asset_buffers_.emplace(asset, allocated);
        if (inserted) {
            asset_buffer_sizes_[asset] = static_cast<std::uint32_t>(size);
            buffered_bytes_ += size;
            return allocated;
        }
        published = entry->second;
    }
    // Another thread copied the same asset meanwhile; keep the published copy.
    guest_free(allocated);
    return published;
}

void HostAssets::release_asset_buffer(std::uint64_t asset) {
    std::uint32_t address = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = asset_buffers_.find(asset);
        if (entry == asset_buffers_.end()) return;
        address = entry->second;
        asset_buffers_.erase(entry);
        const auto size = asset_buffer_sizes_.find(asset);
        if (size != asset_buffer_sizes_.end()) {
            buffered_bytes_ -= std::min<std::uint64_t>(buffered_bytes_, size->second);
            asset_buffer_sizes_.erase(size);
        }
    }
    guest_free(address);
}

std::uint32_t HostAssets::next_file_name(std::uint32_t dir_handle, std::uint64_t dir) {
    const char* name = backend_.next_file_name(dir);
    if (name == nullptr) return 0;
    const std::size_t needed = std::strlen(name) + 1;
    if (needed > kMaxFilenameLength) return 0;
    NameBuffer buffer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer = dir_names_[dir_handle];
    }
    if (buffer.capacity < needed) {
        guest_free(buffer.address);
        const std::uint32_t capacity = static_cast<std::uint32_t>((needed + 255) & ~std::size_t{255});
        buffer.address = guest_malloc(capacity);
        buffer.capacity = buffer.address != 0 ? capacity : 0;
        std::lock_guard<std::mutex> lock(mutex_);
        dir_names_[dir_handle] = buffer;
    }
    if (buffer.address == 0) return 0;
    std::uint8_t* destination = runtime_.memory().host_ptr(buffer.address, needed, kPageWrite);
    if (destination == nullptr) return 0;
    std::memcpy(destination, name, needed);
    return buffer.address;
}

void HostAssets::release_dir_buffer(std::uint32_t dir_handle) {
    std::uint32_t address = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = dir_names_.find(dir_handle);
        if (entry == dir_names_.end()) return;
        address = entry->second.address;
        dir_names_.erase(entry);
    }
    guest_free(address);
}

bool HostAssets::handle_host_call(std::uint32_t index, GuestThread& thread) {
    auto& regs = thread.regs();
    switch (index) {
    case ZB_ASSET_HC_AAssetManager_fromJava: {
        // regs[0] is the guest JNIEnv*: ignored, we use the real host JNIEnv of this thread
        // (0 outside an active JNI transition; the backend decides what to do with that).
        const JniBackend::Env env = host_jni_.current_env();
        const JniBackend::Ref java_manager = host_jni_.resolve_ref(regs[1], "AAssetManager_fromJava");
        const std::uint64_t manager = backend_.manager_from_java(env, java_manager);
        regs[0] = manager == 0 ? 0 : managers_.add(manager);
        return true;
    }
    case ZB_ASSET_HC_AAssetManager_open: {
        const std::uint64_t manager = require_manager(regs[0]);
        bool ok = false;
        std::string filename;
        if (manager != 0) filename = read_guest_string(runtime_.memory(), regs[1], ok);
        std::uint64_t asset = 0;
        if (manager != 0 && ok) asset = backend_.open(manager, filename, static_cast<std::int32_t>(regs[2]));
        regs[0] = asset == 0 ? 0 : assets_.add(asset);
        return true;
    }
    case ZB_ASSET_HC_AAsset_close: {
        const std::optional<std::uint64_t> asset = assets_.remove(regs[0]);
        if (asset && *asset != 0) {
            // Before the close: once it is closed the host pointer can come back for a new asset.
            release_asset_buffer(*asset);
            backend_.close(*asset);
        }
        regs[0] = 0;
        return true;
    }
    case ZB_ASSET_HC_AAssetManager_openDir: {
        const std::uint64_t manager = require_manager(regs[0]);
        bool ok = false;
        std::string dirname;
        if (manager != 0) dirname = read_guest_string(runtime_.memory(), regs[1], ok);
        std::uint64_t dir = 0;
        if (manager != 0 && ok) dir = backend_.open_dir(manager, dirname);
        regs[0] = dir == 0 ? 0 : dirs_.add(dir);
        return true;
    }
    case ZB_ASSET_HC_AAssetDir_getNextFileName: {
        const std::uint32_t handle = regs[0];
        const std::uint64_t dir = dirs_.get(handle).value_or(0);
        regs[0] = dir != 0 ? next_file_name(handle, dir) : 0;
        return true;
    }
    case ZB_ASSET_HC_AAssetDir_rewind: {
        const std::uint64_t dir = dirs_.get(regs[0]).value_or(0);
        if (dir != 0) backend_.rewind_dir(dir);
        regs[0] = 0;
        return true;
    }
    case ZB_ASSET_HC_AAssetDir_close: {
        const std::uint32_t handle = regs[0];
        const std::optional<std::uint64_t> dir = dirs_.remove(handle);
        if (dir && *dir != 0) backend_.close_dir(*dir);
        release_dir_buffer(handle);
        regs[0] = 0;
        return true;
    }
    case ZB_ASSET_HC_AAsset_getLength64:
    case ZB_ASSET_HC_AAsset_getRemainingLength64: {
        // off64_t comes back in r0 (low) and r1 (high), AAPCS.
        const std::uint64_t asset = require_asset(regs[0]);
        std::int64_t length = -1;
        if (asset != 0) {
            length = index == ZB_ASSET_HC_AAsset_getLength64 ? backend_.length(asset) : backend_.remaining_length(asset);
        }
        const auto bits = static_cast<std::uint64_t>(length);
        regs[0] = static_cast<std::uint32_t>(bits);
        regs[1] = static_cast<std::uint32_t>(bits >> 32);
        return true;
    }
    case ZB_ASSET_HC_AAsset_getRemainingLength: {
        const std::uint64_t asset = require_asset(regs[0]);
        std::int64_t remaining = asset != 0 ? backend_.remaining_length(asset) : -1;
        if (remaining > std::numeric_limits<std::int32_t>::max()) remaining = -1;
        regs[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(remaining));
        return true;
    }
    case ZB_ASSET_HC_AAsset_seek: {
        // off_t is 32 bits for the guest: (asset, offset, whence) in r0-r2.
        const std::uint64_t asset = require_asset(regs[0]);
        std::int64_t position = -1;
        if (asset != 0) {
            position = backend_.seek(asset, static_cast<std::int32_t>(regs[1]), static_cast<int>(regs[2]));
        }
        if (position > std::numeric_limits<std::int32_t>::max()) position = -1;
        regs[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(position));
        return true;
    }
    case ZB_ASSET_HC_AAsset_seek64: {
        // AAPCS: the 64-bit offset takes the even pair r2:r3 (r1 is skipped) and whence goes to
        // the stack; the off64_t result comes back in r0:r1.
        const std::uint64_t asset = require_asset(regs[0]);
        const std::int64_t offset = static_cast<std::int64_t>(
            (static_cast<std::uint64_t>(regs[3]) << 32) | static_cast<std::uint64_t>(regs[2]));
        std::int64_t position = -1;
        const std::uint8_t* stack = runtime_.memory().host_ptr(regs[13], 4, kPageRead);
        if (asset != 0 && stack != nullptr) {
            std::int32_t whence = 0;
            std::memcpy(&whence, stack, 4);
            position = backend_.seek(asset, offset, whence);
        }
        const auto bits = static_cast<std::uint64_t>(position);
        regs[0] = static_cast<std::uint32_t>(bits);
        regs[1] = static_cast<std::uint32_t>(bits >> 32);
        return true;
    }
    case ZB_ASSET_HC_AAsset_isAllocated: {
        const std::uint64_t asset = require_asset(regs[0]);
        regs[0] = asset != 0 ? static_cast<std::uint32_t>(backend_.is_allocated(asset)) : 0;
        return true;
    }
    case ZB_ASSET_HC_AAsset_openFileDescriptor64: {
        const std::uint64_t asset = require_asset(regs[0]);
        std::int32_t fd = -1;
        if (asset != 0) {
            AssetBackend::FileDescriptor descriptor = backend_.open_file_descriptor(asset);
            if (descriptor.fd >= 0) {
                std::uint8_t* out_start = runtime_.memory().host_ptr(regs[1], 8, kPageWrite);
                std::uint8_t* out_length = runtime_.memory().host_ptr(regs[2], 8, kPageWrite);
                if (out_start != nullptr && out_length != nullptr) {
                    std::memcpy(out_start, &descriptor.start, 8);
                    std::memcpy(out_length, &descriptor.length, 8);
                    fd = descriptor.fd;
                } else {
                    ::close(descriptor.fd);
                }
            }
        }
        regs[0] = static_cast<std::uint32_t>(fd);
        return true;
    }
    case ZB_ASSET_HC_AAsset_getLength: {
        const std::uint64_t asset = require_asset(regs[0]);
        std::int64_t length = asset != 0 ? backend_.length(asset) : -1;
        if (length > std::numeric_limits<std::int32_t>::max()) {
            if (!logged_overflow_) {
                log("AAsset_getLength: asset length %lld exceeds INT32_MAX; returning -1",
                    static_cast<long long>(length));
                logged_overflow_ = true;
            }
            length = -1;
        }
        regs[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(length));
        return true;
    }
    case ZB_ASSET_HC_AAsset_read: {
        const std::uint64_t asset = require_asset(regs[0]);
        const std::uint32_t buf_addr = regs[1];
        const std::uint32_t count = regs[2];
        std::int64_t result = -1;
        if (asset != 0) {
            if (count == 0) {
                result = 0;
            } else {
                std::uint8_t* host_buf = runtime_.memory().host_ptr(buf_addr, count, kPageWrite);
                if (host_buf != nullptr) result = backend_.read(asset, host_buf, count);
            }
        }
        regs[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(result));
        return true;
    }
    case ZB_ASSET_HC_AAsset_getBuffer: {
        const std::uint64_t asset = require_asset(regs[0]);
        regs[0] = asset != 0 ? asset_buffer(asset) : 0;
        return true;
    }
    case ZB_ASSET_HC_AAsset_openFileDescriptor: {
        const std::uint64_t asset = require_asset(regs[0]);
        std::int32_t fd = -1;
        if (asset != 0) {
            AssetBackend::FileDescriptor descriptor = backend_.open_file_descriptor(asset);
            const bool fits = descriptor.fd >= 0 && descriptor.start >= 0 &&
                              descriptor.start <= std::numeric_limits<std::int32_t>::max() &&
                              descriptor.length >= 0 &&
                              descriptor.length <= std::numeric_limits<std::int32_t>::max();
            if (fits) {
                std::uint8_t* out_start = runtime_.memory().host_ptr(regs[1], 4, kPageWrite);
                std::uint8_t* out_length = runtime_.memory().host_ptr(regs[2], 4, kPageWrite);
                if (out_start != nullptr && out_length != nullptr) {
                    const std::uint32_t start32 = static_cast<std::uint32_t>(descriptor.start);
                    const std::uint32_t length32 = static_cast<std::uint32_t>(descriptor.length);
                    std::memcpy(out_start, &start32, 4);
                    std::memcpy(out_length, &length32, 4);
                    fd = descriptor.fd;
                } else {
                    ::close(descriptor.fd);
                }
            } else if (descriptor.fd >= 0) {
                ::close(descriptor.fd);
            }
        }
        regs[0] = static_cast<std::uint32_t>(fd);
        return true;
    }
    default:
        return false;
    }
}

}  // namespace zb
