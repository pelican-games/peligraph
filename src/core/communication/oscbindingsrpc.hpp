#pragma once

namespace Pelican {

class InputSequenceRuntime;
class OscInput;
class PathResolver;
class ProblemList;
class ProjectBasicConfig;
class RpcServer;

// get_osc_bindings {} -> {profile: {name, reference} | null, digest,
// live, applied_live}. live is read from disk; applied_live is in the engine.
//
// edit_osc_bindings {profile, base_digest, live} replaces the whole live array.
// It checks the profile identity and raw-file digest, validates with the input
// profile OSC parser, preserves all other ordered JSON keys and commits through
// the project document transaction. A successful commit refreshes every profile
// cache naming the same reference, then applies the active bindings on the
// engine frame thread. -> {committed: true, changed, digest, previous_digest,
// previous, applied, apply_error?}. unchanged input writes nothing.
//
// Refusals carry error_code/error_fields: external_modification,
// input_profile.bindings_invalid@1, none_active@1, profile_changed@1,
// not_writable@1, transaction_busy@1, edit_during_replay@1.
void configureOscBindingsRpcHandlers(RpcServer &server,
                                     PathResolver &path_resolver,
                                     ProjectBasicConfig &project_config,
                                     OscInput &osc_input,
                                     InputSequenceRuntime &input_sequence,
                                     ProblemList &problems);

} // namespace Pelican
