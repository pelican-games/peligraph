#include "renderfallback.hpp"

#include <utility>

namespace PelicanStudio {

bool RenderFallbackSession::observeStatus(const Json &reply) {
    auto reading = readRenderPipeline(reply);
    // An engine before BS-1 says nothing either way: nothing changes.
    if (reading.state == RenderPipelineReading::State::Unknown) return false;
    // The project's configuration is published: what a restart was asked
    // for has happened, or is no longer needed.
    if (reading.state == RenderPipelineReading::State::Ready && fallback()) {
        restart_feature_.clear();
        restart_message_.clear();
        // RR-1s: what published it published what the editor held; the
        // reads are asked again (the owner's), and until then say nothing.
        authoring_ = {};
    }
    reading_ = std::move(reading);
    return compose();
}

bool RenderFallbackSession::observeReloadRefusal(const Json &error) {
    auto feature = renderRestartFeature(error);
    if (feature.empty()) return false;
    restart_feature_ = std::move(feature);
    restart_message_ = error.is_object() ? error.value("message", std::string{}) : std::string{};
    return compose();
}

bool RenderFallbackSession::wantsStatusAfter(const Json &reply) const {
    if (!fallback() || !reply.is_object()) return false;
    if (reply.value("status", std::string{}) == "committed") return true;
    const auto committed = reply.find("committed");
    return committed != reply.end() && committed->is_boolean() && committed->get<bool>();
}

void RenderFallbackSession::engineGone() {
    reading_ = {};
    authoring_ = {};
    held_problem_ = {};
    restart_feature_.clear();
    restart_message_.clear();
    (void)compose();
}

bool RenderFallbackSession::observeAuthoringRead(const Json &reply) {
    if (!reply.is_object()) return false;
    const auto live = reply.find("live");
    // An engine before RR-1 says nothing either way.
    if (live == reply.end() || !live->is_boolean()) return false;
    AuthoringReading next;
    next.known = true;
    next.live = live->get<bool>();
    const auto unparsed = reply.find("held_unparsed");
    next.held_unparsed = unparsed != reply.end() && unparsed->is_boolean() && unparsed->get<bool>();
    if (const auto root = reply.find("source_reference"); root != reply.end() && root->is_string())
        next.root = graphRefusalSource(Json{{"document", *root}}, "document");
    if (const auto digest = reply.find("source_digest"); digest != reply.end()) {
        if (digest->is_object()) {
            const auto hex = digest->find("hex");
            if (hex != digest->end() && hex->is_string()) next.digest = hex->get<std::string>();
        } else if (digest->is_string()) {
            next.digest = digest->get<std::string>();
        }
    }
    authoring_ = std::move(next);
    return compose();
}

bool RenderFallbackSession::observeProblems(const std::vector<ProblemRow> &rows) {
    HeldProblem next;
    for (const auto &row : rows) {
        // The start's refusal is the placeholder's; a shader's is not one of
        // the documents the editor holds.
        if (row.origin != ProblemRow::Origin::Engine || !row.error() || row.source == "startup" ||
            row.document_kind == "shader")
            continue;
        next = HeldProblem{row.sentence, row.code, row.document, row.node, row.line, row.column};
        break;
    }
    if (next == held_problem_) return false;
    held_problem_ = std::move(next);
    return compose();
}

std::string RenderFallbackSession::restartReason() const {
    if (restart_feature_.empty()) return {};
    return "the rendering setup enables " + restart_feature_ + ", which this engine was started without";
}

bool RenderFallbackSession::compose() {
    Notice next;
    if (!restart_feature_.empty()) {
        // Decision 4: the configuration on disk is fixed; only a restart
        // makes the module it needs.
        next.shown = true;
        next.restart_feature = restart_feature_;
        next.line = "Restart the engine to enable " + restart_feature_ +
                    ": the rendering setup needs it, and this engine was started without it.";
        next.detail = restart_message_;
    } else if (fallback()) {
        next.shown = true;
        next.line = renderFallbackSentence(reading_);
        next.document = renderFallbackDocument(reading_);
        next.node = renderFallbackNode(reading_);
        const auto code = reading_.refusal.value("code", std::string{});
        const auto message = reading_.refusal.value("message", std::string{});
        next.detail = message.empty() ? code : message;
        if (!reading_.root.empty()) next.detail += "\nrendering root: " + reading_.root;
        // Decision 8: a document that is not JSON is shown to be fixed.
        if (code == "render_document_syntax@1") {
            next.syntax = true;
            const auto number = [&](const char *key) -> std::int64_t {
                const auto found = reading_.refusal.find(key);
                return found != reading_.refusal.end() && found->is_number_integer()
                           ? found->get<std::int64_t>()
                           : 0;
            };
            next.syntax_line = number("line");
            next.syntax_column = number("column");
        }
    } else if (authoring_.known && authoring_.held_unparsed) {
        // RR-1s, decision 1: the version on disk is not JSON; the reads keep
        // the last one that was. Show the document fixes it.
        next.shown = true;
        next.held = true;
        const auto &file = authoring_.root.empty() ? held_problem_.document : authoring_.root;
        next.line = (file.empty() ? std::string{"The rendering root"} : file) +
                    " is not valid JSON; the Graph shows the last version that was.";
        next.document = file;
        next.syntax = !file.empty();
        if (held_problem_.code == "render_document_syntax@1") {
            next.syntax_line = held_problem_.line;
            next.syntax_column = held_problem_.column;
        }
        next.detail = held_problem_.sentence;
    } else if (authoring_.known && !authoring_.live) {
        // RR-1s, decision 1: a reload refused the version on disk; the Graph
        // and every structural read show it, the picture is the one before.
        next.shown = true;
        next.held = true;
        next.line = "Showing the saved version; the picture is still the previous one";
        next.line += held_problem_.sentence.empty() ? std::string{"."} : " \xE2\x80\x94 " + held_problem_.sentence;
        next.document = held_problem_.document;
        next.node = held_problem_.node;
        next.detail = "The Graph and the other panels show the version on disk and edit it; the engine draws "
                      "the version before until a fixed version is taken in.";
    }
    if (next == notice_) return false;
    notice_ = std::move(next);
    return true;
}

std::optional<RenderDocumentSyntaxSpot> renderDocumentSyntaxOf(std::string_view text) {
    try {
        (void)nlohmann::json::parse(text);
        return std::nullopt;
    } catch (const nlohmann::json::parse_error &error) {
        RenderDocumentSyntaxSpot spot;
        std::string reason = error.what();
        // "[json.exception.parse_error.101] parse error at line L, column C: <reason>"
        constexpr std::string_view at_line = "at line ";
        if (const auto at = reason.find(at_line); at != std::string::npos) {
            std::size_t cursor = at + at_line.size();
            const auto number = [&]() -> std::int64_t {
                std::int64_t value = 0;
                bool any = false;
                while (cursor < reason.size() && reason[cursor] >= '0' && reason[cursor] <= '9') {
                    value = value * 10 + (reason[cursor] - '0');
                    ++cursor;
                    any = true;
                }
                return any ? value : 0;
            };
            spot.line = number();
            constexpr std::string_view at_column = ", column ";
            if (reason.compare(cursor, at_column.size(), at_column) == 0) {
                cursor += at_column.size();
                spot.column = number();
            }
            if (reason.compare(cursor, 2, ": ") == 0) cursor += 2;
            reason.erase(0, cursor);
        }
        spot.reason = std::move(reason);
        return spot;
    }
}

RenderDocumentRepair::RenderDocumentRepair(Transport transport) : transport_{std::move(transport)} {}

std::int64_t RenderDocumentRepair::send(const std::string &method, const Json &params) {
    std::string error;
    const auto id = transport_.request ? transport_.request(method, params, &error) : 0;
    if (id == 0)
        notice_ = "Could not ask the engine (" + method + ")" + (error.empty() ? std::string{"."} : ": " + error);
    return id;
}

void RenderDocumentRepair::notify() {
    if (changed) changed();
}

bool RenderDocumentRepair::open(const std::string &document, std::int64_t line, std::int64_t column) {
    ++epoch_;
    wanted_ = graphRefusalSource(Json{{"document", document}}, "document");
    reference_.clear();
    digest_.clear();
    content_.clear();
    ticket_.clear();
    poll_request_ = 0;
    restore_request_ = 0;
    line_ = line;
    column_ = column;
    notice_.clear();
    list_request_ = send("get_render_documents", Json::object());
    state_ = list_request_ != 0 ? State::Reading : State::Closed;
    notify();
    return list_request_ != 0;
}

bool RenderDocumentRepair::save(const std::string &content) {
    if (state_ != State::Editing) return false;
    if (const auto spot = renderDocumentSyntaxOf(content)) {
        notice_ = "Not valid JSON yet";
        if (spot->line > 0)
            notice_ += " at line " + std::to_string(spot->line) + ", column " + std::to_string(spot->column);
        notice_ += ": " + spot->reason;
        notify();
        return false;
    }
    notice_.clear();
    // RR-1s, decision 2: after a refused reload as on the placeholder, the
    // digest listed is the engine's base.
    restore_request_ = send("restore_render_documents",
                            Json{{"documents", Json::array({Json{{"reference", reference_},
                                                                 {"base_digest", digest_},
                                                                 {"content", content}}})}});
    if (restore_request_ != 0) state_ = State::Saving;
    notify();
    return restore_request_ != 0;
}

void RenderDocumentRepair::close() {
    ++epoch_;
    state_ = State::Closed;
    list_request_ = 0;
    restore_request_ = 0;
    poll_request_ = 0;
    ticket_.clear();
    notice_.clear();
    notify();
}

void RenderDocumentRepair::engineGone() {
    if (state_ == State::Closed) return;
    close();
}

void RenderDocumentRepair::answered(const Json &result, bool *taken) {
    const auto status = result.is_object() ? result.value("status", std::string{}) : std::string{};
    if (status == "accepted" && !result.value("ticket", std::string{}).empty()) {
        ticket_ = result.value("ticket", std::string{});
    }
    if (status == "accepted" || status == "pending") {
        if (ticket_.empty() || !transport_.schedule) {
            state_ = State::Editing;
            notice_ = "The engine took the document but its ticket cannot be followed.";
            return;
        }
        const auto epoch = epoch_;
        transport_.schedule(TicketPollMs, [this, epoch] {
            if (epoch != epoch_ || ticket_.empty()) return;
            poll_request_ = send("get_edit_result", Json{{"ticket", ticket_}});
            if (poll_request_ == 0) {
                state_ = State::Editing;
                notify();
            }
        });
        return;
    }
    ticket_.clear();
    if (status == "committed") {
        // The owner reads the commit as it reads every commit.
        *taken = false;
        state_ = State::Closed;
        notice_.clear();
        if (restored) restored(result);
        return;
    }
    // Refused: said in one sentence, the document stays open.
    state_ = State::Editing;
    const auto error = result.is_object() ? result.value("error", Json::object()) : Json::object();
    auto sentence = renderDocumentSyntaxDescription(error);
    if (sentence.empty())
        sentence = knownRefusalDescription(error.value("code", std::string{}), error.value("message", std::string{}));
    if (sentence.empty()) sentence = error.value("message", std::string{"The engine refused the document."});
    notice_ = std::move(sentence);
}

bool RenderDocumentRepair::receiveResult(std::int64_t id, const Json &result) {
    if (!owns(id)) return false;
    bool taken = true;
    if (id == list_request_) {
        list_request_ = 0;
        const auto documents = result.is_object() ? result.find("documents") : result.end();
        if (documents != result.end() && documents->is_array())
            for (const auto &document : *documents) {
                if (!document.is_object()) continue;
                const auto reference = document.value("reference", std::string{});
                if (graphRefusalSource(Json{{"document", reference}}, "document") != wanted_) continue;
                reference_ = reference;
                digest_ = document.value("digest", std::string{});
                content_ = document.value("content", std::string{});
                break;
            }
        if (reference_.empty()) {
            state_ = State::Closed;
            notice_ = "The engine does not list " + wanted_ + " among the render documents.";
        } else {
            state_ = State::Editing;
        }
    } else {
        if (id == restore_request_) restore_request_ = 0;
        if (id == poll_request_) poll_request_ = 0;
        answered(result, &taken);
    }
    notify();
    return taken;
}

bool RenderDocumentRepair::receiveFailure(std::int64_t id, const std::string &message) {
    if (!owns(id)) return false;
    if (id == list_request_) {
        list_request_ = 0;
        state_ = State::Closed;
        notice_ = "Could not read the render documents: " + message;
    } else {
        restore_request_ = id == restore_request_ ? 0 : restore_request_;
        poll_request_ = id == poll_request_ ? 0 : poll_request_;
        ticket_.clear();
        state_ = State::Editing;
        notice_ = "Could not save the document: " + message;
    }
    notify();
    return true;
}

} // namespace PelicanStudio
