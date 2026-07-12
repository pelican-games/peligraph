#pragma once

#include "atlas.hpp"
#include "bitmapfont.hpp"
#include "commandbuffer.hpp"
#include "document.hpp"
#include "drawcommands.hpp"
#include "inputrouter.hpp"
#include "layout.hpp"
#include "../container.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <nlohmann/json_fwd.hpp>
#include <vulkan/vulkan.hpp>

namespace Pelican { class InputState; }

namespace Pelican::ui {

struct UiTexturePage {
    std::uint16_t id = 0;
    std::string stable_name;
    std::filesystem::path image_path;
    std::string embedded_resource;
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
    BitmapFont bitmap_font;
    std::uint16_t font_page_id = 0;
    WidgetArena widget_arena;
    std::vector<WidgetId> traversal;
    std::map<std::string, WidgetId, std::less<>> widget_ids;
    InputRouter input_router;
    UiCommandBuffer command_buffer;
    std::optional<WidgetId> hovered_widget;
    CommitStatus last_commit;
    RouteFrameResult last_route;
    std::size_t parser_invocations = 0;

    void loadVisuals(const DocumentNode &node);
    void createRuntimeWidgets(const DocumentNode &node);
    void syncLayout(vk::Extent2D extent, double ui_scale);
    const DocumentNode *findNode(std::string_view path) const;
    void handlePointer(const RoutedPointerEvent &event);
    void emitBindings(const DocumentNode &node, std::string_view trigger, const RoutedPointerEvent &event);

  public:
    UiModule();
    explicit UiModule(const nlohmann::json &document_json);

    DrawBatch buildFrame(vk::Extent2D extent, double ui_scale = 1.0) const;
    RouteFrameResult routeFrameInput(Pelican::InputState &input, vk::Extent2D extent, double ui_scale = 1.0);
    const std::vector<UiTexturePage> &pages() const noexcept { return texture_pages; }
    const UiDocument &uiDocument() const noexcept { return document; }
    std::size_t parserInvocationsForTesting() const noexcept { return parser_invocations; }
    const WidgetArena &arenaForTesting() const noexcept { return widget_arena; }
    const std::vector<WidgetId> &traversalForTesting() const noexcept { return traversal; }
    const CommitStatus &lastCommitForTesting() const noexcept { return last_commit; }
};

} // namespace Pelican::ui
