#include "graphfiltersrpc.hpp"

#include "rpcserver.hpp"
#include "../loader/pathresolver.hpp"
#include "../../project/graphfiltersdocument.hpp"
#include "../../project/projectdocumenttransaction.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>

namespace Pelican {
namespace {

using Json = nlohmann::json;
constexpr std::string_view getMethod = "get_graph_filters";
constexpr std::string_view editMethod = "edit_graph_filters";

[[noreturn]] void invalidParams(const std::string &message) {
    throw JsonRpcHandlerError(JsonRpcErrorCodes::invalidParams, message);
}

[[noreturn]] void refuse(const std::string &message, std::string_view code, Json fields) {
    throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError, message,
                              Json{{"error_code", std::string{code}}, {"error_fields", std::move(fields)}});
}

struct DiskRead {
    enum class State { missing, present, unreadable } state = State::missing;
    std::string bytes;
    std::string error;
};

DiskRead readDisk(const std::filesystem::path &path) {
    DiskRead read;
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        read.state = DiskRead::State::unreadable;
        read.error = "could not inspect " + path.generic_string() + ": " + ec.message();
        return read;
    }
    if (!exists) return read;
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        read.state = DiskRead::State::unreadable;
        read.error = "could not read " + path.generic_string();
        return read;
    }
    read.bytes = std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        read.state = DiskRead::State::unreadable;
        read.error = "could not read " + path.generic_string();
        return read;
    }
    read.state = DiskRead::State::present;
    return read;
}

Json documentJson(const GraphFilters &document) {
    return Json::parse(renderGraphFilters(document));
}

Json currentDigest(const std::filesystem::path &path) {
    const auto read = readDisk(path);
    if (read.state != DiskRead::State::present) return nullptr;
    return projectDocumentDigest(read.bytes);
}

Json getFilters(const PathResolver &resolver) {
    const auto read = readDisk(resolver.projectRoot() / "graph_filters.json");
    Json answer{{"digest", nullptr}, {"document", nullptr}, {"error", nullptr}};
    if (read.state == DiskRead::State::unreadable) {
        answer["error"] = read.error;
        return answer;
    }
    if (read.state == DiskRead::State::missing) return answer;
    answer["digest"] = projectDocumentDigest(read.bytes);
    try {
        auto parsed = parseGraphFilters(Json::parse(read.bytes));
        if (!parsed.document) answer["error"] = parsed.error;
        else answer["document"] = documentJson(*parsed.document);
    } catch (const std::exception &error) {
        answer["error"] = std::string{"invalid JSON: "} + error.what();
    }
    return answer;
}

Json editFilters(const Json &params, PathResolver &resolver) {
    if (!params.is_object()) invalidParams(std::string{editMethod} + " params must be an object");
    for (const auto &[key, value] : params.items()) {
        (void)value;
        if (key != "base_digest" && key != "document")
            invalidParams(std::string{editMethod} + " does not take '" + key + "'");
    }
    const auto base = params.find("base_digest");
    if (base == params.end() || (!base->is_null() && !base->is_string()))
        invalidParams(std::string{editMethod} + " requires 'base_digest' to be a string or null");
    const auto candidate = params.find("document");
    if (candidate == params.end() || !candidate->is_object())
        invalidParams(std::string{editMethod} + " requires object 'document'");

    auto parsed_candidate = parseGraphFilters(*candidate);
    if (!parsed_candidate.document)
        refuse(std::string{editMethod} + ": " + parsed_candidate.error, graphFiltersInvalidCode,
               {{"reason", parsed_candidate.error}});

    const auto path = resolver.projectRoot() / "graph_filters.json";
    auto previous = readDisk(path);
    if (previous.state == DiskRead::State::unreadable)
        refuse(std::string{editMethod} + ": " + previous.error, graphFiltersInvalidCode,
               {{"reason", previous.error}});
    const Json actual = previous.state == DiskRead::State::present
                            ? Json(projectDocumentDigest(previous.bytes)) : Json(nullptr);
    if (*base != actual)
        refuse(std::string{editMethod} + ": graph_filters.json changed since it was read",
               graphFiltersExternalModificationCode, {{"expected", *base}, {"actual", actual}});

    std::optional<GraphFilters> previous_document;
    if (previous.state == DiskRead::State::present) {
        try {
            auto parsed = parseGraphFilters(Json::parse(previous.bytes));
            if (!parsed.document)
                refuse(std::string{editMethod} + ": existing graph_filters.json: " + parsed.error,
                       graphFiltersInvalidCode, {{"reason", parsed.error}});
            previous_document = std::move(parsed.document);
        } catch (const JsonRpcHandlerError &) {
            throw;
        } catch (const std::exception &error) {
            const std::string reason = std::string{"existing graph_filters.json contains invalid JSON: "} + error.what();
            refuse(std::string{editMethod} + ": " + reason, graphFiltersInvalidCode, {{"reason", reason}});
        }
    }

    const std::string bytes = renderGraphFilters(*parsed_candidate.document);
    const std::string previous_digest = actual.is_null() ? std::string{} : actual.get<std::string>();
    Json answer{{"committed", true},
                {"previous_digest", actual},
                {"previous", previous_document ? documentJson(*previous_document) : Json(nullptr)},
                {"document", documentJson(*parsed_candidate.document)}};
    if (previous.state == DiskRead::State::present && previous.bytes == bytes) {
        answer["changed"] = false;
        answer["digest"] = actual;
        return answer;
    }

    const std::array documents{ProjectCandidateDocument{
        .reference = "graph_filters.json",
        .normalized_reference = "graph_filters.json",
        .path = path,
        .operation = previous.state == DiskRead::State::missing ? ProjectDocumentOperation::create
                                                                : ProjectDocumentOperation::replace,
        .expected = previous.state == DiskRead::State::missing
                        ? ProjectDocumentExpectedState{ProjectDocumentExistence::missing, {}}
                        : ProjectDocumentExpectedState{ProjectDocumentExistence::present, previous_digest},
        .bytes = bytes,
    }};
    try {
        (void)commitProjectDocuments(resolver.projectRoot(), documents);
    } catch (const ProjectDocumentExternalModification &error) {
        refuse(std::string{editMethod} + ": " + error.what(), graphFiltersExternalModificationCode,
               {{"expected", *base}, {"actual", currentDigest(path)}});
    } catch (const ProjectDocumentTransactionBusy &error) {
        refuse(std::string{editMethod} + ": " + error.what(), graphFiltersTransactionBusyCode,
               {{"path", error.path().generic_string()}});
    } catch (const std::runtime_error &error) {
        if (std::string_view{error.what()}.find("another render authoring transaction or recovery is active") ==
            std::string_view::npos) throw;
        refuse(std::string{editMethod} + ": " + error.what(), graphFiltersTransactionBusyCode,
               {{"path", projectDocumentTransactionDirectory(resolver.projectRoot()).generic_string()}});
    }
    answer["changed"] = true;
    answer["digest"] = projectDocumentDigest(bytes);
    return answer;
}

} // namespace

void configureGraphFiltersRpcHandlers(RpcServer &server, PathResolver &path_resolver) {
    server.setHandler(std::string{getMethod}, [&path_resolver](const Json &params) {
        if (!params.is_null() && !(params.is_object() && params.empty()))
            invalidParams(std::string{getMethod} + " takes no parameters");
        return getFilters(path_resolver);
    });
    server.setHandler(std::string{editMethod}, [&path_resolver](const Json &params) {
        return editFilters(params, path_resolver);
    });
}

} // namespace Pelican
