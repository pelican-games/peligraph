#pragma once

#include <optional>

namespace Pelican {

enum class CameraProjectionKind {
    Perspective,
    Orthographic,
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
