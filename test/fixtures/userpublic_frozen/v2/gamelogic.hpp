#pragma once

#include "export.hpp"

#include <cstdint>

#ifndef PELICAN_GAME_LOGIC_ABI_VERSION
#define PELICAN_GAME_LOGIC_ABI_VERSION 2
#endif
#define PELICAN_GAME_LOGIC_EPOCH_ANCHOR_INNER(epoch) pelican_game_logic_epoch_v##epoch
#define PELICAN_GAME_LOGIC_EPOCH_ANCHOR(epoch) \
    PELICAN_GAME_LOGIC_EPOCH_ANCHOR_INNER(epoch)
#define PELICAN_GAME_LOGIC_STRINGIFY_INNER(value) #value
#define PELICAN_GAME_LOGIC_STRINGIFY(value) PELICAN_GAME_LOGIC_STRINGIFY_INNER(value)

namespace Pelican {

// The SDK epoch is monotonic. Every game DLL produced by
// pelican_configure_game_logic_target imports the matching anchor from its
// executable host before any DLL initializer can run.
inline constexpr std::uint32_t gameLogicAbiVersion =
    PELICAN_GAME_LOGIC_ABI_VERSION;
inline constexpr const char *gameLogicEpochAnchorSymbol =
    "pelican_game_logic_epoch_v" PELICAN_GAME_LOGIC_STRINGIFY(
        PELICAN_GAME_LOGIC_ABI_VERSION);
static_assert(gameLogicAbiVersion == PELICAN_GAME_LOGIC_ABI_VERSION);

extern "C" PELICAN_API std::uint64_t
PELICAN_GAME_LOGIC_EPOCH_ANCHOR(PELICAN_GAME_LOGIC_ABI_VERSION)() noexcept;

} // namespace Pelican
