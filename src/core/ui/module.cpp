#include "module.hpp"

#include "../loader/basicconfig.hpp"
#include "../loader/engineresources.hpp"
#include "../loader/pathresolver.hpp"
#include "../os/inputstate.hpp"
#include "../userpublic/details/event/registerer.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <limits>
#include <set>
#include <stdexcept>

namespace Pelican::ui {
namespace {

void requireDocument(DocumentParseResult parsed, UiDocument &out) {
    if (!parsed) {
        const auto &error = parsed.errors.front();
        throw std::runtime_error("pelican.ui v1 " + std::string{toString(error.code)} + " at " +
                                 error.path + ": " + error.message);
    }
    out = std::move(*parsed.document);
}

std::array<float, 4> uvRect(RectI rect, PointI page) {
    return {static_cast<float>(rect.left) / page.x, static_cast<float>(rect.top) / page.y,
            static_cast<float>(rect.right) / page.x, static_cast<float>(rect.bottom) / page.y};
}

void appendQuad(std::vector<QuadCommand> &out, const DocumentNode &node, const LayoutRecord &layout,
                RectI destination, RectI source, const UiModule::ResolvedVisual &visual,
                const ViewportTransform &viewport, std::array<std::uint8_t, 4> color, Sampler sampler) {
    const auto destination_px = viewport.uiToPx(destination);
    const auto scissor_px = intersect(viewport.uiToPx(layout.clip_ui), viewport.content_rect_px);
    if (destination_px.left >= destination_px.right || destination_px.top >= destination_px.bottom ||
        scissor_px.left >= scissor_px.right || scissor_px.top >= scissor_px.bottom) return;
    QuadCommand command;
    command.key = {"rgba_straight", sampler,
                   visual.texture_name, scissor_px, visual.page_id, 0};
    command.rect_px = destination_px;
    command.layer = node.layer;
    command.decl_seq = node.decl_seq;
    command.uv_rect = uvRect(source, visual.page_size);
    command.color = color;
    command.widget_id = node.path;
    out.push_back(std::move(command));
}

void emitVisual(const DocumentNode &node, const LayoutRecord &layout,
                const UiModule::ResolvedVisual &visual, const ViewportTransform &viewport,
                std::vector<QuadCommand> &commands, std::array<std::uint8_t, 4> color) {
    if (!node.nine_patch) {
        appendQuad(commands, node, layout, layout.rect_ui, visual.source_rect, visual, viewport, color, node.sampler);
        return;
    }
    const auto margins = *node.nine_patch;
    const auto source = visual.source_rect;
    if (margins.left < 0 || margins.top < 0 || margins.right < 0 || margins.bottom < 0 ||
        margins.left + margins.right > source.width() || margins.top + margins.bottom > source.height())
        throw std::runtime_error("pelican.ui v1 invalid nine_patch margins: " + node.path);
    const auto width = static_cast<std::int32_t>(layout.rect_ui.width());
    const auto height = static_cast<std::int32_t>(layout.rect_ui.height());
    const auto left = std::min(margins.left, width);
    const auto right = std::min(margins.right, std::max(0, width - left));
    const auto top = std::min(margins.top, height);
    const auto bottom = std::min(margins.bottom, std::max(0, height - top));
    const std::int32_t dx[]{layout.rect_ui.left, layout.rect_ui.left + left,
                            layout.rect_ui.right - right, layout.rect_ui.right};
    const std::int32_t dy[]{layout.rect_ui.top, layout.rect_ui.top + top,
                            layout.rect_ui.bottom - bottom, layout.rect_ui.bottom};
    const std::int32_t sx[]{source.left, source.left + margins.left,
                            source.right - margins.right, source.right};
    const std::int32_t sy[]{source.top, source.top + margins.top,
                            source.bottom - margins.bottom, source.bottom};
    for (int y = 0; y < 3; ++y) for (int x = 0; x < 3; ++x) {
        appendQuad(commands, node, layout, {dx[x], dy[y], dx[x + 1], dy[y + 1]},
                   {sx[x], sy[y], sx[x + 1], sy[y + 1]}, visual, viewport, color, node.sampler);
    }
}

void visitNodes(const DocumentNode &node, const std::map<std::string, const LayoutRecord *, std::less<>> &layouts,
                const std::map<std::string, UiModule::ResolvedVisual, std::less<>> &visuals,
                const WidgetArena &arena, const std::map<std::string, WidgetId, std::less<>> &widget_ids,
                const BitmapFont &font, std::uint16_t font_page_id,
                const ViewportTransform &viewport, std::vector<QuadCommand> &commands) {
    const auto layout = layouts.find(node.path);
    const auto widget_id = widget_ids.find(node.path);
    const auto *state = widget_id == widget_ids.end() ? nullptr : arena.resolve(widget_id->second);
    const bool visible = state ? state->visible : node.visibility == Visibility::Visible;
    if (visible && node.color[3] != 0 && layout != layouts.end() &&
        (node.type == "panel" || node.type == "image")) {
        const auto visual = visuals.find(node.sprite);
        if (visual == visuals.end()) throw std::runtime_error("pelican.ui v1 unresolved sprite: " + node.sprite);
        emitVisual(node, *layout->second, visual->second, viewport, commands, node.color);
    }
    if (visible && node.type == "button" && layout != layouts.end()) {
        auto color = node.color;
        if (state && state->pressed) color = node.pressed_color;
        else if (state && state->hovered) color = node.hover_color;
        emitVisual(node, *layout->second, visuals.at(""), viewport, commands, color);
    }
    if (visible && (node.type == "label" || node.type == "button") &&
        layout != layouts.end() && !node.text.empty()) {
        const auto size = font.measure(node.text);
        auto x = layout->second->rect_ui.left;
        auto y = layout->second->rect_ui.top;
        if (node.type == "button") {
            x += static_cast<std::int32_t>((layout->second->rect_ui.width() - size.x) / 2);
            y += static_cast<std::int32_t>((layout->second->rect_ui.height() - size.y) / 2);
        }
        UiModule::ResolvedVisual glyph_visual{font_page_id, "engine:debug_text_font", {},
                                               {static_cast<std::int32_t>(font.atlasWidth()),
                                                static_cast<std::int32_t>(font.atlasHeight())}};
        for (const auto &glyph : font.layout(x, y, node.text)) {
            glyph_visual.source_rect = glyph.source;
            appendQuad(commands, node, *layout->second, glyph.destination, glyph.source,
                       glyph_visual, viewport, node.text_color, Sampler::Nearest);
        }
    }
    for (const auto &child : node.children)
        visitNodes(child, layouts, visuals, arena, widget_ids, font, font_page_id, viewport, commands);
}

const DocumentNode *findNodeRecursive(const DocumentNode &node, std::string_view path) {
    if (node.path == path) return &node;
    for (const auto &child : node.children)
        if (const auto *found = findNodeRecursive(child, path)) return found;
    return nullptr;
}

} // namespace

UiModule::UiModule() : UiModule(nlohmann::json::parse(GET_MODULE(ProjectBasicConfig).uiConfigJson())) {}

UiModule::UiModule(const nlohmann::json &document_json) : bitmap_font{BitmapFont::bundledDebugFont()} {
    ++parser_invocations;
    requireDocument(parseUiDocument(document_json), document);
    visuals.emplace("", ResolvedVisual{});
    loadVisuals(document.root);
    const auto has_text = [](const auto &self, const DocumentNode &node) -> bool {
        if ((node.type == "label" || node.type == "button") && !node.text.empty()) return true;
        return std::ranges::any_of(node.children, [&](const auto &child) { return self(self, child); });
    };
    if (has_text(has_text, document.root)) {
        if (texture_pages.size() + 1 >= std::numeric_limits<std::uint16_t>::max())
            throw std::runtime_error("limit_exceeded: UI texture page count exceeds uint16");
        font_page_id = static_cast<std::uint16_t>(texture_pages.size() + 1);
        texture_pages.push_back({.id = font_page_id, .stable_name = "engine:debug_text_font",
                                 .embedded_resource = "debug_text_font.png",
                                 .size = {static_cast<std::int32_t>(bitmap_font.atlasWidth()),
                                          static_cast<std::int32_t>(bitmap_font.atlasHeight())}});
    }
    createRuntimeWidgets(document.root);
}

void UiModule::loadVisuals(const DocumentNode &node) {
    if (!node.sprite.empty() && !visuals.contains(node.sprite)) {
        const auto resolved = GET_MODULE(PathResolver).resolveExistingFileReference(node.sprite);
        const auto *fragment = std::get_if<ResolvedPathFragment>(&resolved);
        if (fragment == nullptr || fragment->fragment.kind != "sprite")
            throw std::runtime_error("pelican.ui v1 expected #sprite/name reference: " + node.sprite);
        std::ifstream stream{fragment->path};
        if (!stream) throw std::runtime_error("pelican.atlas v1 file not found: " + fragment->path.string());
        const auto atlas = parseAtlasV1(nlohmann::json::parse(stream), fragment->path);
        const auto &sprite = findAtlasSprite(atlas, fragment->fragment.path);
        const auto &page = atlas.pages.at(sprite.page);
        auto found_page = std::find_if(texture_pages.begin(), texture_pages.end(),
                                       [&](const UiTexturePage &entry) { return entry.image_path == page.image_path; });
        std::uint16_t page_id;
        if (found_page == texture_pages.end()) {
            if (texture_pages.size() + 1 >= std::numeric_limits<std::uint16_t>::max())
                throw std::runtime_error("limit_exceeded: UI texture page count exceeds uint16");
            page_id = static_cast<std::uint16_t>(texture_pages.size() + 1);
            texture_pages.push_back({.id = page_id,
                                     .stable_name = "atlas:" + fragment->path.generic_string() + "/page:" + std::to_string(sprite.page),
                                     .image_path = page.image_path, .size = page.size});
        } else page_id = found_page->id;
        const auto &registered_page = texture_pages.at(page_id - 1);
        visuals.emplace(node.sprite, ResolvedVisual{page_id, registered_page.stable_name, sprite.rect, page.size});
    }
    for (const auto &child : node.children) loadVisuals(child);
}

void UiModule::createRuntimeWidgets(const DocumentNode &node) {
    const auto id = widget_arena.create({.stable_id = node.path, .type = node.type,
        .layer = node.layer, .decl_seq = node.decl_seq,
        .visible = node.visibility == Visibility::Visible, .enabled = node.enabled,
        .hit_testable = node.hit_testable && node.type == "button", .overflow_clip = node.overflow_clip,
        .text = node.text, .value = node.value, .checked = node.checked});
    traversal.push_back(id);
    widget_ids.emplace(node.path, id);
    for (const auto &child : node.children) createRuntimeWidgets(child);
}

void UiModule::syncLayout(vk::Extent2D extent, double ui_scale) {
    const auto viewport = ViewportTransform::letterboxed(
        {static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height)}, ui_scale);
    const auto solved = solveLayout(document, viewport.content_rect_ui);
    if (!solved) throw std::runtime_error("pelican.ui layout failed before input routing");
    for (const auto &record : solved.widgets) {
        const auto found = widget_ids.find(record.id);
        if (found == widget_ids.end()) continue;
        auto *state = widget_arena.resolve(found->second);
        state->rect_ui = record.rect_ui;
        state->clip_ui = record.clip_ui;
    }
}

const DocumentNode *UiModule::findNode(std::string_view path) const {
    return findNodeRecursive(document.root, path);
}

void UiModule::emitBindings(const DocumentNode &node, std::string_view trigger,
                            const RoutedPointerEvent &event) {
    for (const auto &binding : node.emits) {
        if (binding.trigger != trigger) continue;
        nlohmann::json payload = nlohmann::json::object();
        for (const auto &field : binding.fields) {
            switch (field.source) {
            case EmitSourceKind::Static: payload[field.name] = nlohmann::json::parse(field.static_json); break;
            case EmitSourceKind::StableId: payload[field.name] = node.path; break;
            case EmitSourceKind::DragDeltaUi:
                payload[field.name] = {event.drag_delta_ui.x, event.drag_delta_ui.y};
                break;
            case EmitSourceKind::WidgetValue:
                if (field.widget_path == "text") payload[field.name] = node.text;
                else if (field.widget_path == "checked") payload[field.name] = node.checked ? 1 : 0;
                else payload[field.name] = node.value;
                break;
            }
        }
        const void *payload_ptr = binding.payload_object ? static_cast<const void *>(&payload) : nullptr;
        internal::emitEventByName(binding.event, payload_ptr);
    }
}

void UiModule::handlePointer(const RoutedPointerEvent &event) {
    if (std::ranges::find(event.effects, PointerEffect::HoverExit) != event.effects.end() && hovered_widget) {
        command_buffer.push(SetHovered{*hovered_widget, false});
        hovered_widget.reset();
    }
    if (std::ranges::find(event.effects, PointerEffect::HoverEnter) != event.effects.end() && event.hover_target) {
        command_buffer.push(SetHovered{*event.hover_target, true});
        hovered_widget = event.hover_target;
    }
    if (!event.target) return;
    const auto *state = widget_arena.resolve(*event.target);
    const auto *node = state ? findNode(state->stable_id) : nullptr;
    if (node == nullptr || node->type != "button") return;
    for (const auto effect : event.effects) {
        switch (effect) {
        case PointerEffect::Capture: command_buffer.push(SetPressed{*event.target, true}); break;
        case PointerEffect::ReleaseCapture:
        case PointerEffect::Cancel: command_buffer.push(SetPressed{*event.target, false}); break;
        case PointerEffect::Click: emitBindings(*node, "on_click", event); break;
        case PointerEffect::DragStart: emitBindings(*node, "on_drag_start", event); break;
        case PointerEffect::Drag: emitBindings(*node, "on_drag", event); break;
        default: break;
        }
    }
}

RouteFrameResult UiModule::routeFrameInput(Pelican::InputState &input, vk::Extent2D extent, double ui_scale) {
    syncLayout(extent, ui_scale);
    const auto viewport = ViewportTransform::letterboxed(
        {static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height)}, ui_scale);
    last_route = input_router.routeFrameInput(input.currentFrameInput(), widget_arena, traversal, viewport,
                                               [&](const auto &event) { handlePointer(event); });
    last_commit = command_buffer.commit(widget_arena);
    if (last_route.consumed_pointer) input.consumePointerForActions();
    return last_route;
}

DrawBatch UiModule::buildFrame(vk::Extent2D extent, double ui_scale) const {
    if (extent.width > static_cast<std::uint32_t>(INT32_MAX) || extent.height > static_cast<std::uint32_t>(INT32_MAX))
        throw std::runtime_error("limit_exceeded: UI framebuffer extent exceeds int32");
    const auto viewport = ViewportTransform::letterboxed(
        {static_cast<std::int32_t>(extent.width), static_cast<std::int32_t>(extent.height)}, ui_scale);
    const auto solved = solveLayout(document, viewport.content_rect_ui);
    if (!solved) {
        const auto &error = solved.errors.front();
        throw std::runtime_error("pelican.ui v1 " + std::string{toString(error.code)} + " at " + error.path +
                                 ": " + error.message);
    }
    std::map<std::string, const LayoutRecord *, std::less<>> layouts;
    std::map<RectI, std::uint16_t> clip_ids;
    for (const auto &record : solved.widgets) {
        layouts.emplace(record.id, &record);
        if (!clip_ids.contains(record.clip_ui)) {
            if (clip_ids.size() >= maxUniqueClips)
                throw std::runtime_error("limit_exceeded: UI document '" + document.key + "' has more than 256 unique clips");
            clip_ids.emplace(record.clip_ui, static_cast<std::uint16_t>(clip_ids.size()));
        }
    }
    std::vector<QuadCommand> commands;
    visitNodes(document.root, layouts, visuals, widget_arena, widget_ids, bitmap_font,
               font_page_id, viewport, commands);
    for (auto &command : commands) {
        const auto layout = layouts.at(command.widget_id);
        command.key.clip_id = clip_ids.at(layout->clip_ui);
    }
    return buildDrawBatch(std::move(commands), document.key);
}

} // namespace Pelican::ui
