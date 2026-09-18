#pragma once

namespace Pelican {

// `--camera` applied to the live Camera, once per process.
//
// This used to be `SeqPlayer::applyCameraOverride()` and
// `VatPlayer::applyCameraOverride()` -- 19 byte-identical lines in each -- and
// it is not playback: it reads `EngineLaunchConfig::camera_override` and pokes
// `Camera`, and neither player looks at the result. With both `--play-seq` and
// `--play-vat` given it also ran twice. The startup path calls this one
// function instead, at the earliest point either constructor used to reach it.
//
// Throws std::runtime_error when the override's position and target coincide,
// exactly as the two members did.
void applyLaunchCameraOverride();

} // namespace Pelican
