#pragma once

#include "osclistenerconfig.hpp"

#include "../container.hpp"

#include <cstdint>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace Pelican {

// OSC-2b: one UDP socket feeding OscInput, owned by the engine thread.
//
// Configuration is process-wide (player options) and lives on
// EngineLaunchConfig; the module itself is always constructible so builds and
// runs without --osc-port can still report state "off".  start() binds a
// single socket and launches one worker; a bind or thread-creation failure is
// non-fatal and leaves state "failed" with an osc.bind_failed@1 error.
//
// allow is an exact-match sender IPv4 filter, not authentication and not a
// security boundary.
//
// The listener promises bounded memory, not flood resistance.  A packet is
// judged when OscInput stages it; datagrams already queued by the OS while a
// replay is running may therefore be applied after the replay ends.  That is
// accepted rather than fixed.
//
// fork() without exec after start() is unsupported: the child inherits the
// socket descriptor and worker thread bookkeeping but not a sane polling
// thread.
class OscListener final : public ModuleBase<OscListener> {
public:
    OscListener();
    ~OscListener();

    OscListener(const OscListener &) = delete;
    OscListener &operator=(const OscListener &) = delete;

    // Idempotent for a listening listener.  Returns false only when the bind or
    // worker thread failed; statusJson() then carries the named error.  Config
    // errors throw osc.config_invalid@1 because they are fatal at startup.
    bool start(const OscListenerConfig &config);

    // Idempotent, safe before start and after a failed start.  Sets the stop
    // flag, joins (never while holding the status mutex), closes the socket,
    // then releases Winsock for this listener.
    void stop();

    [[nodiscard]] nlohmann::json statusJson() const;

    // Small const accessors used by the tests instead of guessing at thread or
    // port state from the outside.
    [[nodiscard]] bool socketOpen() const;
    [[nodiscard]] bool workerStarted() const;
    [[nodiscard]] bool localEndpoint(std::string &address,
                                     std::uint16_t &port) const;
    // POSIX only; returns false on Windows and when no socket is open.
    [[nodiscard]] bool reuseAddressOptionForTesting() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Pelican
