#pragma once

#include "../container.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace Pelican {

struct CameraBakeSample {
    double time = 0.0;
    float pos[3]{};
    float rotation_xyzw[4]{0.0f, 0.0f, 0.0f, 1.0f};
};

DECLARE_MODULE(CameraBakeRecorder) {
    bool active = false;
    double fps = 60.0;
    std::filesystem::path output_path;
    std::string camera_name = "camera";
    std::vector<CameraBakeSample> samples;

  public:
    ~CameraBakeRecorder();

    void start(const std::filesystem::path &path, double sample_fps);
    void recordFrame();
    void finish();
    bool isActive() const noexcept { return active; }
    std::size_t sampleCount() const noexcept { return samples.size(); }
};

} // namespace Pelican
