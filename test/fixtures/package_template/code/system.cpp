// One game system shipped by a package.
//
// Everything this file includes comes from the public Pelican SDK.  The
// package target's only engine include path is the published SDK surface, so
// reaching for an engine-internal header (for example "vkcore/renderer.hpp")
// does not compile.
#include <gamecontext.hpp>
#include <gamesystem.hpp>

#include <iostream>

namespace {

class AcmeGlowKitSystem {
    bool announced_ = false;

  public:
    void update(Pelican::GameContext &) {
        if (announced_) return;
        announced_ = true;
        // One line on stderr: the proof that a package's code is in the
        // running player and not only in the build.
        std::cerr << "acme.glowkit: package system running" << std::endl;
    }
};

} // namespace

PELICAN_REGISTER_SYSTEM(AcmeGlowKitSystem, 100);
