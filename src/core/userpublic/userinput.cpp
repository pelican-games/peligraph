#include "userinput.hpp"
#include "../os/inputstate.hpp"

namespace Pelican {

bool UserInput::getKey(KeyCode code) {
    return GET_MODULE(InputState).currentSnapshot().getKey(code);
}

bool UserInput::isKeyPushed(KeyCode code) {
    return GET_MODULE(InputState).currentSnapshot().isKeyPushed(code);
}

bool UserInput::isKeyReleased(KeyCode code) {
    return GET_MODULE(InputState).currentSnapshot().isKeyReleased(code);
}

} // namespace Pelican
