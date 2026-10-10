#pragma once

#include "material.hpp"

namespace Pelican {

// Both hidden variant records and an incomplete base must stay out of live
// draw-loop validation. Nested registration restores the caller's state.
class ScopedMaterialPublicationDeferral {
  public:
    explicit ScopedMaterialPublicationDeferral(bool &deferred)
        : deferred_{deferred}, previous_{deferred} {
        deferred_ = true;
    }
    ~ScopedMaterialPublicationDeferral() { deferred_ = previous_; }
    ScopedMaterialPublicationDeferral(const ScopedMaterialPublicationDeferral &) = delete;
    ScopedMaterialPublicationDeferral &operator=(const ScopedMaterialPublicationDeferral &) = delete;

  private:
    bool &deferred_;
    bool previous_;
};

// Keep the publication transaction independent of Vulkan allocation: a base
// is observable only with its complete variant map. On any later failure,
// release also removes entries already accepted by other live tables.
template <typename CreateBase, typename AttachVariants, typename Publish,
          typename Rollback>
GlobalMaterialId registerMaterialFamily(
    bool &deferred, CreateBase create_base, AttachVariants attach_variants,
    Publish publish, Rollback rollback) {
    auto base = invalidMaterialId();
    try {
        {
            ScopedMaterialPublicationDeferral scope{deferred};
            base = create_base();
            attach_variants(base);
        }
        if (!deferred) publish(base);
        return base;
    } catch (...) {
        if (base != invalidMaterialId()) rollback(base);
        throw;
    }
}

} // namespace Pelican
