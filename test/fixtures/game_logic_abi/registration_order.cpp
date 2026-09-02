#include <gamesystem.hpp>

namespace {

class DuplicateSystem final {
  public:
    void update(Pelican::GameContext &) {}
};

class AlphaSystem final {
  public:
    void update(Pelican::GameContext &) {}
};

class ZuluSystem final {
  public:
    void update(Pelican::GameContext &) {}
};

} // namespace

#if WP381_REGISTRATION_ORDER == 1
PELICAN_REGISTER_SYSTEM(DuplicateSystem, 10);
PELICAN_REGISTER_SYSTEM(DuplicateSystem, 20);
#elif WP381_REGISTRATION_ORDER == 2
PELICAN_REGISTER_SYSTEM(ZuluSystem, 10);
PELICAN_REGISTER_SYSTEM(ZuluSystem, 20);
PELICAN_REGISTER_SYSTEM(AlphaSystem, 30);
PELICAN_REGISTER_SYSTEM(AlphaSystem, 40);
#elif WP381_REGISTRATION_ORDER == 3
PELICAN_REGISTER_SYSTEM(AlphaSystem, 10);
PELICAN_REGISTER_SYSTEM(AlphaSystem, 20);
PELICAN_REGISTER_SYSTEM(ZuluSystem, 30);
PELICAN_REGISTER_SYSTEM(ZuluSystem, 40);
#else
#error Unsupported WP381 registration-order fixture
#endif
