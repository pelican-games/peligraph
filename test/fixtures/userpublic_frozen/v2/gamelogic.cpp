#include "gamelogic.hpp"

#include <cstdint>

namespace Pelican {

extern "C" std::uint64_t
PELICAN_GAME_LOGIC_EPOCH_ANCHOR(PELICAN_GAME_LOGIC_ABI_VERSION)() noexcept {
    // ASLR gives each process a different value while the address remains
    // stable for the lifetime of the executable export host. The epoch is
    // mixed in so an accidental constant-fold to zero cannot masquerade as a
    // successful anchor call in the constructor control fixture.
    static const std::uint8_t process_anchor = 0;
    return static_cast<std::uint64_t>(
               reinterpret_cast<std::uintptr_t>(&process_anchor)) ^
           (static_cast<std::uint64_t>(gameLogicAbiVersion) << 56U) ^
           UINT64_C(0x50454c4943414e00);
}

} // namespace Pelican
