#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace PelicanStudio {

// Tracks one edit, without owning its draft, retry policy or completion UI.
// Abandoning tracking does not cancel an edit in the engine.
class RenderEditTicket {
public:
    using Json = nlohmann::json;
    struct Transport {
        std::function<std::int64_t(const std::string &, const Json &, std::string *)> request;
        std::function<void(int, std::function<void()>)> schedule;
    };
    enum class Kind { NotMine, Accepted, Pending, Committed, Refused, Failed, Unconfirmed };
    enum class Cause { None, MissingTicket };
    struct Outcome {
        Kind kind = Kind::NotMine;
        Json result;
        std::string error;
        Cause cause = Cause::None;
    };
    explicit RenderEditTicket(Transport transport,
                              std::function<void(const std::string &)> publishDigest = {});
    ~RenderEditTicket();
    RenderEditTicket(const RenderEditTicket &) = delete;
    RenderEditTicket &operator=(const RenderEditTicket &) = delete;
    bool submit(const std::string &method, const Json &params);
    Outcome receive(std::int64_t id, const Json &result);
    Outcome fail(std::int64_t id, const std::string &error);
    void resume();
    void abandon();
    bool busy() const noexcept { return busy_; }
    bool awaitingResponse() const noexcept { return request_ != 0; }
    const std::string &ticket() const noexcept { return ticket_; }
    // Owned outcomes, including scheduled transport failures. Repeated
    // Unconfirmed replies are returned, but only the first notifies the UI.
    std::function<void(const Outcome &)> changed;
private:
    Transport transport_;
    std::function<void(const std::string &)> publish_digest_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
    std::uint64_t epoch_ = 0;
    std::int64_t request_ = 0;
    std::string ticket_;
    bool busy_ = false;
    bool scheduled_ = false;
    bool unconfirmed_reported_ = false;
    Outcome deliver(Outcome outcome);
    void schedule(int milliseconds, bool retry = false);
    void poll();
};
} // namespace PelicanStudio
