#include "../src/core/loader/editorprojectiontransaction.hpp"
#include "../src/core/light/lightcontainer.hpp"
#include "../src/core/phys/physworld.hpp"
#include "../src/core/loader/componentcodec.hpp"
#include "authoringscenetestsupport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {
namespace {

class DocumentTarget final : public test_support::SceneProjectionTarget {
  public:
    explicit DocumentTarget(AuthoringSceneDocument value)
        : SceneProjectionTarget(std::move(value)) {}
};

nlohmann::json projectionFixture() {
    return nlohmann::json::parse(R"json({
      "schema": "pelican.scene",
      "version": 1,
      "scenes": {
        "main": {
          "objects": [
            {
              "name": "Root",
              "components": [
                {"name":"transform","pos":[10,0,0],"rotation":[0,0,0.7071067811865476,0.7071067811865476],"scale":[2,3,4]},
                {"name":"light","type":"directional","direction":[0,-1,0],"intensity":1,"color":[1,1,1]}
              ]
            },
            {
              "name": "Middle",
              "parent": "Root",
              "components": [
                {"name":"transform","pos":[1,2,3],"rotation":[0,0,0,1],"scale":[0.5,2,1]}
              ]
            },
            {
              "name": "Leaf",
              "parent": "Middle",
              "components": [
                {"name":"transform","pos":[2,0,1],"rotation":[0,0,0,1],"scale":[1,0.5,2]},
                {"name":"collider","shape":"sphere","radius":0.5}
              ]
            },
            {
              "name": "Zero",
              "components": [
                {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[0,1,1]}
              ]
            }
          ]
        }
      }
    })json");
}

struct RuntimeSnapshot {
    std::array<int, 8> values{1, 2, 3, 4, 5, 6, 7, 8};
    std::uint64_t entity_id = 41;
    std::vector<std::uint32_t> free_list{9, 4, 2};
    std::uint64_t component_version = 17;
    std::uint64_t light_binding = 23;
    std::uint64_t phys_binding = 29;
    std::uint64_t renderer_handle = 31;
    std::vector<std::string> lifecycle_trace{"base"};
    std::size_t journal_size = 0;

    bool operator==(const RuntimeSnapshot &) const = default;
};

class SnapshotAdapter final : public EditorProjectionAdapter {
    RuntimeSnapshot &runtime_;
    EditorProjectionAdapterKind kind_;
    std::string name_;
    EditorProjectionPublicationMode mode_;
    std::size_t index_ = 0;
    int old_value_ = 0;
    std::size_t old_trace_size_ = 0;
    bool prepared_ = false;
    bool published_ = false;

  public:
    SnapshotAdapter(RuntimeSnapshot &runtime, EditorProjectionAdapterKind kind,
                    std::string name,
                    EditorProjectionPublicationMode mode, std::size_t index)
        : runtime_(runtime), kind_(kind), name_(std::move(name)), mode_(mode),
          index_(index) {}

    EditorProjectionAdapterKind kind() const noexcept override { return kind_; }
    std::string_view name() const noexcept override { return name_; }
    EditorProjectionPublicationMode publicationMode() const noexcept override {
        return mode_;
    }

    void prepare(const EditorProjectionPrepareContext &context) override {
        REQUIRE(context.next_document.revision().value ==
                context.base_document.revision().value + 1);
        old_value_ = runtime_.values[index_];
        old_trace_size_ = runtime_.lifecycle_trace.size();
        prepared_ = true;
    }

    void publish() noexcept override {
        runtime_.values[index_] = old_value_ + 100;
        runtime_.lifecycle_trace.push_back(name_);
        if (kind_ == EditorProjectionAdapterKind::EcsArchetype) {
            ++runtime_.entity_id;
            runtime_.free_list.back() += 1;
            ++runtime_.component_version;
        } else if (kind_ == EditorProjectionAdapterKind::Light) {
            ++runtime_.light_binding;
        } else if (kind_ == EditorProjectionAdapterKind::PhysWorld) {
            ++runtime_.phys_binding;
        } else if (kind_ == EditorProjectionAdapterKind::RendererModel) {
            ++runtime_.renderer_handle;
        }
        published_ = true;
    }

    void rollback() noexcept override {
        if (published_) {
            runtime_.values[index_] = old_value_;
            runtime_.lifecycle_trace.resize(old_trace_size_);
            if (kind_ == EditorProjectionAdapterKind::EcsArchetype) {
                --runtime_.entity_id;
                runtime_.free_list.back() -= 1;
                --runtime_.component_version;
            } else if (kind_ == EditorProjectionAdapterKind::Light) {
                --runtime_.light_binding;
            } else if (kind_ == EditorProjectionAdapterKind::PhysWorld) {
                --runtime_.phys_binding;
            } else if (kind_ == EditorProjectionAdapterKind::RendererModel) {
                --runtime_.renderer_handle;
            }
        }
        prepared_ = false;
        published_ = false;
    }

    void finish() noexcept override {
        prepared_ = false;
        published_ = false;
    }
};

class OnePointFault final : public EditorProjectionFaultInjector {
  public:
    EditorProjectionFaultPoint point = EditorProjectionFaultPoint::Prepare;
    std::string adapter;

    OnePointFault(EditorProjectionFaultPoint point_value,
                  std::string adapter_value)
        : point(point_value), adapter(std::move(adapter_value)) {}

    bool shouldFail(EditorProjectionFaultPoint candidate,
                    std::string_view name) noexcept override {
        return candidate == point && name == adapter;
    }
};

std::vector<SnapshotAdapter> makeSnapshotAdapters(RuntimeSnapshot &runtime) {
    using Kind = EditorProjectionAdapterKind;
    using Mode = EditorProjectionPublicationMode;
    std::vector<SnapshotAdapter> result;
    result.reserve(8);
    result.emplace_back(runtime, Kind::EcsExistingValue, "ecs_value",
                        Mode::StagedNoexcept, 0);
    result.emplace_back(runtime, Kind::EcsArchetype, "ecs_archetype",
                        Mode::InverseToken, 1);
    result.emplace_back(runtime, Kind::TransformClosure, "transform",
                        Mode::StagedNoexcept, 2);
    result.emplace_back(runtime, Kind::RendererModel, "renderer",
                        Mode::InverseToken, 3);
    result.emplace_back(runtime, Kind::Camera, "camera",
                        Mode::StagedNoexcept, 4);
    result.emplace_back(runtime, Kind::Light, "light", Mode::InverseToken, 5);
    result.emplace_back(runtime, Kind::PhysWorld, "phys", Mode::InverseToken, 6);
    result.emplace_back(runtime, Kind::BehaviorAttachment, "behavior",
                        Mode::StagedNoexcept, 7);
    return result;
}

std::vector<EditorProjectionAdapter *>
adapterPointers(std::vector<SnapshotAdapter> &adapters) {
    std::vector<EditorProjectionAdapter *> result;
    result.reserve(adapters.size());
    for (auto &adapter : adapters) result.push_back(&adapter);
    return result;
}

void requireVec3(glm::vec3 value, glm::vec3 expected) {
    REQUIRE_THAT(value.x,
                 Catch::Matchers::WithinAbs(expected.x, 1.0e-4F));
    REQUIRE_THAT(value.y,
                 Catch::Matchers::WithinAbs(expected.y, 1.0e-4F));
    REQUIRE_THAT(value.z,
                 Catch::Matchers::WithinAbs(expected.z, 1.0e-4F));
}

} // namespace

TEST_CASE("Editor projection restores every adapter prepare and inverse publish fault",
          "[editor-projection][fault]") {
    const auto source = projectionFixture();
    const std::array commands{
        makeSetComponentValueCommand(
            "main", "Root", "transform",
            nlohmann::json{{"name", "transform"},
                           {"pos", {12, 1, 0}},
                           {"rotation", {0, 0, 0, 1}},
                           {"scale", {2, 3, 4}}}),
        makeSetComponentValueCommand(
            "main", "Root", "light",
            nlohmann::json{{"name", "light"},
                           {"type", "directional"},
                           {"direction", {1, -1, 0}},
                           {"intensity", 3.0},
                           {"color", {1, 0.5, 0.25}}}),
    };

    for (std::size_t failed = 0; failed < 8; ++failed) {
        DYNAMIC_SECTION("prepare fault " << failed) {
            DocumentTarget target{AuthoringSceneDocument::load(
                source.dump(), SceneRevision{50})};
            const auto semantic_before = target.document.rawJson();
            RuntimeSnapshot runtime;
            runtime.lifecycle_trace.reserve(32);
            const auto runtime_before = runtime;
            auto adapters = makeSnapshotAdapters(runtime);
            auto pointers = adapterPointers(adapters);
            OnePointFault fault{EditorProjectionFaultPoint::Prepare,
                                std::string{adapters[failed].name()}};
            EditorProjectionTransaction transaction{target, SceneRevision{50},
                                                    &fault};
            const auto result = transaction.commit(commands, pointers);

            REQUIRE(result.status == EditorProjectionStatus::Failed);
            REQUIRE(result.error.has_value());
            REQUIRE(result.error->code ==
                    EditorProjectionErrorCode::AdapterPrepareFailed);
            REQUIRE(target.document.revision().value == 50);
            REQUIRE(target.document.rawJson() == semantic_before);
            REQUIRE(runtime == runtime_before);
            REQUIRE(runtime.journal_size == 0);
        }
    }

    const std::array<std::string_view, 4> inverse_names{
        "ecs_archetype", "renderer", "light", "phys"};
    for (const auto failed_name : inverse_names) {
        DYNAMIC_SECTION("inverse publish fault " << failed_name) {
            DocumentTarget target{AuthoringSceneDocument::load(
                source.dump(), SceneRevision{70})};
            const auto semantic_before = target.document.rawJson();
            RuntimeSnapshot runtime;
            runtime.lifecycle_trace.reserve(32);
            const auto runtime_before = runtime;
            auto adapters = makeSnapshotAdapters(runtime);
            auto pointers = adapterPointers(adapters);
            OnePointFault fault{EditorProjectionFaultPoint::Publish,
                                std::string{failed_name}};
            EditorProjectionTransaction transaction{target, SceneRevision{70},
                                                    &fault};
            const auto result = transaction.commit(commands, pointers);

            REQUIRE(result.status == EditorProjectionStatus::Failed);
            REQUIRE(result.error->code ==
                    EditorProjectionErrorCode::AdapterPublishFailed);
            REQUIRE(target.document.revision().value == 70);
            REQUIRE(target.document.rawJson() == semantic_before);
            REQUIRE(runtime == runtime_before);
            REQUIRE(runtime.journal_size == 0);
        }
    }

    DocumentTarget target{
        AuthoringSceneDocument::load(source.dump(), SceneRevision{90})};
    RuntimeSnapshot runtime;
    runtime.lifecycle_trace.reserve(32);
    auto adapters = makeSnapshotAdapters(runtime);
    auto pointers = adapterPointers(adapters);
    EditorProjectionTransaction transaction{target, SceneRevision{90}};
    const auto success = transaction.commit(commands, pointers);
    REQUIRE(success.committed());
    REQUIRE(target.document.revision().value == 91);
    REQUIRE(target.document.rawJson() != source);
    REQUIRE(runtime.values ==
            std::array<int, 8>{101, 102, 103, 104, 105, 106, 107, 108});

    const auto committed_document = target.document.rawJson();
    const auto committed_runtime = runtime;
    EditorProjectionTransaction stale{target, SceneRevision{90}};
    const auto stale_result = stale.commit(commands, pointers);
    REQUIRE(stale_result.status == EditorProjectionStatus::Rejected);
    REQUIRE(stale_result.error->code ==
            EditorProjectionErrorCode::StaleRevision);
    REQUIRE(stale_result.error->object_path == "/");
    REQUIRE(target.document.revision().value == 91);
    REQUIRE(target.document.rawJson() == committed_document);
    REQUIRE(runtime == committed_runtime);
}

TEST_CASE("WP360 AfterPublication restores the authoring resolved pair and every reader",
          "[wp360][editor-projection][fault][atomic-pair]") {
    auto source = projectionFixture();
    source["scenes"]["main"]["objects"][0]["components"].push_back(
        nlohmann::json{{"name", "camera"},
                       {"type", "perspective"},
                       {"yfov", 0.51f},
                       {"znear", 0.2f},
                       {"zfar", 51.0f}});
    DocumentTarget target{AuthoringSceneDocument::load(
        source.dump(), SceneRevision{360})};
    const auto authoring_before = target.document.rawJson();
    const auto revision_before = target.projectionState().revision();
    const auto generation_before =
        target.projectionState().resolverGeneration();
    RuntimeSnapshot runtime;
    runtime.lifecycle_trace.reserve(32);
    const auto runtime_before = runtime;
    auto adapters = makeSnapshotAdapters(runtime);
    auto pointers = adapterPointers(adapters);
    OnePointFault fault{EditorProjectionFaultPoint::AfterPublication,
                        "scene_pair"};
    EditorProjectionTransaction transaction{target, revision_before, &fault};
    const std::array commands{
        makeSetComponentValueCommand(
            "main", "Root", "light",
            nlohmann::json{{"name", "light"},
                           {"type", "directional"},
                           {"direction", {1, 0, 0}},
                           {"intensity", 8.75f},
                           {"color", {0.25f, 0.5f, 1.0f}}}),
        makeSetComponentValueCommand(
            "main", "Root", "camera",
            nlohmann::json{{"name", "camera"},
                           {"type", "perspective"},
                           {"yfov", 0.91f},
                           {"znear", 0.3f},
                           {"zfar", 91.0f}}),
    };

    const auto result = transaction.commit(commands, pointers);
    REQUIRE(result.status == EditorProjectionStatus::Failed);
    REQUIRE(result.error.has_value());
    REQUIRE(result.error->code ==
            EditorProjectionErrorCode::AdapterPublishFailed);
    REQUIRE(target.projectionState().revision() == revision_before);
    REQUIRE(target.projectionState().resolverGeneration() ==
            generation_before);
    REQUIRE(target.document.rawJson() == authoring_before);
    REQUIRE(runtime == runtime_before);

    const auto &resolved = target.projectionState().resolved();
    const auto *scene = resolved.findScene("main");
    REQUIRE(scene != nullptr);
    const auto &root = scene->objects.front();
    const auto find_component = [&](std::string_view name)
        -> const ResolvedComponent & {
        const auto found = std::find_if(
            root.components.begin(), root.components.end(),
            [&](const auto &component) { return component.name == name; });
        REQUIRE(found != root.components.end());
        return *found;
    };
    REQUIRE_THAT(find_component("light")
                     .effective_json.at("intensity").get<float>(),
                 Catch::Matchers::WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(find_component("camera")
                     .effective_json.at("yfov").get<float>(),
                 Catch::Matchers::WithinAbs(0.51f, 1.0e-6f));
    REQUIRE_THAT(find_component("camera")
                     .effective_json.at("zfar").get<float>(),
                 Catch::Matchers::WithinAbs(51.0f, 1.0e-6f));
    std::cout << "WP360_AFTER_PUBLICATION_ROLLBACK revision="
              << resolved.revision().value << " generation="
              << resolved.resolverGeneration().value
              << " camera_yfov="
              << find_component("camera").effective_json.at("yfov")
              << " light_intensity="
              << find_component("light").effective_json.at("intensity")
              << '\n';
}

TEST_CASE("Editor projection structural stage preserves stable identity and destroy interval",
          "[editor-projection][structural][identity]") {
    DocumentTarget target{AuthoringSceneDocument::load(
        projectionFixture().dump(), SceneRevision{100}, 400)};
    const auto original = target.document.query().front().objects;
    REQUIRE(original.size() == 4);
    const auto middle_id = original[1].authoring_object_id;
    const auto leaf_id = original[2].authoring_object_id;
    const auto zero_id = original[3].authoring_object_id;

    const std::array remove{makeRemoveObjectCommand(leaf_id)};
    EditorProjectionTransaction destroy{target, SceneRevision{100}};
    const auto destroyed = destroy.commit(remove, {});
    REQUIRE(destroyed.committed());
    REQUIRE(destroyed.removed_objects.size() == 1);
    const auto closure = destroyed.removed_objects.front();
    REQUIRE(closure.authoring_object_id == leaf_id);
    REQUIRE(closure.previous_object_id == middle_id);
    REQUIRE(closure.next_object_id == zero_id);
    REQUIRE(target.document.query().front().objects.size() == 3);

    const std::array restore{makeRestoreObjectCommand(closure)};
    EditorProjectionTransaction inverse{target, SceneRevision{101}};
    const auto restored = inverse.commit(restore, {});
    REQUIRE(restored.committed());
    const auto after_inverse = target.document.query().front().objects;
    REQUIRE(after_inverse.size() == 4);
    REQUIRE(after_inverse[1].authoring_object_id == middle_id);
    REQUIRE(after_inverse[2].authoring_object_id == leaf_id);
    REQUIRE(after_inverse[2].authoredJson() == closure.authored_json);

    const auto spawned_json = nlohmann::json{
        {"name", "Spawned"},
        {"components", nlohmann::json::array({
             nlohmann::json{{"name", "transform"},
                            {"pos", {1, 2, 3}},
                            {"rotation", {0, 0, 0, 1}},
                            {"scale", {1, 1, 1}}}})}};
    const std::array insert{makeInsertObjectCommand("main", 2, spawned_json)};
    EditorProjectionTransaction spawn{target, SceneRevision{102}};
    const auto spawned = spawn.commit(insert, {});
    REQUIRE(spawned.committed());
    REQUIRE(spawned.structural_changes.size() == 1);
    const auto spawned_id = spawned.structural_changes.front().authoring_object_id;
    REQUIRE(spawned_id.value == 404);
    REQUIRE(target.document.query().front().objects[2].authoring_object_id ==
            spawned_id);

    const std::array rename_reorder{
        makeRenameObjectCommand(spawned_id, std::optional<std::string>{"Renamed"}),
        makeReorderObjectCommand(spawned_id, 4),
    };
    EditorProjectionTransaction reshape{target, SceneRevision{103}};
    REQUIRE(reshape.commit(rename_reorder, {}).committed());
    const auto final_objects = target.document.query().front().objects;
    REQUIRE(final_objects.back().authoring_object_id == spawned_id);
    REQUIRE(final_objects.back().name == std::optional<std::string>{"Renamed"});
}

TEST_CASE("Structural spawn destroy and mixed command faults restore document and runtime exactly",
          "[editor-projection][structural][fault][mixed]") {
    const auto source = projectionFixture();
    const auto identity_source = AuthoringSceneDocument::load(
        source.dump(), SceneRevision{1}, 700);
    const auto ids = test_support::authoring(identity_source)
                         .query()
                         .front()
                         .objects;
    const auto leaf_id = ids[2].authoring_object_id;
    const auto spawned_object = nlohmann::json{
        {"name", "FaultSpawn"},
        {"components", nlohmann::json::array({
             nlohmann::json{{"name", "transform"},
                            {"pos", {4, 5, 6}},
                            {"rotation", {0, 0, 0, 1}},
                            {"scale", {1, 1, 1}}}})}};

    const auto run_fault = [&](std::vector<EditorProjectionCommand> commands,
                               EditorProjectionFaultPoint point,
                               std::string adapter_name) {
        DocumentTarget target{AuthoringSceneDocument::load(
            source.dump(), SceneRevision{200}, 700)};
        const auto document_before = target.document.encodeSemantic();
        RuntimeSnapshot runtime;
        runtime.lifecycle_trace.reserve(32);
        const auto runtime_before = runtime;
        auto adapters = makeSnapshotAdapters(runtime);
        auto pointers = adapterPointers(adapters);
        OnePointFault fault{point, std::move(adapter_name)};
        EditorProjectionTransaction transaction{target, SceneRevision{200},
                                                &fault};
        const auto result = transaction.commit(commands, pointers);
        REQUIRE(result.status == EditorProjectionStatus::Failed);
        REQUIRE(target.document.revision() == SceneRevision{200});
        REQUIRE(target.document.encodeSemantic() == document_before);
        REQUIRE(runtime == runtime_before);
    };

    for (const auto point : {EditorProjectionFaultPoint::Prepare,
                             EditorProjectionFaultPoint::Publish}) {
        DYNAMIC_SECTION("spawn fault " << static_cast<int>(point)) {
            run_fault(
                {makeInsertObjectCommand("main", 1, spawned_object)}, point,
                "ecs_archetype");
        }
        DYNAMIC_SECTION("destroy fault " << static_cast<int>(point)) {
            run_fault({makeRemoveObjectCommand(leaf_id)}, point,
                      "ecs_archetype");
        }
        DYNAMIC_SECTION("mixed spawn set light fault "
                        << static_cast<int>(point)) {
            run_fault(
                {makeInsertObjectCommand("main", 1, spawned_object),
                 makeSetComponentValueCommand(
                     "main", "Root", "transform",
                     nlohmann::json{{"name", "transform"},
                                    {"pos", {20, 2, 0}},
                                    {"rotation", {0, 0, 0, 1}},
                                    {"scale", {2, 3, 4}}}),
                 makeSetComponentValueCommand(
                     "main", "Root", "light",
                     nlohmann::json{{"name", "light"},
                                    {"type", "directional"},
                                    {"direction", {0, -1, -1}},
                                    {"intensity", 4.0},
                                    {"color", {1, 0.5, 0.25}}})},
                point, point == EditorProjectionFaultPoint::Prepare
                           ? "ecs_archetype"
                           : "light");
        }
    }
}

TEST_CASE("Transform projection recomputes descendants and reparent policies",
          "[editor-projection][transform][reparent]") {
    DocumentTarget target{AuthoringSceneDocument::load(
        projectionFixture().dump(), SceneRevision{1})};
    TransformComponent root_world{}, middle_world{}, leaf_world{}, zero_world{};
    LocalTransformComponent root_local{}, middle_local{}, leaf_local{}, zero_local{};
    TransformProjectionAdapter transform{{
        {"main", AuthoringObjectId{1}, EntityId{1, 1}, &root_world,
         &root_local},
        {"main", AuthoringObjectId{2}, EntityId{2, 1}, &middle_world,
         &middle_local},
        {"main", AuthoringObjectId{3}, EntityId{3, 1}, &leaf_world,
         &leaf_local},
        {"main", AuthoringObjectId{4}, EntityId{4, 1}, &zero_world,
         &zero_local},
    }};
    std::array<EditorProjectionAdapter *, 1> adapters{&transform};

    const std::array set_root{makeSetComponentValueCommand(
        "main", "Root", "transform",
        nlohmann::json{{"name", "transform"},
                       {"pos", {10, 0, 0}},
                       {"rotation", {0, 0, 0.7071067811865476,
                                     0.7071067811865476}},
                       {"scale", {2, 3, 4}}})};
    EditorProjectionTransaction first{target, SceneRevision{1}};
    REQUIRE(first.commit(set_root, adapters).committed());
    requireVec3(root_world.pos, {10, 0, 0});
    requireVec3(middle_world.pos, {4, 2, 12});
    requireVec3(middle_world.scale, {1, 6, 4});
    requireVec3(leaf_world.scale, {1, 3, 8});
    REQUIRE((middle_local.parent == EntityId{1, 1}));
    REQUIRE((leaf_local.parent == EntityId{2, 1}));

    const auto world_before = leaf_world;
    const auto local_before = leaf_local;
    const std::array preserve_world{makeReparentCommand(
        "main", "Leaf", std::optional<std::string>{"Root"},
        ReparentPreserve::World)};
    EditorProjectionTransaction second{target, SceneRevision{2}};
    REQUIRE(second.commit(preserve_world, adapters).committed());
    requireVec3(leaf_world.pos, world_before.pos);
    requireVec3(leaf_world.scale, world_before.scale);
    REQUIRE((leaf_local.parent == EntityId{1, 1}));
    REQUIRE_FALSE((leaf_local.pos.x == local_before.pos.x &&
                   leaf_local.pos.y == local_before.pos.y &&
                   leaf_local.pos.z == local_before.pos.z));

    const auto preserved_local = leaf_local;
    const std::array preserve_local{makeReparentCommand(
        "main", "Leaf", std::nullopt, ReparentPreserve::Local)};
    EditorProjectionTransaction third{target, SceneRevision{3}};
    REQUIRE(third.commit(preserve_local, adapters).committed());
    REQUIRE(leaf_local.parent == invalidEntityId);
    REQUIRE(leaf_local.pos.x == preserved_local.pos.x);
    REQUIRE(leaf_local.pos.y == preserved_local.pos.y);
    REQUIRE(leaf_local.pos.z == preserved_local.pos.z);
    REQUIRE_FALSE(leaf_world.pos == world_before.pos);

    const auto document_before_reject = target.document.rawJson();
    const auto leaf_before_reject = leaf_world;
    const std::array zero_scale{makeReparentCommand(
        "main", "Leaf", std::optional<std::string>{"Zero"},
        ReparentPreserve::World)};
    EditorProjectionTransaction zero_reject{target, SceneRevision{4}};
    const auto zero_result = zero_reject.commit(zero_scale, adapters);
    REQUIRE(zero_result.status == EditorProjectionStatus::Rejected);
    REQUIRE(zero_result.error->code ==
            EditorProjectionErrorCode::TransformZeroParentScale);
    REQUIRE(zero_result.error->object_path ==
            "/scenes/main/objects/Leaf");
    REQUIRE(target.document.revision().value == 4);
    REQUIRE(target.document.rawJson() == document_before_reject);
    REQUIRE(leaf_world.pos == leaf_before_reject.pos);

    const std::array cycle{makeReparentCommand(
        "main", "Root", std::optional<std::string>{"Middle"},
        ReparentPreserve::Local)};
    EditorProjectionTransaction cycle_reject{target, SceneRevision{4}};
    const auto cycle_result = cycle_reject.commit(cycle, adapters);
    REQUIRE(cycle_result.status == EditorProjectionStatus::Rejected);
    REQUIRE(cycle_result.error->code ==
            EditorProjectionErrorCode::TransformCycle);
    REQUIRE(target.document.rawJson() == document_before_reject);

    const auto runtime_json = projectTransformRuntimeJson(
        TransformCodecTarget{&leaf_world, &leaf_local, nullptr});
    REQUIRE(runtime_json.contains("local_trs"));
    REQUIRE(runtime_json.contains("world_trs"));
}

TEST_CASE("Transform projection binds an unnamed child by authoring id",
          "[editor-projection][transform][hierarchy][unnamed][wp244]") {
    const auto fixture = nlohmann::json::parse(R"json({
      "schema":"pelican.scene","version":1,
      "scenes":{"main":{"objects":[
        {"name":"Root","components":[
          {"name":"transform","pos":[2,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}
        ]},
        {"parent":"Root","components":[
          {"name":"transform","pos":[1,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}
        ]}
      ]}}
    })json");
    DocumentTarget target{AuthoringSceneDocument::load(
        fixture.dump(), SceneRevision{1})};
    TransformComponent root_world{}, child_world{};
    LocalTransformComponent root_local{}, child_local{};
    TransformProjectionAdapter transform{{
        {"main", AuthoringObjectId{1}, EntityId{1, 1}, &root_world,
         &root_local},
        {"main", AuthoringObjectId{2}, EntityId{2, 1}, &child_world,
         &child_local},
    }};
    std::array<EditorProjectionAdapter *, 1> adapters{&transform};
    const std::array edit_root{makeSetComponentValueCommand(
        "main", "Root", "transform",
        nlohmann::json{{"name", "transform"},
                       {"pos", {4, 0, 0}},
                       {"rotation", {0, 0, 0, 1}},
                       {"scale", {1, 1, 1}}})};

    EditorProjectionTransaction transaction{target, SceneRevision{1}};
    REQUIRE(transaction.commit(edit_root, adapters).committed());
    requireVec3(root_world.pos, {4, 0, 0});
    requireVec3(child_world.pos, {5, 0, 0});
    REQUIRE((child_local.parent == EntityId{1, 1}));
}

TEST_CASE("Light and Phys prepared publication is failure atomic",
          "[editor-projection][light][phys]") {
    const std::vector<LightLoadEntry> lights{
        {"Key", nlohmann::json{{"name", "light"},
                                {"type", "directional"},
                                {"direction", {0, -1, 0}},
                                {"intensity", 2.0},
                                {"color", {1, 1, 1}}}},
    };
    const auto prepared_lights = LightContainer::prepareLoad(lights);
    REQUIRE(prepared_lights.directional_lights.size() == 1);
    REQUIRE(prepared_lights.directional_names.at("Key") == 0);
    auto invalid_lights = lights;
    invalid_lights.push_back(lights.front());
    REQUIRE_THROWS(LightContainer::prepareLoad(invalid_lights));
    REQUIRE(prepared_lights.directional_lights.size() == 1);

    PhysWorld world;
    PhysWorld::Binding first{
        .identity = phys::ColliderIdentity{.name = "Ball"},
        .metadata = {},
        .collider = ColliderComponent{},
        .transform_source = PhysWorldTransform{},
    };
    auto next = world.prepareBindings({first});
    REQUIRE(next.bindings.size() == 1);
    REQUIRE(next.bindings.front().identity.collider_id.valid());
    world.publishPrepared(std::move(next));
    const auto published = world.snapshotPrepared();
    REQUIRE(published.bindings.size() == 1);

    REQUIRE_THROWS(world.prepareBindings({first, first}));
    const auto after_failure = world.snapshotPrepared();
    REQUIRE(after_failure.bindings.size() == 1);
    REQUIRE(after_failure.bindings.front().identity.name == "Ball");
    REQUIRE(after_failure.bindings.front().identity.collider_id ==
            published.bindings.front().identity.collider_id);
}

} // namespace Pelican
