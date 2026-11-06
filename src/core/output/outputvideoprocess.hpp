#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

struct OutputVideoEncoderOptions {
    std::filesystem::path executable;
    std::filesystem::path partial_path;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t fps_num = 0;
    std::uint64_t fps_den = 0;
    // Input is display-encoded SDR; linear data is not converted before BT.709 encoding.
    bool bgra = false;
};

std::vector<std::string> outputVideoEncoderArguments(
    const OutputVideoEncoderOptions &options);
std::string quoteOutputVideoWindowsCommandLine(
    const std::vector<std::string> &arguments);
std::uint64_t outputVideoPosition(std::int64_t elapsed_nanoseconds,
                                  std::uint64_t fps_num,
                                  std::uint64_t fps_den);
std::optional<std::filesystem::path> findOutputVideoEncoder(
    const std::optional<std::filesystem::path> &configured);
std::optional<std::string> outputVideoEncoderVersion(
    const std::filesystem::path &executable, std::chrono::milliseconds timeout);

class OutputVideoEncoderProcess {
  public:
    OutputVideoEncoderProcess();
    ~OutputVideoEncoderProcess();
    OutputVideoEncoderProcess(const OutputVideoEncoderProcess &) = delete;
    OutputVideoEncoderProcess &operator=(const OutputVideoEncoderProcess &) = delete;

    bool start(const OutputVideoEncoderOptions &options, std::string &error);
    bool write(const std::uint8_t *data, std::size_t size, std::string &error);
    void closeInput();
    std::optional<int> wait(std::chrono::milliseconds timeout);
    void terminate();
    bool running() const;
    std::uint64_t pid() const;
    std::string stderrTail() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Pelican
