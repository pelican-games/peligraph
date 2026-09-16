#include "../userpublic/animation/abi_v1.hpp"
#include "../userpublic/animation/pose_staging_v1.hpp"
#include "../userpublic/animation/vrm_application_v1.hpp"

// PELICAN_WITH_ANIMATION=OFF. animation/abi_v1.hpp stays frozen and the
// negotiation entry points stay exported, so a game DLL built against the SDK
// still loads and learns by name that this binary brings no animation runtime.
// Nothing here evaluates a pose: the evaluator, the service and the
// pelican.pose graph module are absent from the build, and a document naming a
// pose operation is rejected as graph.operation_unregistered@1.
namespace Pelican::Animation {

Status getApiV1(std::uint32_t, ApiV1 *out_api) noexcept {
    if (out_api == nullptr) return Status::invalid_argument;
    return Status::unsupported_version;
}

Status getPoseStagingServiceV1(std::uint32_t, PoseStagingServiceV1 *out_service) noexcept {
    if (out_service == nullptr) return Status::invalid_argument;
    return Status::unsupported_version;
}

} // namespace Pelican::Animation

namespace Pelican::Vrm {

Animation::Status getApplicationServiceV1(std::uint32_t, ApplicationServiceV1 *out_service) noexcept {
    if (out_service == nullptr) return Animation::Status::invalid_argument;
    return Animation::Status::unsupported_version;
}

} // namespace Pelican::Vrm
