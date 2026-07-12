#include <gamelogic.hpp>

#include <cstdint>

#ifdef _WIN32
#define PELICAN_GAME_EXPORT extern "C" __declspec(dllexport)
#else
#define PELICAN_GAME_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#ifndef PELICAN_GAME_LOGIC_ABI_VERSION
#define PELICAN_GAME_LOGIC_ABI_VERSION ::Pelican::gameLogicAbiVersion
#endif

PELICAN_GAME_EXPORT std::uint32_t pelican_game_logic_abi_version() {
    return PELICAN_GAME_LOGIC_ABI_VERSION;
}
