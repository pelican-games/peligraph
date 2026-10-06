#include "rendereditticket.hpp"
#include "rendereditcompletion.hpp"

#include <utility>

namespace PelicanStudio {
RenderEditTicket::RenderEditTicket(Transport transport,
                                 std::function<void(const std::string &)> publishDigest)
    : transport_(std::move(transport)), publish_digest_(std::move(publishDigest)) {}

RenderEditTicket::~RenderEditTicket() = default;

RenderEditTicket::Outcome RenderEditTicket::deliver(Outcome outcome) {
    if (outcome.kind == Kind::Unconfirmed) {
        // Retry through the same epoch and lifetime guard as ordinary polls.
        schedule(500, true);
        if (unconfirmed_reported_) return outcome;
        unconfirmed_reported_ = true;
    }
    if (changed) changed(outcome);
    return outcome;
}

void RenderEditTicket::abandon() {
    ++epoch_;
    request_ = 0;
    ticket_.clear();
    busy_ = scheduled_ = false;
    unconfirmed_reported_ = false;
}

bool RenderEditTicket::submit(const std::string &method, const Json &params) {
    if (busy_) return false;
    busy_ = true;
    std::string error;
    request_ = transport_.request ? transport_.request(method, params, &error) : 0;
    if (request_) return true;
    abandon();
    deliver({Kind::Failed, {}, error.empty() ? "The edit could not be sent." : error});
    return false;
}

void RenderEditTicket::schedule(int milliseconds, bool retry) {
    if (scheduled_ || request_ || ticket_.empty()) return;
    scheduled_ = true;
    const auto epoch = epoch_;
    std::weak_ptr<int> lifetime = lifetime_;
    transport_.schedule(milliseconds, [this, lifetime, epoch, retry] {
        if (lifetime.expired() || epoch != epoch_) return;
        scheduled_ = false;
        if (retry) resume();
        else poll();
    });
}

void RenderEditTicket::resume() {
    // Invalidate a previous timer as well: an explicit retry must not leave
    // two outstanding questions about the same ticket.
    if (request_ || ticket_.empty()) return;
    ++epoch_;
    scheduled_ = false;
    poll();
}

void RenderEditTicket::poll() {
    if (request_ || ticket_.empty()) return;
    std::string error;
    request_ = transport_.request("get_edit_result", {{"ticket", ticket_}}, &error);
    if (!request_)
        deliver({Kind::Unconfirmed, {}, error.empty() ? "The edit result could not be read." : error});
}

RenderEditTicket::Outcome RenderEditTicket::fail(std::int64_t id, const std::string &error) {
    if (!id || id != request_) return {};
    request_ = 0;
    if (!ticket_.empty()) return deliver({Kind::Unconfirmed, {}, error});
    abandon();
    return deliver({Kind::Failed, {}, error});
}

RenderEditTicket::Outcome RenderEditTicket::receive(std::int64_t id, const Json &result) {
    if (!id || id != request_) return {};
    request_ = 0;
    const bool polling = !ticket_.empty();
    try {
        if (!result.is_object()) throw std::invalid_argument("The engine answered something that is not an object.");
        const auto status = result.value("status", std::string{});
        if (status == "committed") {
            const auto completion = readRenderEditCompletion(result);
            // Clear before notifying: callers may submit a subsequent edit.
            abandon();
            if (!completion.source_digest.empty() && publish_digest_)
                publish_digest_(completion.source_digest);
            return deliver({Kind::Committed, result, {}});
        }
        if (status == "rejected") {
            abandon();
            return deliver({Kind::Refused, result, {}});
        }
        if (!polling && status == "accepted") {
            const auto ticket = result.find("ticket");
            ticket_ = ticket != result.end() && ticket->is_string()
                ? ticket->get<std::string>() : std::string{};
            if (ticket_.empty()) {
                abandon();
                return deliver({Kind::Refused, result, "The engine returned no edit ticket.", Cause::MissingTicket});
            }
            schedule(0);
            return deliver({Kind::Accepted, result, {}});
        }
        if (polling && (status == "pending" || status == "running")) {
            schedule(status == "running" ? 500 : 25);
            return deliver({Kind::Pending, result, {}});
        }
        std::string error = "The engine returned an unknown edit status: " + status;
        if (result.contains("error")) error = result.at("error").is_string()
            ? result.at("error").get<std::string>() : result.at("error").dump();
        if (polling) return deliver({Kind::Unconfirmed, result, error});
        abandon();
        return deliver({Kind::Failed, result, error});
    } catch (const std::exception &error) {
        if (polling) return deliver({Kind::Unconfirmed, result, error.what()});
        abandon();
        return deliver({Kind::Failed, result, error.what()});
    }
}
} // namespace PelicanStudio
