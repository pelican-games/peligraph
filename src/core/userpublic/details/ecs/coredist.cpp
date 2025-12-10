#include "coredist.hpp"
#include "../../../ecs/core.hpp"

namespace Pelican {

namespace internal {

ECSCoreTemplatePublic &getEcsCore() { return GET_MODULE(ECSCore).getTemplatePublicModule(); }

} // namespace internal

} // namespace Pelican
