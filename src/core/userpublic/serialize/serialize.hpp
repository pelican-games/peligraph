#pragma once

#include <cstdint>
#include <geom/quat.hpp>
#include <geom/vec.hpp>
#include <string>

namespace Pelican {

template <class T, class TArchive>
concept ISerializable = requires(T v, TArchive ar) { v.ref(ar); };

} // namespace Pelican
