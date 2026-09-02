#include <gamelogic.hpp>

#include <cstdint>

#ifdef _WIN32
#define PELICAN_GAME_EXPORT extern "C" __declspec(dllexport)
#else
#define PELICAN_GAME_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#ifndef PELICAN_GAME_LOGIC_ABI_VERSION
#define PELICAN_GAME_LOGIC_ABI_VERSION 2
#endif
#define PELICAN_EPOCH_ANCHOR_NAME_INNER(epoch) pelican_game_logic_epoch_v##epoch
#define PELICAN_EPOCH_ANCHOR_NAME(epoch) PELICAN_EPOCH_ANCHOR_NAME_INNER(epoch)

extern "C" PELICAN_API std::uint64_t
PELICAN_EPOCH_ANCHOR_NAME(PELICAN_GAME_LOGIC_ABI_VERSION)() noexcept;

// Keeping the call in an exported, non-inline function forces a normal,
// direct, named import even for a game DLL with no registrations. The OS
// loader resolves this import before it executes any DLL initializer.
PELICAN_GAME_EXPORT std::uint64_t pelican_game_logic_epoch_nonce() noexcept {
    return PELICAN_EPOCH_ANCHOR_NAME(PELICAN_GAME_LOGIC_ABI_VERSION)();
}
