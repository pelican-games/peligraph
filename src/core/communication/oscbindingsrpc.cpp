#include "oscbindingsrpc.hpp"

#include "rpcserver.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../osc/oscinput.hpp"
#include "../os/inputsequence.hpp"
#include "../os/liveinputrecording.hpp"
#include "../userpublic/userinput.hpp"
#include "../watch/problemlist.hpp"
#include "../../project/projectdocumenttransaction.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;
constexpr std::string_view problem_domain = "osc_bindings";

[[noreturn]] void invalidParams(const std::string &message) {
    throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams, message);
}

[[noreturn]] void refuse(std::string_view code, const std::string &message,
                         Json fields = Json::object()) {
    throw JsonRpcHandlerError(
        JsonRpcErrorCodes::applicationError, message,
        Json{{"error_code", std::string{code}},
             {"error_fields", std::move(fields)}});
}

std::string readBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("cannot read " + path.generic_string());
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

Json ordinary(const OrderedJson &value) { return Json::parse(value.dump()); }

Json bindingsJson(const std::vector<Osc::OscBinding> &bindings) {
    Json live = Json::array();
    for (const auto &binding : bindings) {
        Json entry{{"address", binding.address}, {"input", binding.input}};
        if (binding.has_map) {
            entry["map"] = {{"from", binding.map.from}, {"to", binding.map.to}};
        }
        live.push_back(std::move(entry));
    }
    return live;
}

std::vector<Osc::OscBinding> parseBindings(const OrderedJson &document) {
    return Osc::parseOscBindings(ordinary(document),
                                 GET_MODULE(LiveInputs).declarations());
}

struct ActiveProfile {
    std::string name;
    std::string reference;
    std::unordered_map<std::string, std::string> refs;
};

std::optional<ActiveProfile> activeProfile(ProjectBasicConfig &config) {
    const auto active = internal::activeInputProfile();
    if (!active) return std::nullopt;
    auto refs = config.inputProfileJsonRefs();
    const auto found = refs.find(*active);
    if (found == refs.end()) return std::nullopt;
    return ActiveProfile{*active, found->second, std::move(refs)};
}

std::filesystem::path writableProfilePath(const PathResolver &paths,
                                          const ActiveProfile &profile) {
    const auto refuse_path = [&] {
        refuse("input_profile.not_writable@1",
               "edit_osc_bindings: the active input profile is not a regular writable project file",
               {{"profile", profile.name}, {"path", profile.reference}});
    };
    const std::filesystem::path relative{profile.reference};
    if (profile.reference.empty() || profile.reference.find("://") != std::string::npos ||
        relative.is_absolute())
        refuse_path();
    for (const auto &part : relative) {
        if (part == "..") refuse_path();
    }
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(paths.projectRoot(), error);
    if (error) refuse_path();
    const auto candidate = std::filesystem::weakly_canonical(
        paths.projectRoot() / relative.lexically_normal(), error);
    if (error) refuse_path();
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() || *root_it != *candidate_it) refuse_path();
    }
    const auto status = std::filesystem::symlink_status(candidate, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        refuse_path();
    const auto permissions = status.permissions();
    const auto writable_bits = std::filesystem::perms::owner_write |
                               std::filesystem::perms::group_write |
                               std::filesystem::perms::others_write;
    if ((permissions & writable_bits) == std::filesystem::perms::none) refuse_path();
    std::ofstream writable{candidate, std::ios::binary | std::ios::app};
    if (!writable) refuse_path();
    return candidate;
}

std::string renderDocument(const OrderedJson &document,
                           std::string_view previous) {
    auto text = document.dump(2);
    if (previous.ends_with('\n')) text += '\n';
    if (previous.find("\r\n") == std::string_view::npos) return text;
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 16);
    for (const char ch : text) {
        if (ch == '\n') crlf += '\r';
        crlf += ch;
    }
    return crlf;
}

void requireOnly(const Json &params,
                 std::span<const std::string_view> allowed) {
    for (const auto &[key, value] : params.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            invalidParams("edit_osc_bindings does not take '" + key + "'");
    }
}

Json readBindings(PathResolver &paths, ProjectBasicConfig &config,
                  OscInput &osc_input) {
    const auto profile = activeProfile(config);
    if (!profile) {
        return {{"profile", nullptr}, {"digest", nullptr},
                {"live", Json::array()}, {"applied_live", Json::array()}};
    }
    const auto path = writableProfilePath(paths, *profile);
    const auto bytes = readBytes(path);
    const auto document = OrderedJson::parse(bytes);
    parseBindings(document);
    Json live = document.contains("live") ? ordinary(document["live"]) : Json::array();
    return {{"profile", {{"name", profile->name}, {"reference", profile->reference}}},
            {"digest", projectDocumentDigest(bytes)},
            {"live", std::move(live)},
            {"applied_live", bindingsJson(osc_input.snapshotBindings())}};
}

Json editBindings(const Json &params, PathResolver &paths,
                  ProjectBasicConfig &config, OscInput &osc_input,
                  InputSequenceRuntime &input_sequence, ProblemList &problems) {
    if (!params.is_object()) invalidParams("edit_osc_bindings params must be an object");
    static constexpr std::array<std::string_view, 3> keys{
        "profile", "base_digest", "live"};
    requireOnly(params, keys);
    const auto base = params.find("base_digest");
    const auto requested_live = params.find("live");
    const auto requested_profile = params.find("profile");
    if (base == params.end() || !base->is_string())
        invalidParams("edit_osc_bindings requires string 'base_digest'");
    if (requested_live == params.end() || !requested_live->is_array())
        invalidParams("edit_osc_bindings requires array 'live'");
    if (requested_profile == params.end() || !requested_profile->is_object() ||
        !requested_profile->contains("name") ||
        !(*requested_profile)["name"].is_string() ||
        !requested_profile->contains("reference") ||
        !(*requested_profile)["reference"].is_string())
        invalidParams("edit_osc_bindings requires the profile returned by get_osc_bindings");

    const auto active = activeProfile(config);
    if (!active) {
        refuse("input_profile.none_active@1",
               "edit_osc_bindings: this project has no active input profile");
    }
    if ((*requested_profile)["name"].get<std::string>() != active->name ||
        (*requested_profile)["reference"].get<std::string>() != active->reference) {
        refuse("input_profile.profile_changed@1",
               "edit_osc_bindings: the active input profile changed",
               {{"expected", *requested_profile},
                {"actual", {{"name", active->name}, {"reference", active->reference}}}});
    }
    const auto path = writableProfilePath(paths, *active);
    const auto previous_bytes = readBytes(path);
    const auto previous_digest = projectDocumentDigest(previous_bytes);
    if (previous_digest != base->get_ref<const std::string &>()) {
        refuse("external_modification",
               "edit_osc_bindings: input profile changed since it was read",
               {{"expected", *base}, {"actual", previous_digest}});
    }

    OrderedJson previous_document;
    OrderedJson document;
    try {
        previous_document = OrderedJson::parse(previous_bytes);
        document = previous_document;
        document["live"] = OrderedJson::parse(requested_live->dump());
        parseBindings(document);
    } catch (const std::exception &error) {
        refuse("input_profile.bindings_invalid@1",
               "edit_osc_bindings: " + std::string{error.what()},
               {{"reason", error.what()}});
    }
    const Json previous = previous_document.contains("live")
                              ? ordinary(previous_document["live"])
                              : Json::array();
    Json answer{{"committed", true}, {"changed", false},
                {"digest", previous_digest}, {"previous_digest", previous_digest},
                {"previous", previous}, {"applied", *requested_live}};
    if (document.contains("live") && previous_document.contains("live") &&
        document["live"] == previous_document["live"]) {
        answer["applied"] = previous;
        return answer;
    }
    if (!previous_document.contains("live") && requested_live->empty()) {
        answer["applied"] = Json::array();
        return answer;
    }
    const auto still_active = activeProfile(config);
    if (!still_active || still_active->name != active->name ||
        still_active->reference != active->reference) {
        refuse("input_profile.profile_changed@1",
               "edit_osc_bindings: the active input profile changed before commit");
    }
    const auto &live_recording = GET_MODULE(LiveInputRecordingRuntime);
    if (input_sequence.isRecording() || input_sequence.isReplaying() ||
        live_recording.isRecording() || live_recording.isReplaying()) {
        refuse("input_profile.edit_during_replay@1",
               "edit_osc_bindings: bindings cannot change while input is recording or replaying");
    }

    const auto bytes = renderDocument(document, previous_bytes);
    const std::array documents{ProjectCandidateDocument{
        .reference = active->reference,
        .normalized_reference = active->reference,
        .path = path,
        .operation = ProjectDocumentOperation::replace,
        .expected = {ProjectDocumentExistence::present, previous_digest},
        .bytes = bytes,
    }};
    try {
        auto lock = ProjectDocumentTransactionLock::acquire(paths.projectRoot());
        (void)commitProjectDocuments(lock, documents);
    } catch (const ProjectDocumentExternalModification &error) {
        std::string actual;
        try {
            actual = projectDocumentDigest(readBytes(path));
        } catch (...) {
        }
        refuse("external_modification", error.what(),
               {{"expected", *base},
                {"actual", actual.empty() ? Json{nullptr} : Json{actual}}});
    } catch (const ProjectDocumentTransactionBusy &error) {
        refuse("input_profile.transaction_busy@1", error.what(),
               {{"path", error.path().generic_string()}});
    }
    answer["changed"] = true;
    answer["digest"] = projectDocumentDigest(bytes);
    config.refreshInputProfileJsons(active->reference, bytes);

    std::vector<std::string> names;
    for (const auto &[name, reference] : active->refs) {
        if (reference == active->reference) names.push_back(name);
    }
    try {
        internal::replaceInputProfileOscBindings(names, ordinary(document));
        problems.clear(problem_domain);
    } catch (const std::exception &error) {
        answer["apply_error"] = error.what();
        problems.replace(problem_domain,
                         {Json{{"severity", "error"},
                               {"source", problemSourceEditRpc},
                               {"code", "input_profile.apply_failed@1"},
                               {"message", error.what()},
                               {"fields", {{"path", active->reference}}}}});
    }
    return answer;
}

} // namespace

void configureOscBindingsRpcHandlers(
    RpcServer &server, PathResolver &path_resolver,
    ProjectBasicConfig &project_config, OscInput &osc_input,
    InputSequenceRuntime &input_sequence, ProblemList &problems) {
    server.setHandler("get_osc_bindings",
        [&](const Json &params) {
            if (!params.is_null() && !(params.is_object() && params.empty()))
                invalidParams("get_osc_bindings takes no parameters");
            try {
                return readBindings(path_resolver, project_config, osc_input);
            } catch (const JsonRpcHandlerError &) {
                throw;
            } catch (const std::exception &error) {
                refuse("input_profile.bindings_invalid@1", error.what(),
                       {{"reason", error.what()}});
            }
        });
    server.setHandler("edit_osc_bindings",
        [&](const Json &params) {
            return editBindings(params, path_resolver, project_config, osc_input,
                                input_sequence, problems);
        });
}

} // namespace Pelican
