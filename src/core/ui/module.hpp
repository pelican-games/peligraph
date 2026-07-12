#pragma once

#include "atlas.hpp"
#include "document.hpp"
#include "drawcommands.hpp"
#include "layout.hpp"
#include "../container.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <nlohmann/json_fwd.hpp>
#include <vulkan/vulkan.hpp>

namespace Pelican::ui {

struct UiTexturePage {
    std::uint16_t id = 0;
    std::string stable_name;
    std::filesystem::path image_path;
    PointI size{};
};

DECLARE_MODULE(UiModule) {
  public:
    struct ResolvedVisual {
        std::uint16_t page_id = 0;
        std::string texture_name = "white";
        RectI source_rect{};
        PointI page_size{1, 1};
    };

  private:
    UiDocument document;
    std::vector<UiTexturePage> texture_pages;
    std::map<std::string, ResolvedVisual, std::less<>> visuals;
    std::size_t parser_invocations = 0;

    void loadVisuals(const DocumentNode &node);

  public:
    UiModule();
    explicit UiModule(const nlohmann::json &document_json);

    DrawBatch buildFrame(vk::Extent2D extent, double ui_scale = 1.0) const;
    const std::vector<UiTexturePage> &pages() const noexcept { return texture_pages; }
    const UiDocument &uiDocument() const noexcept { return document; }
    std::size_t parserInvocationsForTesting() const noexcept { return parser_invocations; }
};

} // namespace Pelican::ui
