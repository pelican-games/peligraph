#pragma once

#include "handle.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Pelican {

template <typename THandle, typename TResource> class ResourceContainer {
    using HandleBaseType = typename THandle::BaseType;
    HandleBaseType counter = 0;
    std::unordered_map<THandle, TResource, typename THandle::Hash> kv;
    std::vector<HandleBaseType> free_handles;

  public:
    THandle reg(TResource &&rsrc) {
        return reg(std::move(rsrc), std::numeric_limits<HandleBaseType>::max());
    }
    THandle reg(TResource &&rsrc, HandleBaseType exclusive_limit) {
        const bool reusing = !free_handles.empty();
        HandleBaseType value{};
        if (reusing) {
            value = free_handles.back();
        } else {
            if (counter >= exclusive_limit) {
                throw std::runtime_error("resource handle table exhausted");
            }
            value = counter;
        }
        const auto new_handle = THandle{value};
        const auto [_, inserted] = kv.emplace(new_handle, std::move(rsrc));
        if (!inserted) throw std::runtime_error("resource handle table is inconsistent");
        if (reusing) free_handles.pop_back();
        else ++counter;
        return new_handle;
    }
    std::optional<TResource> extract(THandle handle, bool reusable = true) {
        const auto found = kv.find(handle);
        if (found == kv.end()) return std::nullopt;
        // Make the only potentially allocating free-list operation happen
        // before ownership is moved out of the live table.
        if (reusable) free_handles.reserve(free_handles.size() + 1);
        std::optional<TResource> result{std::move(found->second)};
        kv.erase(found);
        if (reusable) free_handles.push_back(handle.value);
        return result;
    }
    void recycle(THandle handle) {
        if (kv.contains(handle) ||
            std::find(free_handles.begin(), free_handles.end(), handle.value) != free_handles.end()) {
            throw std::runtime_error("resource handle cannot be recycled twice");
        }
        free_handles.push_back(handle.value);
    }
    void unreg(THandle handle) { (void)extract(handle); }
    auto &get(THandle handle) { return kv.at(handle); }
    const auto &get(THandle handle) const { return kv.at(handle); }
    bool contains(THandle handle) const { return kv.contains(handle); }
    size_t size() const { return kv.size(); }
};

} // namespace Pelican
