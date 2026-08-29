#include "../../../src/core/communication/editorcommandservice.hpp"
#include "../../../src/core/communication/editorrpchandlers.hpp"
#include "../../../src/core/communication/rpcserver.hpp"
#include "../../../src/core/loader/authoringscenedocument.hpp"
#include "../../../src/core/userpublic/behavior.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

struct Wp360WireParams {
    std::int32_t count{};
    std::string label;
    static constexpr auto schema = structFields(
        behaviorParamsPolicy,
        defaulted(field<&Wp360WireParams::count>("count"),
                  std::int32_t{360}),
        defaulted(field<&Wp360WireParams::label>("label"),
                  "parent-default"));
};

class Wp360WireBehavior final : public Behavior {
  public:
    using Params = Wp360WireParams;
};

constexpr std::string_view wire_scene = R"json({"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[{"name":"WP360WireProbe","components":[{"name":"camera","type":"perspective","yfov":0.731},{"name":"behavior","type":"wp360_wire_behavior","params":{}},{"name":"light","type":"point","position":[7,8,9],"intensity":6.25,"color":[0.2,0.4,0.8]},{"name":"unknown_read_only","payload":{"identity":360}}]}]}}})json";

} // namespace
} // namespace Pelican

int main(int argc, char **argv) {
    using namespace Pelican;
    if (argc != 2) {
        throw std::invalid_argument(
            "capture_parent_rpc requires one output path");
    }
    auto &registry = internal::getBehaviorRegisterer();
    (void)registry.registerBehavior<Wp360WireBehavior>(
        "wp360_wire_behavior", 1, {});
    const auto document = AuthoringSceneDocument::load(
        wire_scene, SceneRevision{360});
    const EditorCommandService service{EditorCommandServiceDependencies{
        .document = [&document]() -> const AuthoringSceneDocument & {
            return document;
        },
        .current_scene_id = [] { return std::string{"main"}; },
    }};
    EditorCommandRpcAdapter editor{service};
    std::istringstream input{
        R"json({"jsonrpc":"2.0","id":360,"method":"get_components","params":{"name":"WP360WireProbe"}})json"
        "\n"};
    std::ostringstream output;
    RpcServer server{input, output};
    configureEditorRpcHandlers(
        server, editor,
        EditorRpcHandlerHooks{.snapshot_imported = [] {},
                              .save_busy = [] { return false; }});
    server.run();

    const auto bytes = output.str();
    std::ofstream fixture{argv[1], std::ios::binary};
    if (!fixture) throw std::runtime_error("failed to open capture output");
    fixture.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!fixture) throw std::runtime_error("failed to write capture output");
}
