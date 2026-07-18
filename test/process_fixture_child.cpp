#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::string childMode() {
#ifdef _WIN32
    char *buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, "PELICAN_WP173_CHILD_MODE") != 0 || buffer == nullptr) return {};
    const std::string result{buffer};
    std::free(buffer);
    return result;
#else
    const char *value = std::getenv("PELICAN_WP173_CHILD_MODE");
    return value == nullptr ? std::string{} : std::string{value};
#endif
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc >= 2 && std::string_view{argv[1]} == "psd-extract") {
        if (childMode() == "hang") {
            std::cout << "importer-stdout-marker\n" << std::flush;
            std::cerr << "importer-stderr-marker\n" << std::flush;
            for (;;) std::this_thread::sleep_for(std::chrono::seconds{1});
        }
        return 3;
    }
    if (argc >= 2 && std::string_view{argv[1]} == "emit") {
        const std::string_view value = argc >= 3 ? argv[2] : "none";
        std::cout << "stdout-capture:" << value << '\n';
        std::cerr << "stderr-capture:" << value << '\n';
        return 0;
    }
    if (argc >= 2 && std::string_view{argv[1]} == "hang") {
        std::cout << "hang-ready\n" << std::flush;
        for (;;) std::this_thread::sleep_for(std::chrono::seconds{1});
    }
    return 2;
}
