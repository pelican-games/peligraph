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
#include <functional>
#include <map>
#include <string_view>
#include <optional>
#include <nlohmann/json_fwd.hpp>
#include <vulkan/vulkan.hpp>

namespace Pelican { class InputState; }

namespace Pelican::ui {

// GP-3: the properties game code and the graph's ui.* sinks write. A node is
// addressed by (document key, path); the path is the node ids from the root,
// root included, joined by '/' (DocumentNode::path, the same string an emit's
// stable_id carries). One writer per (document, path, property).
enum class UiProperty { text, visible, value };
std::string_view uiPropertyName(UiProperty property) noexcept;
// text: label and button. value: gauge. visible: every node.
bool uiNodeHasProperty(std::string_view type, UiProperty property) noexcept;

enum class UiWriteStatus { ok, node_unknown, property_mismatch, sink_conflict };

struct UiWrite {
    UiProperty property = UiProperty::text;
    std::string text;
    bool visible = true;
    double value = 0.0;
};

// Answers whether a graph sink owns (path, property) of the project's UI
// document. The sink is the value's writer; a facade write there is refused.
using UiSinkClaims = std::function<bool(std::string_view path, UiProperty property)>;

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
    // PM-1a: while the game is paused the UI routes input but emits no event.
    bool events_suppressed = false;
    std::size_t parser_invocations = 0;

    void loadVisuals(const DocumentNode &node);
    void createRuntimeWidgets(const DocumentNode &node);
    void syncLayout(vk::Extent2D extent, double ui_scale);
    const DocumentNode *findNode(std::string_view path) const;
    void handlePointer(const RoutedPointerEvent &event);
    void emitBindings(const DocumentNode &node, std::string_view trigger, const RoutedPointerEvent &event);
    DocumentNode *findMutableNode(std::string_view path);
    // After a commit: carry the runtime text and value back into the document
    // the layout and the draw read.
    void syncDocumentFromArena();
    CommitStatus commitPending();

  public:
    UiModule();
    explicit UiModule(const nlohmann::json &document_json);

    // GP-3. Whether (document_key, path) names a node that has `property`. An
    // empty document_key is the project's UI document (v0 has one); any other
    // key must equal that document's `key`.
    UiWriteStatus checkTarget(std::string_view document_key, std::string_view path,
                              UiProperty property) const;
    // Queues a write for the next commit boundary. Refused, and nothing
    // queued, when the node is unknown, lacks the property (or the value is
    // not finite), or a sink owns the unit.
    UiWriteStatus queueWrite(std::string_view document_key, std::string_view path, UiWrite write,
                             const UiSinkClaims &claimed_by_sink = {});
    // The frame boundary after game code ran and before the frame is drawn.
    CommitStatus commitFrameWrites();
    // A ui.* sink's value for this frame. The graph is the unit's only
    // writer, so it lands at once rather than through the command buffer.
    // False when the path names no node that has the property.
    bool applySinkValue(std::string_view path, UiProperty property, double value);

    DrawBatch buildFrame(vk::Extent2D extent, double ui_scale = 1.0) const;
    RouteFrameResult routeFrameInput(Pelican::InputState &input, vk::Extent2D extent, double ui_scale = 1.0);
    void setEventsSuppressed(bool suppressed) noexcept { events_suppressed = suppressed; }
    bool eventsSuppressed() const noexcept { return events_suppressed; }
    const std::vector<UiTexturePage> &pages() const noexcept { return texture_pages; }
    const UiDocument &uiDocument() const noexcept { return document; }
    std::size_t parserInvocationsForTesting() const noexcept { return parser_invocations; }
    const WidgetArena &arenaForTesting() const noexcept { return widget_arena; }
    const std::vector<WidgetId> &traversalForTesting() const noexcept { return traversal; }
    const CommitStatus &lastCommitForTesting() const noexcept { return last_commit; }
};

} // namespace Pelican::ui
