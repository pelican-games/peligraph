#include "userinput.hpp"
#include "../os/window.hpp"

namespace Pelican {

bool UserInput::getKey(KeyCode code) {
    GET_MODULE(Window); // TODO
    return false;
}

bool UserInput::isKeyPushed(KeyCode code) {
    GET_MODULE(Window); // TODO
    return false;
}

bool UserInput::isKeyReleased(KeyCode code) {
    GET_MODULE(Window); // TODO
    return false;
}

} // namespace Pelican
