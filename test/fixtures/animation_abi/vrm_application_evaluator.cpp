#include "animation/vrm_application_v1.hpp"
#include "vrm_application_fixture_protocol.hpp"

#include <array>
#include <cstring>

#if defined(_WIN32)
#define VRM_APPLICATION_FIXTURE_EXPORT extern "C" __declspec(dllexport)
#else
#define VRM_APPLICATION_FIXTURE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {
using namespace Pelican;
using namespace Pelican::Animation;
using namespace Pelican::Vrm;

template <class T> T descriptor() {
    T value{};
    value.struct_size = sizeof(T);
    value.version = applicationDescriptorVersionV1;
    return value;
}

std::uint32_t finish(VrmApplicationFixtureResult *result, Status status) {
    if (result) result->status = static_cast<std::uint32_t>(status);
    return static_cast<std::uint32_t>(status);
}
} // namespace

// This DLL intentionally receives only the public table and an opaque public
// instance handle. It neither links pelican_core nor includes an internal header.
VRM_APPLICATION_FIXTURE_EXPORT std::uint32_t
pelican_vrm_application_public_evaluator(const ApplicationServiceV1 *service,
                                         InstanceHandle instance,
                                         VrmApplicationFixtureResult *result) {
    if (!service || !result || !service->set_expression_inputs ||
        !service->snapshot_expression_inputs ||
        !service->evaluate_expression_look_at ||
        !service->resolve_expression_frame ||
        !service->query_expression_weight || !service->get_diagnostic ||
        !service->publish_application_frame)
        return finish(result, Status::invalid_argument);
    *result = {};

    ExpressionWeightV1 weight{};
    constexpr char happy[] = "happy";
    weight.name = happy;
    weight.name_size = sizeof(happy) - 1;
    weight.value = 0.75f;
    auto input = descriptor<SetExpressionInputDescV1>();
    input.instance = instance;
    input.weights = &weight;
    input.weight_count = 1;
    input.input_revision = 11;
    input.look_at_yaw_degrees = 22.5f;
    input.look_at_pitch_degrees = -15.0f;
    input.flags = expression_input_look_at;
    if (const auto status = service->set_expression_inputs(service->context, &input);
        status != Status::ok)
        return finish(result, status);

    auto snapshot = descriptor<SnapshotExpressionInputDescV1>();
    snapshot.instance = instance;
    snapshot.frame_revision = 77;
    if (const auto status = service->snapshot_expression_inputs(
            service->context, &snapshot);
        status != Status::ok)
        return finish(result, status);
    auto look_at = descriptor<EvaluateExpressionLookAtDescV1>();
    look_at.snapshot = snapshot.snapshot;
    if (const auto status = service->evaluate_expression_look_at(
            service->context, &look_at);
        status != Status::ok)
        return finish(result, status);
    auto resolve = descriptor<ResolveExpressionFrameDescV1>();
    resolve.snapshot = snapshot.snapshot;
    if (const auto status = service->resolve_expression_frame(service->context,
                                                               &resolve);
        status != Status::ok)
        return finish(result, status);
    result->expression_count = resolve.expression_count;
    result->morph_weight_count = resolve.morph_weight_count;
    result->material_override_count = resolve.material_override_count;
    result->diagnostic_count = resolve.diagnostic_count;

    auto query = descriptor<QueryResolvedExpressionWeightDescV1>();
    query.resolved = resolve.resolved;
    query.name = happy;
    query.name_size = sizeof(happy) - 1;
    if (const auto status = service->query_expression_weight(service->context,
                                                              &query);
        status != Status::ok)
        return finish(result, status);
    result->happy_weight = query.value;

    if (resolve.diagnostic_count != 0) {
        auto diagnostic = descriptor<GetApplicationDiagnosticDescV1>();
        diagnostic.resolved = resolve.resolved;
        diagnostic.expression_name = result->expression_name;
        diagnostic.expression_name_capacity = sizeof(result->expression_name);
        diagnostic.material_color_type = result->material_color_type;
        diagnostic.material_color_type_capacity =
            sizeof(result->material_color_type);
        if (const auto status = service->get_diagnostic(service->context,
                                                         &diagnostic);
            status != Status::ok)
            return finish(result, status);
        result->diagnostic_code = static_cast<std::uint32_t>(diagnostic.code);
    }

    auto publish = descriptor<PublishApplicationFrameDescV1>();
    publish.resolved = resolve.resolved;
    return finish(result, service->publish_application_frame(service->context,
                                                               &publish));
}
