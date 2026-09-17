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
// The playback parameters of a pose.clip_sample node, with the only copy of
// their defaults. The contract validates them here and the animation backend
// reads them through the same two functions, so a node cannot mean one thing to
// the catalog and another to the evaluator. `speed` defaults to 1, `start_time`
// to 0 and `loop` to true -- the same three the animation component carries.
double poseClipSampleNumber(const nlohmann::json &parameters, const char *key, double fallback,
                            const std::string &node);
bool poseClipSampleLoop(const nlohmann::json &parameters, const std::string &node);
// AG-2 root motion. `root_motion` is "none" (the default) or "extract"; with
// "extract" the node stops letting the clip's root translation slide the mesh
// inside the instance and hands the per-frame difference to the owning object's
// transform instead. `root_node` is the glTF node index whose translation is
// that root -- the same number space `render.attribute{pose_node}` uses -- and
// it is required when extracting. A value that is neither "none" nor "extract"
// is a plain invalid_argument, like every other clip_sample parameter: the
// named rejections are reserved for what an author can only discover once the
// rig, the clip and the object are known.
bool poseClipSampleExtractsRootMotion(const nlohmann::json &parameters, const std::string &node);
// pose.root_node_unknown@1 when `extract` is asked for without a usable
// `root_node`. Out of range and absent from the layout are the same name,
// raised at attach time where the rig is known.
std::uint32_t poseClipSampleRootNode(const nlohmann::json &parameters, const std::string &node);

} // namespace Pelican
