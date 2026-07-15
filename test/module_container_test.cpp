#include "../src/core/container.hpp"
#include "../src/core/log.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Pelican {
namespace {

std::vector<std::string> destruction_events;

DECLARE_MODULE(GraphLeaf) {
  public:
    ~GraphLeaf() { destruction_events.emplace_back("leaf"); }
};

DECLARE_MODULE(GraphRoot) {
  public:
    GraphRoot() { (void)GET_MODULE(GraphLeaf); }
    ~GraphRoot() { destruction_events.emplace_back("root"); }
};

class CycleSecond;

DECLARE_MODULE(CycleFirst) {
  public:
    CycleFirst();
};

DECLARE_MODULE(CycleSecond) {
  public:
    CycleSecond();
};

CycleFirst::CycleFirst() { (void)GET_MODULE(CycleSecond); }
CycleSecond::CycleSecond() { (void)GET_MODULE(CycleFirst); }

DECLARE_MODULE(FailingModule) {
  public:
    FailingModule() { throw std::runtime_error("injected module construction failure"); }
};

DECLARE_MODULE(ExistingModule) {
  public:
    int value = 7;
};

DECLARE_MODULE(LateModule) {};
DECLARE_MODULE(RuntimeInitializedModule) {};
DECLARE_MODULE(WorkerInitializedModule) {};

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

bool hasDependency(const ModuleGraphSnapshot &snapshot, const std::string &from,
                   const std::string &to) {
    return std::find(snapshot.dependencies.begin(), snapshot.dependencies.end(),
                     ModuleDependencyEdge{from, to}) != snapshot.dependencies.end();
}

std::string readSourceFile(const std::filesystem::path &relative_path) {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / relative_path;
    std::ifstream file{path, std::ios::binary};
    if (!file) throw std::runtime_error("failed to read source file: " + path.string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::string_view functionBody(std::string_view source, std::string_view signature) {
    const auto signature_pos = source.find(signature);
    if (signature_pos == std::string_view::npos)
        throw std::runtime_error("function signature not found: " + std::string{signature});
    const auto body_start = source.find('{', signature_pos + signature.size());
    if (body_start == std::string_view::npos)
        throw std::runtime_error("function body not found: " + std::string{signature});

    std::size_t depth = 0;
    for (std::size_t pos = body_start; pos < source.size(); ++pos) {
        if (source[pos] == '{') {
            ++depth;
        } else if (source[pos] == '}' && --depth == 0) {
            return source.substr(body_start, pos - body_start + 1);
        }
    }
    throw std::runtime_error("unterminated function body: " + std::string{signature});
}

std::size_t countOccurrences(std::string_view source, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t pos = 0; (pos = source.find(needle, pos)) != std::string_view::npos;
         pos += needle.size()) {
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE("module graph records constructor dependencies and destroys dependents first",
          "[module][lifecycle]") {
    ensureLogger();
    destruction_events.clear();
    {
        FastModuleContainer modules;
        (void)GET_MODULE(GraphLeaf);
        (void)GET_MODULE(GraphRoot);

        const auto snapshot = FastModuleContainer::graphSnapshot();
        REQUIRE(snapshot.phase == ModuleRuntimePhase::booting);
        REQUIRE_FALSE(snapshot.creation_frozen);
        REQUIRE(hasDependency(snapshot, typeid(GraphRoot).name(), typeid(GraphLeaf).name()));
        REQUIRE(FastModuleContainer::tryGet<GraphRoot>() != nullptr);
    }
    REQUIRE(destruction_events == std::vector<std::string>{"root", "leaf"});
}

TEST_CASE("module construction failures and cycles never publish partial modules",
          "[module][lifecycle]") {
    ensureLogger();
    FastModuleContainer modules;

    REQUIRE_THROWS_WITH(GET_MODULE(FailingModule), "injected module construction failure");
    REQUIRE_FALSE(FastModuleContainer::isInitialized<FailingModule>());
    REQUIRE(FastModuleContainer::tryGet<FailingModule>() == nullptr);

    REQUIRE_THROWS_WITH(GET_MODULE(CycleFirst),
                        Catch::Matchers::ContainsSubstring("Module construction cycle"));
    REQUIRE_FALSE(FastModuleContainer::isInitialized<CycleFirst>());
    REQUIRE_FALSE(FastModuleContainer::isInitialized<CycleSecond>());
}

TEST_CASE("module creation is owner-thread-only while initialized access remains available",
          "[module][thread]") {
    ensureLogger();
    FastModuleContainer modules;
    REQUIRE(GET_MODULE(ExistingModule).value == 7);

    std::exception_ptr existing_error;
    std::thread existing_reader([&] {
        try {
            if (GET_MODULE(ExistingModule).value != 7) {
                throw std::runtime_error("initialized module value changed on worker thread");
            }
        } catch (...) {
            existing_error = std::current_exception();
        }
    });
    existing_reader.join();
    REQUIRE(existing_error == nullptr);

    std::string creation_error;
    std::thread worker_creator([&] {
        try {
            (void)GET_MODULE(WorkerInitializedModule);
        } catch (const std::exception &error) {
            creation_error = error.what();
        }
    });
    worker_creator.join();
    REQUIRE_THAT(creation_error, Catch::Matchers::ContainsSubstring("owner thread"));
    REQUIRE_FALSE(FastModuleContainer::isInitialized<WorkerInitializedModule>());
}

TEST_CASE("runtime freeze and shutdown preserve reads but reject new modules", "[module][phase]") {
    ensureLogger();
    FastModuleContainer modules;
    auto *existing = &GET_MODULE(ExistingModule);

    FastModuleContainer::freezeCreation();
    REQUIRE(FastModuleContainer::phase() == ModuleRuntimePhase::running);
    REQUIRE(FastModuleContainer::isCreationFrozen());
    REQUIRE(&GET_MODULE(ExistingModule) == existing);
    REQUIRE(FastModuleContainer::tryGet<LateModule>() == nullptr);
    REQUIRE_THROWS_WITH(GET_MODULE(LateModule),
                        Catch::Matchers::ContainsSubstring("module graph was frozen"));

    FastModuleContainer::beginShutdown();
    REQUIRE(FastModuleContainer::phase() == ModuleRuntimePhase::shutting_down);
    REQUIRE(&GET_MODULE(ExistingModule) == existing);
    REQUIRE_THROWS_WITH(GET_MODULE(LateModule),
                        Catch::Matchers::ContainsSubstring("during shutdown"));
}

TEST_CASE("running phase records legal late initialization before enforcement", "[module][phase]") {
    ensureLogger();
    FastModuleContainer modules;
    (void)GET_MODULE(ExistingModule);

    FastModuleContainer::enterRunningPhase();
    REQUIRE(FastModuleContainer::phase() == ModuleRuntimePhase::running);
    REQUIRE_FALSE(FastModuleContainer::isCreationFrozen());
    (void)GET_MODULE(RuntimeInitializedModule);

    const auto snapshot = FastModuleContainer::graphSnapshot();
    REQUIRE(snapshot.initialized_after_runtime_start ==
            std::vector<std::string>{typeid(RuntimeInitializedModule).name()});
}

TEST_CASE("module lifetime scopes cannot overlap", "[module][lifecycle]") {
    ensureLogger();
    FastModuleContainer modules;
    REQUIRE_THROWS_WITH([] { FastModuleContainer nested; }(),
                        Catch::Matchers::ContainsSubstring("cannot overlap"));
}

TEST_CASE("RPC module access remains localized to its composition boundary",
          "[module][architecture]") {
    const auto source = readSourceFile("src/core/communication/rpcserver.cpp");
    const auto resolver = functionBody(source, "EngineRpcModules resolveEngineRpcModules()");
    REQUIRE(countOccurrences(source, "GET_MODULE(") > 0);
    REQUIRE(countOccurrences(source, "GET_MODULE(") ==
            countOccurrences(resolver, "GET_MODULE("));
}

TEST_CASE("main loop prepares dependencies before freezing the module graph",
          "[module][architecture]") {
    const auto source = readSourceFile("src/core/appflow/loop.cpp");
    const auto prepare = source.find("prepareRuntimeModuleGraph(modules);");
    const auto freeze = source.find("FastModuleContainer::freezeCreation();");
    REQUIRE(prepare != std::string::npos);
    REQUIRE(freeze != std::string::npos);
    REQUIRE(prepare < freeze);
    REQUIRE(source.find("FastModuleContainer::enterRunningPhase();") == std::string::npos);
}

TEST_CASE("runtime reload entry points stay behind ReloadService participants",
          "[module][architecture][reload]") {
    const auto loop = readSourceFile("src/core/appflow/loop.cpp");
    const auto renderer = readSourceFile("src/core/vkcore/renderer.cpp");
    const auto rpc = readSourceFile("src/core/communication/rpcserver.cpp");
    const auto service = readSourceFile("src/core/watch/reloadservice.cpp");

    REQUIRE(loop.find("pollConfiguredGameLogic(") == std::string::npos);
    REQUIRE(rpc.find("reloadConfiguredGameLogic(") == std::string::npos);
    REQUIRE(renderer.find("reloadModifiedSources(") == std::string::npos);
    REQUIRE(renderer.find("rebuildDirty(") == std::string::npos);
    REQUIRE(countOccurrences(service, "pollModifiedSources(") == 1);
    REQUIRE(countOccurrences(service, "reloadConfiguredGameLogicAttempt(") == 1);
}

} // namespace Pelican
