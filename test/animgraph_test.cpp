#include "../src/core/userpublic/animation/animgraph.hpp"
#include "../src/core/animation/animationservice.hpp"
#include "../src/core/model/modeltemplate.hpp"
#include "../src/core/model/skeletalanimation.hpp"
#include "../src/core/userpublic/details/reload/registrationowner.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace Pelican::AnimationGraph {
namespace {

using namespace Pelican::Animation;

template <class T> T descriptor() {
    T value{};
    value.struct_size = sizeof(T);
    value.version = descriptorVersionV1;
    return value;
}

SkeletalModelData graphAsset() {
    SkeletalModelData model;
    model.nodes = {{.parent = -1, .name = "Root"}};
    model.joint_nodes = {0};
    model.inverse_bind_matrices = {glm::mat4{1.0f}};
    model.skin_bindings = {{"Body", 0, 1}};
    const auto add_clip = [&](std::string name, float start_x, float end_x) {
        SkeletalAnimationClip clip;
        clip.name = std::move(name);
        clip.start = 0.0f;
        clip.end = 1.0f;
        clip.channels.push_back({.node = 0,
                                 .path = AnimationPath::translation,
                                 .times = {0.0f, 1.0f},
                                 .values = {{start_x, 0, 0, 0}, {end_x, 0, 0, 0}}});
        model.clips.push_back(std::move(clip));
    };
    add_clip("A", 0.0f, 1.0f);
    add_clip("B", 10.0f, 11.0f);
    add_clip("C", 20.0f, 21.0f);
    add_clip("Walk", 30.0f, 31.0f);
    add_clip("Run", 40.0f, 41.0f);
    return model;
}

AnimationSinkHandle resolveSink(std::string_view object) {
    auto api = descriptor<ApiV1>();
    REQUIRE(getApiV1(abiVersionV1, &api) == Status::ok);
    auto service = descriptor<AnimationServiceV1>();
    REQUIRE(api.get_animation_service(api.context, animationServiceVersionV1, &service) == Status::ok);
    auto sink = descriptor<ResolveAnimationSinkDescV1>();
    sink.object_name = object.data();
    sink.object_name_size = static_cast<std::uint32_t>(object.size());
    sink.sink_kind = AnimationSinkKind::skeletal_pose;
    REQUIRE(service.resolve_sink(service.context, &sink) == Status::ok);
    return sink.sink;
}

std::uint64_t nextRevision() {
    static std::atomic_uint64_t revision{1000};
    return revision.fetch_add(1);
}

void tick(EvaluatorV1 &evaluator, AnimationSinkHandle sink, double dt) {
    const auto revision = nextRevision();
    REQUIRE(evaluator.prepareTick(static_cast<double>(revision) / 60.0, dt, revision) == Status::ok);
    REQUIRE(animationServiceRuntime().runPhases(sink, revision) == Status::ok);
}

void requireSameSemanticTrace(const StatusTraceV1 &left,
                              const StatusTraceV1 &right) {
    REQUIRE(left.current_state == right.current_state);
    REQUIRE(left.transition_active == right.transition_active);
    REQUIRE(left.transition_target == right.transition_target);
    REQUIRE(left.transition_progress == right.transition_progress);
    REQUIRE(left.snapshot_revision == right.snapshot_revision);
    REQUIRE(left.snapshot_layout_identity == right.snapshot_layout_identity);
    REQUIRE(left.snapshot_pose_hash == right.snapshot_pose_hash);
    REQUIRE(left.semantic_pose_hash == right.semantic_pose_hash);
    REQUIRE(left.cursors.size() == right.cursors.size());
    for (std::size_t index = 0; index < left.cursors.size(); ++index) {
        REQUIRE(left.cursors[index].clip == right.cursors[index].clip);
        REQUIRE(left.cursors[index].time_seconds ==
                right.cursors[index].time_seconds);
        REQUIRE(left.cursors[index].normalized_phase ==
                right.cursors[index].normalized_phase);
    }
}

EvaluatorV1 bind(std::string json, std::string object, internal::RegistrationOwner owner = 101) {
    EvaluatorV1 evaluator{parseDocumentV1(json), object};
    internal::ScopedRegistrationOwner owner_scope{owner};
    REQUIRE(evaluator.bind() == Status::ok);
    return evaluator;
}

const char *orderedGraph = R"json({
  "schema":"pelican.anim_graph", "version":1,
  "parameters":{"go":0}, "initial_state":"A",
  "states":[
    {"name":"A","type":"clip","clip":"A"},
    {"name":"B","type":"clip","clip":"B"},
    {"name":"C","type":"clip","clip":"C"}
  ],
  "transitions":[
    {"from":"A","to":"B","priority":10,"duration":0,"conditions":[{"parameter":"go","op":">","value":0}]},
    {"from":"A","to":"C","priority":10,"duration":0,"conditions":[{"parameter":"go","op":">","value":0}]}
  ]
})json";

} // namespace

TEST_CASE("pelican.anim_graph v1 loader enforces envelope flat parameters and reserved keys",
          "[animation][animgraph][a2][schema]") {
    const auto document = parseDocumentV1(orderedGraph);
    REQUIRE(document.states.size() == 3);
    REQUIRE(document.transitions.size() == 2);
    REQUIRE(document.parameters.size() == 1);

    for (const auto *reserved : {"layers", "events", "graphs", "sync", "trigger"}) {
        const auto json = std::string{"{\"schema\":\"pelican.anim_graph\",\"version\":1,\""} +
                          reserved + "\":[],\"states\":[]}";
        REQUIRE_THROWS_WITH(parseDocumentV1(json),
                            Catch::Matchers::ContainsSubstring(std::string{"reserved v2 key '"} + reserved + "'"));
    }
    REQUIRE_THROWS_WITH(parseDocumentV1(R"({"schema":"pelican.anim_graph","version":1,"parameters":{"move.x":0},"states":[{"name":"A","type":"clip","clip":"A"}],"initial_state":"A"})"),
                        Catch::Matchers::ContainsSubstring("single and flat"));
}

TEST_CASE("transition decision is force sequence then priority then declaration order",
          "[animation][animgraph][a2][transition]") {
    auto model = graphAsset();
    auto &runtime = animationServiceRuntime();
    runtime.reset();
    runtime.registerObject("Hero", model);
    {
        auto evaluator = bind(orderedGraph, "Hero");
        const auto sink = resolveSink("Hero");
        REQUIRE(evaluator.setParameter("go", 1.0) == Status::ok);
        tick(evaluator, sink, 0.0);
        REQUIRE(evaluator.getStatus().current_state == "B"); // equal priority: first declaration
    }
    runtime.reset();
    runtime.registerObject("Hero", model);
    {
        auto evaluator = bind(orderedGraph, "Hero", 102);
        const auto sink = resolveSink("Hero");
        REQUIRE(evaluator.setParameter("go", 1.0) == Status::ok);
        REQUIRE(evaluator.forceState("C") == Status::ok);
        REQUIRE(evaluator.forceState("B") == Status::ok);
        tick(evaluator, sink, 0.0);
        REQUIRE(evaluator.getStatus().current_state == "C"); // first force sequence wins
    }
}

TEST_CASE("interrupt materializes one snapshot and alpha zero output is byte identical",
          "[animation][animgraph][a2][interrupt]") {
    constexpr auto graph = R"json({
      "schema":"pelican.anim_graph","version":1,
      "parameters":{"go":0,"interrupt":0},"initial_state":"A",
      "states":[
        {"name":"A","type":"clip","clip":"A"},
        {"name":"B","type":"clip","clip":"B"},
        {"name":"C","type":"clip","clip":"C"}],
      "transitions":[
        {"from":"A","to":"B","priority":1,"interrupt":"always","duration":1,
         "conditions":[{"parameter":"go","op":">","value":0}]},
        {"from":"B","to":"C","priority":2,"interrupt":"always","duration":1,
         "conditions":[{"parameter":"interrupt","op":">","value":0}]}]
    })json";
    auto model = graphAsset();
    auto &runtime = animationServiceRuntime();
    runtime.reset(); runtime.registerObject("Hero", model); runtime.registerObject("Control", model);
    auto evaluator = bind(graph, "Hero", 103);
    auto control = bind(graph, "Control", 109);
    const auto sink = resolveSink("Hero");
    const auto control_sink = resolveSink("Control");
    REQUIRE(evaluator.setParameter("go", 1.0) == Status::ok);
    REQUIRE(control.setParameter("go", 1.0) == Status::ok);
    tick(evaluator, sink, 0.0);   // A -> B, alpha 0
    tick(control, control_sink, 0.0);
    tick(evaluator, sink, 0.25);  // advance the old transition once
    tick(control, control_sink, 0.25);
    REQUIRE(evaluator.setParameter("interrupt", 1.0) == Status::ok);
    tick(evaluator, sink, 0.25);  // materialize current A/B output, then B -> C
    tick(control, control_sink, 0.25); // same old-transition pose, without interrupt
    const auto trace = evaluator.getStatus();
    REQUIRE(trace.transition_active);
    REQUIRE(trace.transition_target == "C");
    REQUIRE(trace.transition_progress == 0.0);
    REQUIRE(trace.snapshot_revision == trace.frame_revision);
    REQUIRE(trace.snapshot_layout_generation != 0);
    REQUIRE(trace.snapshot_pose_hash != 0);
    REQUIRE(trace.snapshot_pose_hash == trace.semantic_pose_hash);
    REQUIRE_FALSE(evaluator.lastPoseBytes().empty());
    const bool interrupt_bytes_equal = evaluator.lastPoseBytes() == control.lastPoseBytes();
    REQUIRE(interrupt_bytes_equal);
}

TEST_CASE("zero cut always interrupts and same-tick re-enter resets the state clock",
          "[animation][animgraph][a2][clock]") {
    constexpr auto graph = R"json({
      "schema":"pelican.anim_graph","version":1,
      "parameters":{"go":0,"cut":0},"initial_state":"A",
      "states":[{"name":"A","type":"clip","clip":"A"},{"name":"B","type":"clip","clip":"B"},{"name":"C","type":"clip","clip":"C"}],
      "transitions":[
        {"from":"A","to":"B","priority":10,"interrupt":"never","duration":1,"conditions":[{"parameter":"go","op":">","value":0}]},
        {"from":"B","to":"C","priority":-10,"interrupt":"never","duration":0,"conditions":[{"parameter":"cut","op":">","value":0}]}]
    })json";
    auto model = graphAsset();
    auto &runtime = animationServiceRuntime();
    runtime.reset(); runtime.registerObject("Hero", model);
    auto evaluator = bind(graph, "Hero", 104);
    const auto sink = resolveSink("Hero");
    REQUIRE(evaluator.setParameter("go", 1.0) == Status::ok);
    tick(evaluator, sink, 0.0);
    REQUIRE(evaluator.setParameter("cut", 1.0) == Status::ok);
    tick(evaluator, sink, 0.1);
    REQUIRE(evaluator.getStatus().current_state == "C");
    REQUIRE_FALSE(evaluator.getStatus().transition_active);
    REQUIRE(evaluator.forceState("C") == Status::ok);
    tick(evaluator, sink, 0.0);
    const auto trace = evaluator.getStatus();
    const auto c = std::find_if(trace.cursors.begin(), trace.cursors.end(), [](const auto &cursor) {
        return cursor.clip == "C";
    });
    REQUIRE(c != trace.cursors.end());
    REQUIRE(c->normalized_phase == 0.0);
}

TEST_CASE("clock covers negative speed start offset loop endpoint and blend1d leader tie",
          "[animation][animgraph][a2][clock][blend1d]") {
    constexpr auto negative = R"json({
      "schema":"pelican.anim_graph","version":1,"initial_state":"reverse",
      "states":[{"name":"reverse","type":"clip","clip":"A","speed":-1,"start_offset":0,"loop":true}]
    })json";
    auto model = graphAsset();
    auto &runtime = animationServiceRuntime();
    runtime.reset(); runtime.registerObject("Hero", model);
    {
        auto evaluator = bind(negative, "Hero", 105);
        const auto sink = resolveSink("Hero");
        tick(evaluator, sink, 0.25);
        const auto cursor = evaluator.getStatus().cursors.front();
        REQUIRE(cursor.normalized_phase == 0.75);
        REQUIRE(cursor.time_seconds == 0.75);
    }

    constexpr auto offset_and_endpoint = R"json({
      "schema":"pelican.anim_graph","version":1,"initial_state":"offset",
      "states":[{"name":"offset","type":"clip","clip":"A","speed":2,"start_offset":0.2,"loop":true}]
    })json";
    runtime.reset(); runtime.registerObject("Hero", model);
    {
        auto evaluator = bind(offset_and_endpoint, "Hero", 110);
        const auto sink = resolveSink("Hero");
        tick(evaluator, sink, 0.0);
        REQUIRE(std::abs(evaluator.getStatus().cursors.front().time_seconds - 0.2) < 1e-12);
        tick(evaluator, sink, 0.4); // 0.2 + (0.4 * speed 2) reaches the repeat endpoint.
        REQUIRE(std::abs(evaluator.getStatus().cursors.front().normalized_phase - 0.8) < 1e-12);
        REQUIRE(std::abs(evaluator.getStatus().cursors.front().time_seconds) < 1e-12);
    }

    constexpr auto blend = R"json({
      "schema":"pelican.anim_graph","version":1,"parameters":{"speed":0.5},"initial_state":"move",
      "states":[{"name":"move","type":"blend1d","parameter":"speed","clips":[
        {"threshold":0,"clip":"Walk","speed":1,"start_offset":0},
        {"threshold":1,"clip":"Run","speed":2,"start_offset":0}]}]
    })json";
    runtime.reset(); runtime.registerObject("Hero", model);
    {
        auto evaluator = bind(blend, "Hero", 106);
        const auto sink = resolveSink("Hero");
        tick(evaluator, sink, 0.1);
        const auto trace = evaluator.getStatus();
        const auto walk = std::find_if(trace.cursors.begin(), trace.cursors.end(), [](const auto &c) { return c.clip == "Walk"; });
        const auto run = std::find_if(trace.cursors.begin(), trace.cursors.end(), [](const auto &c) { return c.clip == "Run"; });
        REQUIRE(walk != trace.cursors.end()); REQUIRE(run != trace.cursors.end());
        REQUIRE(walk->normalized_phase == 0.1);
        REQUIRE(walk->time_seconds == 0.1);
        REQUIRE(run->time_seconds == 0.1); // shared phase; first equal-weight child is leader
    }

    auto zero_duration = graphAsset();
    zero_duration.clips.front().end = zero_duration.clips.front().start;
    runtime.reset(); runtime.registerObject("Zero", zero_duration);
    EvaluatorV1 zero{parseDocumentV1(negative), "Zero"};
    {
        internal::ScopedRegistrationOwner owner_scope{111};
        REQUIRE(zero.bind() == Status::invalid_argument);
    }
    REQUIRE(zero.lastError().find("zero-duration clip") != std::string::npos);
}

TEST_CASE("all discontinuity notifications reset graph clocks and require monotonic revisions",
          "[animation][animgraph][a2][notification]") {
    auto model = graphAsset();
    auto &runtime = animationServiceRuntime();
    runtime.reset(); runtime.registerObject("Hero", model);
    auto evaluator = bind(orderedGraph, "Hero", 112);
    const auto sink = resolveSink("Hero");
    REQUIRE(evaluator.setParameter("go", 1.0) == Status::ok);
    tick(evaluator, sink, 0.0);
    REQUIRE(evaluator.getStatus().current_state == "B");

    for (const auto kind : {AnimationNotificationKind::set_time,
                            AnimationNotificationKind::replay_seek,
                            AnimationNotificationKind::graph_reload,
                            AnimationNotificationKind::model_reload,
                            AnimationNotificationKind::layout_generation_mismatch}) {
        const PoseLayoutHandle old_layout = kind == AnimationNotificationKind::layout_generation_mismatch
                                                ? PoseLayoutHandle{999, 1, 0}
                                                : PoseLayoutHandle{};
        REQUIRE(evaluator.notify(kind, 2.0, old_layout) == Status::ok);
        REQUIRE(evaluator.getStatus().current_state.empty());
        REQUIRE(evaluator.setParameter("go", 0.0) == Status::ok);
        tick(evaluator, sink, 0.0);
        REQUIRE(evaluator.getStatus().current_state == "A");
        REQUIRE(evaluator.getStatus().cursors.front().normalized_phase == 0.0);
    }
}

TEST_CASE("layout mismatch notification survives skeletal asset removal",
          "[animation][animgraph][notification][reload]") {
    auto model = graphAsset();
    auto replacement = graphAsset();
    const ModelAssetId logical_asset{42};
    auto &runtime = animationServiceRuntime();
    runtime.reset();
    runtime.registerObject("Hero", model, logical_asset);
    auto evaluator = bind(orderedGraph, "Hero", 113);
    const auto sink = resolveSink("Hero");

    runtime.reloadAsset(logical_asset, &model, nullptr);

    REQUIRE(evaluator.notify(
                AnimationNotificationKind::layout_generation_mismatch, 2.0,
                PoseLayoutHandle{999, 1, 0}) == Status::ok);
    REQUIRE(evaluator.getStatus().current_state.empty());

    runtime.reloadAsset(logical_asset, nullptr, &replacement);
    auto api = descriptor<ApiV1>();
    REQUIRE(getApiV1(abiVersionV1, &api) == Status::ok);
    auto service = descriptor<AnimationServiceV1>();
    REQUIRE(api.get_animation_service(api.context, animationServiceVersionV1,
                                      &service) == Status::ok);
    auto instance = descriptor<ResolveAnimationInstanceDescV1>();
    instance.sink = sink;
    REQUIRE(service.resolve_instance(service.context, &instance) == Status::ok);
    auto rig = descriptor<ResolveAnimationRigDescV1>();
    rig.instance = instance.instance;
    REQUIRE(service.resolve_rig(service.context, &rig) == Status::ok);
}

TEST_CASE("WP147 evaluator rebind preserves graph state and unrelated actor frames",
          "[animation][animgraph][wp147][rebind]") {
    constexpr auto graph = R"json({
      "schema":"pelican.anim_graph","version":1,
      "parameters":{"go":0},"initial_state":"A",
      "states":[{"name":"A","type":"clip","clip":"A"},
                {"name":"B","type":"clip","clip":"B"}],
      "transitions":[{"from":"A","to":"B","priority":1,
                      "interrupt":"always","duration":1,
                      "conditions":[{"parameter":"go","op":">","value":0}]}]
    })json";
    auto original = graphAsset();
    auto replacement = original;
    auto stable_model = graphAsset();
    auto &runtime = animationServiceRuntime();
    runtime.reset();
    runtime.registerObject("Reloaded", original);
    runtime.registerObject("Stable", stable_model);
    auto reloaded = bind(graph, "Reloaded", 147);
    auto stable = bind(orderedGraph, "Stable", 148);
    const auto reloaded_sink = resolveSink("Reloaded");
    const auto stable_sink = resolveSink("Stable");

    REQUIRE(reloaded.setParameter("go", 1.0) == Status::ok);
    tick(reloaded, reloaded_sink, 0.0);
    tick(reloaded, reloaded_sink, 0.25);
    tick(reloaded, reloaded_sink, 0.0);
    tick(stable, stable_sink, 0.1);
    const auto before_trace = reloaded.getStatus();
    const auto before_pose = reloaded.lastPoseBytes();
    const auto stable_before = stable.getStatus().semantic_pose_hash;

    auto api = descriptor<ApiV1>();
    REQUIRE(getApiV1(abiVersionV1, &api) == Status::ok);
    auto service = descriptor<AnimationServiceV1>();
    REQUIRE(api.get_animation_service(api.context, animationServiceVersionV1,
                                      &service) == Status::ok);
    auto old_instance = descriptor<ResolveAnimationInstanceDescV1>();
    old_instance.sink = reloaded_sink;
    REQUIRE(service.resolve_instance(service.context, &old_instance) == Status::ok);
    auto old_rig = descriptor<ResolveAnimationRigDescV1>();
    old_rig.instance = old_instance.instance;
    REQUIRE(service.resolve_rig(service.context, &old_rig) == Status::ok);
    auto old_layout = descriptor<ResolvePoseLayoutDescV1>();
    old_layout.rig = old_rig.rig;
    REQUIRE(service.resolve_layout(service.context, &old_layout) == Status::ok);
    auto old_clip = descriptor<ResolveAnimationClipDescV1>();
    old_clip.rig = old_rig.rig;
    old_clip.clip_name = "A";
    old_clip.clip_name_size = 1;
    REQUIRE(service.resolve_clip(service.context, &old_clip) == Status::ok);
    CursorHandle stale_cursor{};
    PoseArenaHandle fixture_arena{};
    auto stale_pose = descriptor<PoseViewV1>();
    {
        internal::ScopedRegistrationOwner owner_scope{149};
        auto owner = descriptor<CurrentAnimationOwnerDescV1>();
        REQUIRE(service.get_current_owner(service.context, &owner) == Status::ok);
        auto cursor = descriptor<CreateClipCursorDescV1>();
        cursor.owner = owner.owner;
        cursor.clip = old_clip.clip;
        REQUIRE(service.create_cursor(service.context, &cursor) == Status::ok);
        stale_cursor = cursor.cursor;
        auto begin = descriptor<BeginPoseArenaFrameDescV1>();
        begin.owner = owner.owner;
        begin.frame_revision = nextRevision();
        REQUIRE(service.begin_pose_frame(service.context, &begin) == Status::ok);
        fixture_arena = begin.arena;
        auto acquire = descriptor<AcquirePoseDescV1>();
        acquire.arena = begin.arena;
        acquire.layout = old_layout.layout;
        acquire.joint_count = old_layout.joint_count;
        acquire.out_view = &stale_pose;
        REQUIRE(service.acquire_pose(service.context, &acquire) == Status::ok);
    }

    runtime.reloadAsset(&original, &replacement);

    auto stale_metadata = descriptor<ClipMetadataV1>();
    stale_metadata.clip = old_clip.clip;
    REQUIRE(service.get_clip_metadata(service.context, &stale_metadata) ==
            Status::stale_generation);
    auto advance = descriptor<AdvanceDescV1>();
    advance.cursor = stale_cursor;
    advance.delta_seconds = 0.1;
    auto interval = descriptor<IntervalResultV1>();
    REQUIRE(api.advance_cursor(api.context, &advance, &interval) ==
            Status::stale_generation);
    auto stale_acquire = descriptor<AcquirePoseDescV1>();
    stale_acquire.arena = fixture_arena;
    stale_acquire.layout = old_layout.layout;
    stale_acquire.joint_count = old_layout.joint_count;
    auto untouched_pose = descriptor<PoseViewV1>();
    stale_acquire.out_view = &untouched_pose;
    REQUIRE(service.acquire_pose(service.context, &stale_acquire) ==
            Status::stale_generation);

    auto current_instance = descriptor<ResolveAnimationInstanceDescV1>();
    current_instance.sink = reloaded_sink;
    REQUIRE(service.resolve_instance(service.context, &current_instance) ==
            Status::ok);
    auto current_rig = descriptor<ResolveAnimationRigDescV1>();
    current_rig.instance = current_instance.instance;
    REQUIRE(service.resolve_rig(service.context, &current_rig) == Status::ok);
    auto current_clip = descriptor<ResolveAnimationClipDescV1>();
    current_clip.rig = current_rig.rig;
    current_clip.clip_name = "A";
    current_clip.clip_name_size = 1;
    REQUIRE(service.resolve_clip(service.context, &current_clip) == Status::ok);
    auto stale_sample = descriptor<SamplePoseAtDescV1>();
    stale_sample.clip = current_clip.clip;
    stale_sample.time_seconds = 0.25;
    stale_sample.output_pose = stale_pose.pose;
    REQUIRE(service.sample_pose_at(service.context, &stale_sample) ==
            Status::stale_generation);

    const auto stale_revision = nextRevision();
    REQUIRE(reloaded.prepareTick(10.0, 0.0, stale_revision) == Status::ok);
    REQUIRE(runtime.runPhases(reloaded_sink, stale_revision) ==
            Status::stale_generation);
    requireSameSemanticTrace(before_trace, reloaded.getStatus());

    // The second model keeps its sink/source/phase and produces the next frame
    // while the reloaded evaluator is stale.
    tick(stable, stable_sink, 0.0);
    REQUIRE(stable.getStatus().frame_revision != 0);
    REQUIRE(stable.getStatus().semantic_pose_hash == stable_before);

    REQUIRE(reloaded.rebind() == Status::ok);
    requireSameSemanticTrace(before_trace, reloaded.getStatus());
    tick(reloaded, reloaded_sink, 0.0);
    requireSameSemanticTrace(before_trace, reloaded.getStatus());
    const auto rebound_pose = reloaded.lastPoseBytes();
    REQUIRE(rebound_pose.size() == before_pose.size());
    REQUIRE(std::memcmp(rebound_pose.data(), before_pose.data(),
                        before_pose.size()) == 0);
}

TEST_CASE("non-finite parameter is rejected and deterministic replay and two actors stay independent",
          "[animation][animgraph][a2][determinism]") {
    auto model = graphAsset();
    auto &runtime = animationServiceRuntime();
    runtime.reset(); runtime.registerObject("HeroA", model); runtime.registerObject("HeroB", model);
    auto a = bind(orderedGraph, "HeroA", 107);
    auto b = bind(orderedGraph, "HeroB", 107);
    const auto sink_a = resolveSink("HeroA");
    const auto sink_b = resolveSink("HeroB");
    REQUIRE(a.setParameter("go", std::numeric_limits<double>::quiet_NaN()) == Status::invalid_argument);
    REQUIRE(a.setParameter("go", std::numeric_limits<double>::infinity()) == Status::invalid_argument);
    REQUIRE(a.setParameter("go", 1.0) == Status::ok);
    REQUIRE(b.setParameter("go", 0.0) == Status::ok);
    tick(a, sink_a, 0.0);
    tick(b, sink_b, 0.0);
    REQUIRE(a.getStatus().current_state == "B");
    REQUIRE(b.getStatus().current_state == "A");
    REQUIRE(a.getStatus().semantic_pose_hash != b.getStatus().semantic_pose_hash);

    const auto replay_once = [&]() {
        runtime.reset(); runtime.registerObject("Replay", model);
        auto evaluator = bind(orderedGraph, "Replay", 108);
        const auto sink = resolveSink("Replay");
        REQUIRE(evaluator.setParameter("go", 1.0) == Status::ok);
        tick(evaluator, sink, 1.0 / 60.0);
        return std::pair{evaluator.getStatus().semantic_pose_hash, evaluator.lastPoseBytes()};
    };
    // Destroy the two active evaluators before reset invalidates their owners.
    a = EvaluatorV1{parseDocumentV1(orderedGraph), "unused"};
    b = EvaluatorV1{parseDocumentV1(orderedGraph), "unused"};
    const auto first = replay_once();
    const auto second = replay_once();
    REQUIRE(first.first == second.first);
    const bool pose_bytes_equal = first.second == second.second;
    REQUIRE(pose_bytes_equal);
}

} // namespace Pelican::AnimationGraph
