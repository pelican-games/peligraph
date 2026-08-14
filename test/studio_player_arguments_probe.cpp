#include "../src/devstudio/viewport/studioplayerarguments.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: studio_player_arguments_probe <project> [configured arguments...]\n";
        return 64;
    }

    std::vector<std::string> configured;
    configured.reserve(static_cast<std::size_t>(argc - 2));
    for (int index = 2; index < argc; ++index) {
        configured.emplace_back(argv[index]);
    }

    for (const auto &argument :
         PelicanStudio::studioPlayerArgumentStrings(std::move(configured),
                                                    argv[1])) {
        std::cout << argument << '\n';
    }
}
