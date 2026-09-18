#include "../src/core/watch/reloadservice.hpp"
#include "../src/core/watch/reloadtransaction.hpp"
#include "../src/core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <array>
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

TEST_CASE("Prepared reload can join another publication without exposing intermediate resources", "[wp98][reload-prepare]") {
    ReloadCoordinator coordinator;
    const auto ref = coordinator.registry().declareResource("models", makeAssetKey("mesh.glb"), number(1));
    int domain = 1;
    bool rolled_back = false;
    const auto group = [&] {
        ReloadTransactionGroup result{"paired"};
        result.add(replace(ref, 2));
        result.setPreparePublish([] {});
        result.setPublish([&]() noexcept { domain = 2; });
        result.setRollback([&]() noexcept { rolled_back = true; });
        return result;
    };
    {
        auto candidate = coordinator.prepare(group());
        candidate->preparePublication();
        REQUIRE(*coordinator.registry().snapshot().find(ref)->payloadAs<int>() == 1);
        REQUIRE(domain == 1);
    }
    REQUIRE(rolled_back);
    REQUIRE(*coordinator.registry().snapshot().find(ref)->payloadAs<int>() == 1);
    auto candidate = coordinator.prepare(group());
    candidate->preparePublication();
    candidate->commit();
    REQUIRE(*coordinator.registry().snapshot().find(ref)->payloadAs<int>() == 2);
    REQUIRE(domain == 2);
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

// Coordinator mechanics only: the actors, prepare, publish and rollback below
// are test lambdas, so nothing here reaches ModelAssetContainer. The production
// acceptance for the model participant lives in modelhotreload_test.cpp
// ("WP382c model reload rollback ...", decision 30).
TEST_CASE("WP382c coordinator mechanics: publication prepares instance and catalog candidates before one commit",
          "[wp382c][reload-participant][cpu]") {
    ReloadCoordinator coordinator;
    const auto model = coordinator.registry().declareResource(
        "models", makeAssetKey("models/hero.glb"), number(1));
    struct PublishedState {
        int instance = 1;
        std::vector<std::string> catalog{"old"};
        std::uint64_t generation = 7;
    } published;
    struct Candidate {
        int instance = 0;
        std::vector<std::string> catalog;
    } candidate;
    int effect_sink_count = 0;
    bool prepared = false;

    ReloadTransactionGroup group{"wp382c-model"};
    group.add(replace(model, 2));
    group.setPreparePublish([&] {
        REQUIRE(*coordinator.registry().snapshot().find(model)->payloadAs<int>() == 1);
        candidate = Candidate{2, {"new"}};
        prepared = true;
        REQUIRE(effect_sink_count == 0);
    });
    group.setPublish([&]() noexcept {
        published.instance = candidate.instance;
        published.catalog.swap(candidate.catalog);
        ++published.generation;
    });
    group.setRollback([&]() noexcept { candidate = {}; });
    coordinator.enqueue(std::move(group));

    REQUIRE(coordinator.applyFrame() == 1);
    REQUIRE(prepared);
    REQUIRE(*coordinator.registry().snapshot().find(model)->payloadAs<int>() == 2);
    REQUIRE(published.instance == 2);
    REQUIRE(published.catalog == std::vector<std::string>{"new"});
    REQUIRE(published.generation == 8);
    REQUIRE(effect_sink_count == 0);
}

// Coordinator mechanics only: the failure below is thrown by a test lambda in
// prepare_publish, not by ModelAssetContainer. The production rollback
// acceptance is modelhotreload_test.cpp ("WP382c model reload rollback ...").
TEST_CASE("WP382c coordinator mechanics: failed model preparation retains registry instance and catalog",
          "[wp382c][reload-participant][rollback][cpu]") {
    ReloadCoordinator coordinator;
    const auto model = coordinator.registry().declareResource(
        "models", makeAssetKey("models/hero.glb"), number(1));
    int instance = 1;
    std::vector<std::string> catalog{"old"};
    std::vector<std::string> candidate;
    bool rolled_back = false;

    ReloadTransactionGroup group{"wp382c-model-failure"};
    group.add(replace(model, 2));
    group.setPreparePublish([&] {
        candidate = {"new"};
        throw std::runtime_error("catalog prepare failed");
    });
    group.setPublish([&]() noexcept {
        instance = 2;
        catalog.swap(candidate);
    });
    group.setRollback([&]() noexcept {
        candidate.clear();
        rolled_back = true;
    });
    coordinator.enqueue(std::move(group));

    REQUIRE(coordinator.applyFrame() == 1);
    REQUIRE(rolled_back);
    REQUIRE(*coordinator.registry().snapshot().find(model)->payloadAs<int>() == 1);
    REQUIRE(instance == 1);
    REQUIRE(catalog == std::vector<std::string>{"old"});
    REQUIRE(candidate.empty());
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

TEST_CASE("ReloadService routes requests through one named participant",
          "[r7][reload-participant]") {
    if (!logger) setupLogger();
    ReloadService service;
    const auto key = makeAssetKey("fake/participant.bin");
    const auto resource = service.transactions().registry().declareResource(
        "fake", key, number(1), {}, 0, sizeof(int));
    int retired = 0;

    service.registerParticipant(ReloadParticipant{
        .name = "fake",
        .file = FileReloadRole{
            .claims = [key](const ReloadRequest &request) { return request.key == key; },
            .apply = PerRequestReloadApply{
                .enqueue = [resource](const ReloadRequest &, ReloadCoordinator &coordinator) {
                    ReloadTransactionGroup group{"fake-participant"};
                    group.add(replace(resource, 2));
                    coordinator.enqueue(std::move(group));
                    return true;
                },
            },
            .retire = [&retired](std::shared_ptr<const void> payload,
                                 ReloadCoordinator &) noexcept {
                retired = *std::static_pointer_cast<const int>(std::move(payload));
                return true;
            },
        },
    });

    REQUIRE(service.participantNames() == std::vector<std::string>{"fake"});
    REQUIRE(service.applyRequestForTesting({key, ReloadKind::modified, {}, 1}));
    REQUIRE(*service.transactions().registry().snapshot().find(resource)->payloadAs<int>() == 2);
    REQUIRE(retired == 1);
    REQUIRE(service.applyRequestForTesting(
        {makeAssetKey("fake/unclaimed.bin"), ReloadKind::modified, {}, 1}));
    REQUIRE(service.unregisterParticipant("fake"));
    REQUIRE_FALSE(service.unregisterParticipant("fake"));
}

TEST_CASE(
    "ReloadService coalesces one participant's dependency changes into one batch",
    "[wp196][reload-participant][batch][coalesce]") {
    if (!logger) setupLogger();
    ReloadService service;
    const auto root =
        makeAssetKey("passes/main.json");
    const auto feature =
        makeAssetKey("features/lighting.json");
    std::size_t apply_count = 0;
    std::vector<AssetKey> received;

    service.registerParticipant(ReloadParticipant{
        .name = "fake.pipeline",
        .file = FileReloadRole{
            .claims =
                [root, feature](
                    const ReloadRequest &request) {
                    return request.key == root ||
                           request.key == feature;
                },
            .apply = BatchReloadApply{
                .apply_batch =
                    [&apply_count, &received](
                        std::span<const ReloadRequest>
                            requests) {
                        ++apply_count;
                        for (const auto &request : requests) {
                            received.push_back(request.key);
                        }
                        return true;
                    },
            },
        },
    });

    const std::array requests{
        ReloadRequest{root, ReloadKind::modified,
                      {}, 1},
        ReloadRequest{feature,
                      ReloadKind::modified, {}, 1},
        ReloadRequest{
            makeAssetKey("unclaimed.txt"),
            ReloadKind::modified, {}, 1},
    };
    const auto results =
        service.applyRequestsForTesting(requests);

    REQUIRE(results ==
            std::vector<bool>{true, true, true});
    REQUIRE(apply_count == 1);
    REQUIRE(received ==
            std::vector<AssetKey>{root, feature});
}

TEST_CASE(
    "ReloadService gives one batch owner selected companion requests atomically",
    "[wp222][reload-participant][batch][companions]") {
    // The two variants only select which requests the owner claims; every
    // assertion lives in the shared body below.  Sections that carry no
    // assertion of their own are rejected by `-w NoAssertions`, so the choice
    // is a generator run, not a section.
    const bool shared = GENERATE(false, true);
    CAPTURE(shared);
    if (!logger) setupLogger();
    ReloadService service;
    const auto graph =
        makeAssetKey("passes/main.json");
    const auto shader =
        makeAssetKey("shaders/main.surface");
    const auto texture =
        makeAssetKey("textures/base.png");
    std::size_t coordinated_applies = 0;
    std::vector<AssetKey> coordinated_owned;
    std::vector<AssetKey> coordinated_companions;
    std::vector<AssetKey> standalone_assets;

    service.registerParticipant(ReloadParticipant{
        .name = "fake.assets",
        .file = FileReloadRole{
            .claims =
                [shader, texture](
                    const ReloadRequest &request) {
                    return request.key == shader ||
                           request.key == texture;
                },
            .apply = BatchReloadApply{
                .apply_batch =
                    [&standalone_assets](
                        std::span<const ReloadRequest>
                            requests) {
                        for (const auto &request : requests) {
                            standalone_assets.push_back(
                                request.key);
                        }
                        return true;
                    },
            },
        },
    });
    service.registerParticipant(ReloadParticipant{
        .name = "fake.pipeline",
        .file = FileReloadRole{
            .claims =
                [graph, shader, shared](const ReloadRequest &request) {
                    return request.key == graph || (shared && request.key == shader);
                },
            .apply = CompanionBatchReloadApply{
                .companion_participants = {
                    "fake.assets"},
                .companion_claims =
                    [shader](
                        std::string_view,
                        const ReloadRequest &request) {
                        return request.key == shader;
                    },
                .apply =
                    [&](
                        std::span<const ReloadRequest> owned,
                        std::span<const ReloadRequest>
                            companions) {
                        ++coordinated_applies;
                        for (const auto &request : owned) {
                            coordinated_owned.push_back(
                                request.key);
                        }
                        for (const auto &request :
                             companions) {
                            coordinated_companions.push_back(
                                request.key);
                        }
                        return true;
                    },
            },
        },
    });

    const std::array requests{
        ReloadRequest{shader, ReloadKind::modified,
                      {}, 1},
        ReloadRequest{graph, ReloadKind::modified,
                      {}, 1},
        ReloadRequest{texture,
                      ReloadKind::modified, {}, 1},
    };
    REQUIRE(
        service.applyRequestsForTesting(requests) ==
        std::vector<bool>{true, true, true});
    // A companion owner has no standalone batch route at all now: the type
    // says the coordinated call is the only way its requests are delivered.
    REQUIRE(coordinated_applies == 1);
    REQUIRE(coordinated_owned ==
            (shared ? std::vector<AssetKey>{shader, graph} : std::vector<AssetKey>{graph}));
    REQUIRE(coordinated_companions ==
            std::vector<AssetKey>{shader});
    REQUIRE(standalone_assets ==
            std::vector<AssetKey>{texture});
}

TEST_CASE("ReloadService rejects ambiguous participant claims before enqueue",
          "[r7][reload-participant]") {
    if (!logger) setupLogger();
    ReloadService service;
    const auto key = makeAssetKey("fake/ambiguous.bin");
    int enqueued = 0;
    for (const auto *name : {"left", "right"}) {
        service.registerParticipant(ReloadParticipant{
            .name = name,
            .file = FileReloadRole{
                .claims = [key](const ReloadRequest &request) { return request.key == key; },
                .apply = PerRequestReloadApply{
                    .enqueue = [&enqueued](const ReloadRequest &, ReloadCoordinator &) {
                        ++enqueued;
                        return true;
                    },
                },
            },
        });
    }

    REQUIRE_FALSE(service.applyRequestForTesting({key, ReloadKind::modified, {}, 1}));
    REQUIRE(enqueued == 0);
    REQUIRE_THROWS(service.registerParticipant(ReloadParticipant{
        .name = "left",
        .file = FileReloadRole{
            .claims = [](const ReloadRequest &) { return false; },
            .apply = PerRequestReloadApply{
                .enqueue = [](const ReloadRequest &, ReloadCoordinator &) { return false; },
            },
        },
    }));
    // A companion owner must name someone other than itself.
    REQUIRE_THROWS(service.registerParticipant(ReloadParticipant{
        .name = "self.companion",
        .file = FileReloadRole{
            .claims = [](const ReloadRequest &) { return false; },
            .apply = CompanionBatchReloadApply{
                .companion_participants = {"self.companion"},
                .apply = [](std::span<const ReloadRequest>,
                            std::span<const ReloadRequest>) { return true; },
            },
        },
    }));
}

TEST_CASE(
    "ReloadService applies nested batch owners outermost first whatever the registration order",
    "[c7][reload-participant][batch][companions]") {
    // "outer" names "inner" as a companion, and "inner" names "leaf". The
    // inner owner must not absorb its own requests before the outer owner has
    // had them, no matter which participant reached the service first.
    const bool inner_first = GENERATE(false, true);
    CAPTURE(inner_first);
    if (!logger) setupLogger();
    ReloadService service;
    const auto outer_key = makeAssetKey("passes/outer.json");
    const auto inner_key = makeAssetKey("shaders/inner.surface");
    const auto leaf_key = makeAssetKey("materials/leaf.json");
    std::vector<AssetKey> outer_owned;
    std::vector<AssetKey> outer_companions;
    std::vector<AssetKey> inner_owned;
    std::vector<AssetKey> inner_companions;
    std::vector<AssetKey> leaf_standalone;

    const auto register_leaf = [&] {
        service.registerParticipant(ReloadParticipant{
            .name = "leaf",
            .file = FileReloadRole{
                .claims = [leaf_key](const ReloadRequest &request) {
                    return request.key == leaf_key;
                },
                .apply = BatchReloadApply{
                    .apply_batch = [&leaf_standalone](
                                       std::span<const ReloadRequest> requests) {
                        for (const auto &request : requests) {
                            leaf_standalone.push_back(request.key);
                        }
                        return true;
                    },
                },
            },
        });
    };
    const auto register_inner = [&] {
        service.registerParticipant(ReloadParticipant{
            .name = "inner",
            .file = FileReloadRole{
                .claims = [inner_key](const ReloadRequest &request) {
                    return request.key == inner_key;
                },
                .apply = CompanionBatchReloadApply{
                    .companion_participants = {"leaf"},
                    .apply = [&](std::span<const ReloadRequest> owned,
                                 std::span<const ReloadRequest> companions) {
                        for (const auto &request : owned)
                            inner_owned.push_back(request.key);
                        for (const auto &request : companions)
                            inner_companions.push_back(request.key);
                        return true;
                    },
                },
            },
        });
    };
    const auto register_outer = [&] {
        service.registerParticipant(ReloadParticipant{
            .name = "outer",
            .file = FileReloadRole{
                .claims = [outer_key](const ReloadRequest &request) {
                    return request.key == outer_key;
                },
                .apply = CompanionBatchReloadApply{
                    .companion_participants = {"inner", "leaf"},
                    .apply = [&](std::span<const ReloadRequest> owned,
                                 std::span<const ReloadRequest> companions) {
                        for (const auto &request : owned)
                            outer_owned.push_back(request.key);
                        for (const auto &request : companions)
                            outer_companions.push_back(request.key);
                        return true;
                    },
                },
            },
        });
    };

    register_leaf();
    if (inner_first) {
        register_inner();
        register_outer();
    } else {
        register_outer();
        register_inner();
    }

    const std::array requests{
        ReloadRequest{inner_key, ReloadKind::modified, {}, 1},
        ReloadRequest{outer_key, ReloadKind::modified, {}, 1},
        ReloadRequest{leaf_key, ReloadKind::modified, {}, 1},
    };
    REQUIRE(service.applyRequestsForTesting(requests) ==
            std::vector<bool>{true, true, true});
    REQUIRE(outer_owned == std::vector<AssetKey>{outer_key});
    REQUIRE(outer_companions == std::vector<AssetKey>{inner_key, leaf_key});
    REQUIRE(inner_owned.empty());
    REQUIRE(inner_companions.empty());
    REQUIRE(leaf_standalone.empty());

    // With nothing of its own to publish, the outer owner steps aside and the
    // inner owner takes its companion.
    outer_owned.clear();
    outer_companions.clear();
    const std::array inner_only{
        ReloadRequest{inner_key, ReloadKind::modified, {}, 1},
        ReloadRequest{leaf_key, ReloadKind::modified, {}, 1},
    };
    REQUIRE(service.applyRequestsForTesting(inner_only) ==
            std::vector<bool>{true, true});
    REQUIRE(outer_owned.empty());
    REQUIRE(outer_companions.empty());
    REQUIRE(inner_owned == std::vector<AssetKey>{inner_key});
    REQUIRE(inner_companions == std::vector<AssetKey>{leaf_key});
    REQUIRE(leaf_standalone.empty());
}

TEST_CASE("ReloadService schedules runtime participants at their declared boundary",
          "[r8][runtime-reload-participant]") {
    if (!logger) setupLogger();
    ReloadService service;
    std::vector<RuntimeReloadTrigger> triggers;
    service.registerParticipant(ReloadParticipant{
        .name = "fake.runtime",
        .runtime = RuntimeReloadParticipant{
            .boundary = RuntimeReloadBoundary::frame_start,
            .apply = [&triggers](RuntimeReloadTrigger trigger) {
                triggers.push_back(trigger);
                if (trigger == RuntimeReloadTrigger::poll) return RuntimeReloadResult{};
                if (trigger == RuntimeReloadTrigger::requested) {
                    return RuntimeReloadResult{.attempted = true, .committed = true};
                }
                return RuntimeReloadResult{.attempted = true, .error = "manual failure"};
            },
            .describe = [](nlohmann::json &status) { status = {{"kind", "fake"}}; },
        },
    });

    REQUIRE(service.applyRuntimeBoundary(RuntimeReloadBoundary::render_start).attempted == 0);
    REQUIRE(service.applyRuntimeBoundary(RuntimeReloadBoundary::frame_start).attempted == 0);
    REQUIRE(service.requestRuntimeReload("fake.runtime"));
    const auto requested = service.applyRuntimeBoundary(RuntimeReloadBoundary::frame_start);
    REQUIRE(requested.attempted == 1);
    REQUIRE(requested.committed == 1);
    REQUIRE(requested.failed == 0);

    const auto manual = service.applyRuntimeNow("fake.runtime");
    REQUIRE(manual.attempted);
    REQUIRE_FALSE(manual.committed);
    REQUIRE(manual.error == "manual failure");
    REQUIRE_FALSE(service.requestRuntimeReload("missing.runtime"));
    REQUIRE(triggers == std::vector<RuntimeReloadTrigger>{RuntimeReloadTrigger::poll,
                                                          RuntimeReloadTrigger::requested,
                                                          RuntimeReloadTrigger::manual});

    const auto status = service.statusJson().at("runtime").at("fake.runtime");
    REQUIRE(status.at("boundary") == "frame_start");
    REQUIRE(status.at("requested") == false);
    REQUIRE(status.at("attempted") == 2);
    REQUIRE(status.at("applied") == 1);
    REQUIRE(status.at("failed") == 1);
    REQUIRE(status.at("last_error") == "manual failure");
    REQUIRE(status.at("details").at("kind") == "fake");
}

} // namespace Pelican::watch
