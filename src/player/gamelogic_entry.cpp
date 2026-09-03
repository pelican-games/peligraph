#include <gamelogic.hpp>

#include <cstdint>

#ifdef _WIN32
#define PELICAN_GAME_EXPORT extern "C" __declspec(dllexport)
#else
#define PELICAN_GAME_EXPORT extern "C" __attribute__((visibility("default")))
#endif

extern "C" PELICAN_API std::uint64_t
PELICAN_GAME_LOGIC_EPOCH_ANCHOR(PELICAN_GAME_LOGIC_ABI_VERSION)() noexcept;

// Keeping the call in an exported, non-inline function forces a normal,
// direct, named import even for a game DLL with no registrations. The OS
// loader resolves this import before it executes any DLL initializer.
PELICAN_GAME_EXPORT std::uint64_t pelican_game_logic_epoch_nonce() noexcept {
    static_assert(Pelican::gameLogicAbiVersion == PELICAN_GAME_LOGIC_ABI_VERSION);
    return PELICAN_GAME_LOGIC_EPOCH_ANCHOR(PELICAN_GAME_LOGIC_ABI_VERSION)();
}
