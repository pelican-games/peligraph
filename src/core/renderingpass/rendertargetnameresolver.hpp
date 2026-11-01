#pragma once

#include "renderingpass.hpp"
#include <functional>
#include <set>
#include <string>

namespace Pelican {

class RenderTargetContainer;

// RC-7e: a lookup by name found no registration because the generation did
// not register the declared target (no graph uses it):
// std::runtime_error "render_target.not_allocated@1: <name>".
[[noreturn]] void throwRenderTargetNotAllocated(const std::string &name);

class RenderTargetNameResolver {
    std::function<GlobalRenderTargetId(const std::string &)> resolve_target;
    std::set<std::string> unallocated_targets;

  public:
    explicit RenderTargetNameResolver(const RenderTargetContainer &rt_container,
                                      std::set<std::string> unallocated = {});
    explicit RenderTargetNameResolver(std::function<GlobalRenderTargetId(const std::string &)> resolve_target);
    GlobalRenderTargetId resolve(const std::string &name) const;
    // RC-7e: the generation declared `name` and did not register it.
    bool unallocated(const std::string &name) const { return unallocated_targets.contains(name); }
};

} // namespace Pelican
