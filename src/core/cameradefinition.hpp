#pragma once

#include <optional>

namespace Pelican {

enum class CameraProjectionKind {
    Perspective,
    Orthographic,
};

enum class CameraPixelPerfectMode {
    off,
    strict,
};

enum class CameraSpriteSortPolicy {
    z,
    y_down,
    declaration,
};

// Sprite policy belongs to a camera because sorting and pixel quantization are
// view-dependent. pixels-per-unit remains a project setting (ProjectBasicConfig).
struct CameraSpritePolicySpec {
    CameraPixelPerfectMode pixel_perfect = CameraPixelPerfectMode::off;
    CameraSpriteSortPolicy sort = CameraSpriteSortPolicy::z;
};

struct CameraProjectionSpec {
    CameraProjectionKind kind = CameraProjectionKind::Perspective;
    float yfov = 0.78539816339f;
    float znear = 0.1f;
    float zfar = 1000.0f;
    std::optional<float> aspect;
    float xmag = 1.0f;
    float ymag = 1.0f;
};

} // namespace Pelican
