#pragma once

#include "handle.hpp"

namespace Pelican {

template <typename THandle, typename TResource> class ResourceContainer {
    using HandleBaseType = typename THandle::BaseType;
    HandleBaseType counter = 0;
    std::unordered_map<THandle, TResource, typename THandle::Hash> kv;

  public:
    THandle reg(TResource &&rsrc) {
        const auto new_handle = THandle{counter};
        counter++;
        kv.emplace(new_handle, std::move(rsrc));
        return new_handle;
    }
    void unreg(THandle handle) { kv.erase(handle); }
    auto &get(THandle handle) { return kv.at(handle); }
    const auto &get(THandle handle) const { return kv.at(handle); }
};

} // namespace Pelican