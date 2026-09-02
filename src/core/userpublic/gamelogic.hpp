#pragma once

#include "export.hpp"

#include <cstdint>

namespace Pelican {

// The SDK epoch is monotonic. Every game DLL produced by
// pelican_configure_game_logic_target imports the matching anchor from its
// executable host before any DLL initializer can run.
inline constexpr std::uint32_t gameLogicAbiVersion = 2;
inline constexpr const char *gameLogicEpochAnchorSymbol =
    "pelican_game_logic_epoch_v2";

extern "C" PELICAN_API std::uint64_t
pelican_game_logic_epoch_v2() noexcept;

} // namespace Pelican
