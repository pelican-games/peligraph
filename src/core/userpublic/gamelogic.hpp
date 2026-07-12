#pragma once

#include <cstdint>

namespace Pelican {

// Increment this only when the C++ game-DLL boundary changes incompatibly.
inline constexpr std::uint32_t gameLogicAbiVersion = 1;
inline constexpr const char *gameLogicAbiSymbol = "pelican_game_logic_abi_version";

using GameLogicAbiVersionFn = std::uint32_t (*)();

} // namespace Pelican
