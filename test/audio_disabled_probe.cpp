#include "../src/core/userpublic/gamecontext.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        Pelican::GameContext{}.playSound("project://missing.wav");
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
    return 0;
}
