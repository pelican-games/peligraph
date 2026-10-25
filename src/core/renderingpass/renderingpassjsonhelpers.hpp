#pragma once

#include "renderingpass.hpp"
#include "../../project/passfieldownership.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

// RC-3a1: the names of image formats and usages, what a render target may
// declare and what each format is, are the project's (imageformat.hpp,
// imageusage.hpp); core maps them to Vulkan (vulkanimagevocabulary.hpp).
// RC-3a2: so are the attachment operations' spellings
// (attachmentoperations.hpp) and the resolution domain's
// (renderresolutiondomain.hpp).
std::string parseStringField(const nlohmann::json &json, std::string_view field_name,
                             std::string_view context);
// A field that accepts a single string or an array of strings. The context
// is the caller's own wording, so the thrown message stays the caller's.
std::vector<std::string> parseStringList(const nlohmann::json &json,
                                         std::string_view context);
std::vector<std::string> parseOptionalStringList(const nlohmann::json &json,
                                                 std::string_view field,
                                                 std::string_view context);
// Name lists keep their authored order and admit each name once.
void appendUnique(std::vector<std::string> &values, std::string value);
void appendUnique(std::vector<std::string> &values,
                  const std::vector<std::string> &more_values);
void validateName(const std::string &name, const std::string &context);
float parseFloatField(const nlohmann::json &json, const std::string &field_name, const std::string &context);
std::vector<std::string> parseStringArrayField(const nlohmann::json &json, const std::string &field_name,
                                               const std::string &context);
std::vector<std::string> parseOptionalRegionTags(
    const nlohmann::json &json, const std::string &context);
// Authoring edits validate view selection before publishing a draft.
RenderViewSelect parseRenderViewSelect(const nlohmann::json &json, const std::string &context);
uint32_t parseUint32Field(const nlohmann::json &json, const std::string &field_name,
                          const std::string &context);

} // namespace Pelican
