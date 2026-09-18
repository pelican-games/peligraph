#pragma once

// Umbrella include kept for the ~28 translation units that were written
// against the single compute-task header. The declarations now live next to
// the class they belong to:
//   framegraphbuffercontainer.hpp  FrameGraphResourceContainer (buffer side)
//   computetaskdefinitionjson.hpp  frame graph / compute task JSON parsing
//   computetaskcontainer.hpp       ComputeTaskContainer and its dependencies
#include "computetaskcontainer.hpp"
#include "computetaskdefinitionjson.hpp"
#include "framegraphbuffercontainer.hpp"

#include "framegraphbufferdefinition.hpp"
#include "frameplanner.hpp"
#include "renderingpass.hpp"
#include "../../project/targetrenderplanning.hpp"
#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/buf.hpp"
#include <array>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>
