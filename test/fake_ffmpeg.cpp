#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
std::string environment(const char *name) {
    const char *value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

std::uint64_t hashFrame(const std::vector<char> &frame) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : frame) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}
}

int main(int argc, char **argv) {
    if (argc == 2 && std::string{argv[1]} == "-version") {
        std::cout << "ffmpeg version fake-1.0\n";
        return 0;
    }
#ifdef _WIN32
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ofstream arguments{environment("PELICAN_FAKE_FFMPEG_SIDE")};
    for (int index = 0; index < argc; ++index)
        arguments << "arg " << argv[index] << '\n';
    arguments.flush();
    if (argc > 1) {
        std::ofstream output{argv[argc - 1], std::ios::binary};
        output << "fake mp4 output\n";
    }
    const auto early = environment("PELICAN_FAKE_FFMPEG_EARLY_EXIT");
    if (!early.empty()) {
        std::cerr << "fake encoder early diagnostic tail\n";
        return std::atoi(early.c_str());
    }
    if (!environment("PELICAN_FAKE_FFMPEG_HANG").empty() ||
        !environment("PELICAN_FAKE_FFMPEG_NO_READ").empty()) {
        while (true) std::this_thread::sleep_for(std::chrono::hours(1));
    }
    std::size_t width = 0;
    std::size_t height = 0;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string{argv[index]} != "-video_size") continue;
        const std::string size = argv[index + 1];
        const auto separator = size.find('x');
        width = std::stoull(size.substr(0, separator));
        height = std::stoull(size.substr(separator + 1));
    }
    const std::size_t frame_size = width * height * 4;
    std::uint64_t bytes = 0;
    std::vector<std::uint64_t> hashes;
    std::vector<char> buffer(frame_size == 0 ? 4096 : frame_size);
    while (true) {
        std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = std::cin.gcount();
        if (count == 0) break;
        bytes += static_cast<std::uint64_t>(count);
        if (frame_size != 0) {
            std::size_t offset = 0;
            while (offset + frame_size <= static_cast<std::size_t>(count)) {
                std::vector<char> frame(buffer.begin() + static_cast<std::ptrdiff_t>(offset),
                    buffer.begin() + static_cast<std::ptrdiff_t>(offset + frame_size));
                hashes.push_back(hashFrame(frame));
                offset += frame_size;
            }
        }
        if (!environment("PELICAN_FAKE_FFMPEG_PARTIAL_PRUNE").empty() && argc > 1) {
            std::ofstream output{argv[argc - 1], std::ios::binary | std::ios::app};
            output.write(buffer.data(), count);
        }
        if (!environment("PELICAN_FAKE_FFMPEG_HEAVY_STDERR").empty())
            for (int line = 0; line < 512; ++line)
                std::cerr << "fake stderr padding line " << line << " "
                          << std::string(256, 'x') << '\n';
    }
    arguments << "bytes " << bytes << '\n';
    arguments << "frames " << hashes.size() << '\n';
    for (const auto hash : hashes) arguments << "hash " << hash << '\n';
    arguments.flush();
    const auto failure = environment("PELICAN_FAKE_FFMPEG_EXIT");
    return failure.empty() ? 0 : std::atoi(failure.c_str());
}
