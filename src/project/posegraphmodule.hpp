#pragma once

#include "graphmodule.hpp"

namespace Pelican {

// pelican.pose: the module that puts a skeletal pose on a common-graph port.
// Registration is all the graph learns; nothing in the partition, the value
// backend or the render backend names a pose operation.
//
// `pelican.pose@1` is opaque on purpose. What a port carries is a staged pose
// -- a PoseStageHandle plus the PoseLayout generation and frame revision it was
// produced for -- not bytes, so two poses from different layout generations can
// be told apart by name (pose.layout_mismatch@1) instead of being memcpy'd
// together. `pelican.rig@1` is the rig identity an operation resolves its clips
// and its output layout against.
//
// The contracts live here (project layer) so the catalog, the editor and a
// build without the animation backend all agree on them. Evaluating them is
// src/core/animation's job.
std::vector<SemanticTypeSchema> builtinPoseTypeSchemas();
void registerPoseGraphModule(GraphModuleRegistry &registry);
// The scalar input ports a pose.state_machine node exposes: one per parameter
// the wrapped pelican.anim_graph document declares, in declaration order. The
// backend reads the same list, so ports and parameters cannot drift apart.
std::vector<std::string> poseStateMachineParameterPorts(const nlohmann::json &document,
                                                        const std::string &node);

} // namespace Pelican
