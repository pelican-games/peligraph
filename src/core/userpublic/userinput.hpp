#pragma once

namespace Pelican {

enum class KeyCode {

};

class UserInput {
    static bool getKey(KeyCode code);
    static bool isKeyPushed(KeyCode code);
    static bool isKeyReleased(KeyCode code);
};

} // namespace Pelican
