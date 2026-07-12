#include "../src/core/watch/reloadservice.hpp"
#include "../src/core/watch/reloadtransaction.hpp"
#include "../src/core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>

namespace Pelican::watch {
namespace {

std::shared_ptr<const void> number(int value) {
    return std::make_shared<const int>(value);
}

ReloadActor replace(LogicalResourceRef target, int value,
                    std::vector<AssetKey> dependencies = {},
                    std::vector<LogicalResourceRef> after = {}) {
    return ReloadActor{
        .name = "fake",
        .target = std::move(target),
        .after = std::move(after),
        .stage = [value, dependencies = std::move(dependencies)] {
            return StagedResourceData{number(value), dependencies, 9, sizeof(int)};
        },
    };
}

} // namespace

TEST_CASE("AssetKey is canonical project identity plus fragment", "[wp98][assetkey]") {
    const auto explicit_ref = makeAssetKey("project://assets/models/character.glb#mesh/Cube");
    const auto mounted_ref = makeAssetKey("assets", "models/character.glb", "mesh/Cube");
    REQUIRE(explicit_ref == mounted_ref);
    REQUIRE(assetKeyString(explicit_ref) ==
            "project://assets/models/character.glb#mesh/Cube");
    REQUIRE(makeAssetKey("assets", "models/../models/character.glb", "mesh/Cube") ==
            explicit_ref);
    REQUIRE(makeAssetKey("project://assets/models/character.glb#mesh/Other") != explicit_ref);
    REQUIRE_THROWS(makeAssetKey("project://assets/../../outside.bin"));
    REQUIRE_THROWS(makeAssetKey("C:\\physical\\override.bin"));
}

TEST_CASE("Logical identity survives 1000 commits and generation changes only on slot reuse",
          "[wp98][identity]") {
    ReloadCoordinator coordinator;
    const auto source = makeAssetKey("assets/texture.ktx2");
    const auto dependency = makeAssetKey("assets/source.png");
    const auto original = coordinator.registry().declareResource(
        "textures", source, number(0), {dependency}, 9, sizeof(int));
    REQUIRE(logicalAssetGeneration(original.id) == 1);

    for (int revision = 1; revision <= 1000; ++revision) {
        ReloadTransactionGroup group{"texture"};
        group.add(replace(original, revision, {dependency}));
        coordinator.enqueue(std::move(group));
        REQUIRE(coordinator.applyFrame() == 1);
        const auto live = coordinator.registry().snapshot().find(original);
        REQUIRE(live);
        REQUIRE(live->ref == original);
        REQUIRE(live->content_revision == static_cast<std::uint64_t>(revision + 1));
        REQUIRE(logicalAssetGeneration(live->ref.id) == 1);
        REQUIRE(*live->payloadAs<int>() == revision);
        REQUIRE(live->compatibility_revision == 9);
    }

    auto before_destroy = coordinator.registry().snapshot();
    REQUIRE(before_destroy.reverseDependents(dependency) ==
            std::vector<LogicalResourceRef>{original});
    REQUIRE(coordinator.registry().destroyResource(original));
    REQUIRE_FALSE(coordinator.registry().snapshot().find(original));

    const auto redeclared = coordinator.registry().declareResource(
        "textures", source, number(2000), {dependency}, 12, sizeof(int));
    REQUIRE(logicalAssetIndex(redeclared.id) == logicalAssetIndex(original.id));
    REQUIRE(logicalAssetGeneration(redeclared.id) == logicalAssetGeneration(original.id) + 1);
    REQUIRE(redeclared != original);
    REQUIRE_FALSE(coordinator.registry().snapshot().find(original));
    REQUIRE(coordinator.registry().snapshot().find(redeclared)->content_revision == 1);
}

TEST_CASE("Candidate or stage failure rolls back resource state and reverse edges",
          "[wp98][rollback]") {
    ReloadCoordinator coordinator;
    const auto old_dependency = makeAssetKey("shaders/old.slang");
    const auto new_dependency = makeAssetKey("shaders/new.slang");
    const auto material = coordinator.registry().declareResource(
        "materials", makeAssetKey("materials/a.json"), number(1), {old_dependency}, 4, 100);
    const auto pipeline = coordinator.registry().declareResource(
        "pipelines", makeAssetKey("shaders/a.surface"), number(2), {old_dependency}, 6, 200);
    const auto before = coordinator.registry().snapshot();

    int staged_candidate_destroyed = 0;
    struct Candidate {
        int *destroyed;
        explicit Candidate(int *counter) : destroyed{counter} {}
        ~Candidate() { ++*destroyed; }
    };
    ReloadTransactionGroup group{"surface-material"};
    group.add(ReloadActor{
        .name = "material",
        .target = material,
        .stage = [&] {
            auto candidate = std::make_shared<const Candidate>(&staged_candidate_destroyed);
            return StagedResourceData{std::move(candidate), {new_dependency}, 5, 111};
        },
    });
    group.add(ReloadActor{
        .name = "pipeline",
        .target = pipeline,
        .after = {material},
        .stage = []() -> StagedResourceData { throw std::runtime_error("fake stage failure"); },
    });
    coordinator.enqueue(std::move(group));
    REQUIRE(coordinator.applyFrame() == 1);

    const auto after = coordinator.registry().snapshot();
    REQUIRE(after.resourceCount() == before.resourceCount());
    REQUIRE(after.liveBytes() == before.liveBytes());
    REQUIRE(after.find(material)->content_revision == before.find(material)->content_revision);
    REQUIRE(after.find(pipeline)->content_revision == before.find(pipeline)->content_revision);
    REQUIRE(after.find(material)->payload == before.find(material)->payload);
    REQUIRE(after.find(pipeline)->payload == before.find(pipeline)->payload);
    REQUIRE(after.reverseDependents(old_dependency) == before.reverseDependents(old_dependency));
    REQUIRE(after.reverseDependents(new_dependency).empty());
    REQUIRE(staged_candidate_destroyed == 1);
    const auto status = coordinator.status();
    REQUIRE(status.applied == 0);
    REQUIRE(status.failed == 1);
    REQUIRE(status.last_reload_error->message == "fake stage failure");
}

TEST_CASE("Transaction runs parse validate stage phases and retires in topological order",
          "[wp98][transaction]") {
    ReloadCoordinator coordinator;
    const auto left = coordinator.registry().declareResource(
        "left", makeAssetKey("left.bin"), number(10));
    const auto right = coordinator.registry().declareResource(
        "right", makeAssetKey("right.bin"), number(20));
    std::vector<std::string> phases;

    ReloadTransactionGroup group{"ordered"};
    group.add(ReloadActor{
        .name = "right",
        .target = right,
        .after = {left},
        .parse = [&] { phases.push_back("parse-right"); },
        .validate = [&] { phases.push_back("validate-right"); },
        .stage = [&] {
            phases.push_back("stage-right");
            return StagedResourceData{number(21)};
        },
    });
    group.add(ReloadActor{
        .name = "left",
        .target = left,
        .parse = [&] { phases.push_back("parse-left"); },
        .validate = [&] { phases.push_back("validate-left"); },
        .stage = [&] {
            phases.push_back("stage-left");
            return StagedResourceData{number(11)};
        },
    });
    coordinator.enqueue(std::move(group));
    std::vector<int> retired;
    coordinator.applyFrame([&](std::shared_ptr<const void> payload) {
        retired.push_back(*std::static_pointer_cast<const int>(std::move(payload)));
    });

    REQUIRE(phases == std::vector<std::string>{"parse-right", "parse-left",
                                               "validate-right", "validate-left",
                                               "stage-right", "stage-left"});
    REQUIRE(retired == std::vector<int>{10, 20});
    REQUIRE(*coordinator.registry().snapshot().find(left)->payloadAs<int>() == 11);
    REQUIRE(*coordinator.registry().snapshot().find(right)->payloadAs<int>() == 21);
}

TEST_CASE("Frame barrier never exposes half-committed revisions across logical tables",
          "[wp98][barrier]") {
    ReloadCoordinator coordinator;
    const auto left = coordinator.registry().declareResource(
        "materials", makeAssetKey("materials/a.json"), number(0), {}, 0, sizeof(int));
    const auto right = coordinator.registry().declareResource(
        "pipelines", makeAssetKey("shaders/a.surface"), number(0), {}, 0, sizeof(int));

    std::atomic<bool> running{true};
    std::atomic<bool> saw_half_commit{false};
    std::thread observer{[&] {
        while (running.load(std::memory_order_relaxed)) {
            const auto snapshot = coordinator.registry().snapshot();
            const auto a = snapshot.find(left);
            const auto b = snapshot.find(right);
            if (!a || !b || a->content_revision != b->content_revision ||
                *a->payloadAs<int>() != *b->payloadAs<int>()) {
                saw_half_commit = true;
                break;
            }
        }
    }};

    for (int value = 1; value <= 1000; ++value) {
        ReloadTransactionGroup group{"two-tables"};
        group.add(replace(left, value));
        group.add(replace(right, value, {}, {left}));
        coordinator.enqueue(std::move(group));
        coordinator.applyFrame();
    }
    running = false;
    observer.join();
    REQUIRE_FALSE(saw_half_commit);
    const auto final = coordinator.registry().snapshot();
    REQUIRE(final.find(left)->content_revision == 1001);
    REQUIRE(final.find(right)->content_revision == 1001);
}

TEST_CASE("ReloadService status preserves HR0 fields and adds resource counters and error",
          "[wp98][status]") {
    if (!logger) setupLogger();
    ReloadService service;
    const auto resource = service.transactions().registry().declareResource(
        "fake", makeAssetKey("fake/a.bin"), number(1));
    ReloadTransactionGroup group{"fake/a.bin"};
    group.add(ReloadActor{
        .name = "fake",
        .target = resource,
        .validate = [] { throw std::runtime_error("invalid fake candidate"); },
        .stage = [] { return StagedResourceData{number(2)}; },
    });
    service.transactions().enqueue(std::move(group));
    service.applyFrame();
    const auto json = service.statusJson();
    REQUIRE(json.contains("state"));
    REQUIRE(json.contains("epoch"));
    REQUIRE(json.contains("error"));
    REQUIRE(json.at("applied") == 0);
    REQUIRE(json.at("failed") == 1);
    REQUIRE(json.at("last_reload_error").at("path") == "fake/a.bin");
    REQUIRE(json.at("last_reload_error").at("kind") == "transaction");
    REQUIRE(json.at("last_reload_error").at("message") == "invalid fake candidate");
}

} // namespace Pelican::watch
