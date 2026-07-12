#include "module.hpp"

#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"

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
                const ViewportTransform &viewport) {
    const auto destination_px = viewport.uiToPx(destination);
    const auto scissor_px = intersect(viewport.uiToPx(layout.clip_ui), viewport.content_rect_px);
    if (destination_px.left >= destination_px.right || destination_px.top >= destination_px.bottom ||
        scissor_px.left >= scissor_px.right || scissor_px.top >= scissor_px.bottom) return;
    QuadCommand command;
    command.key = {node.sprite.empty() ? "rgba_straight" : "rgba_straight", node.sampler,
                   visual.texture_name, scissor_px, visual.page_id, 0};
    command.rect_px = destination_px;
    command.layer = node.layer;
    command.decl_seq = node.decl_seq;
    command.uv_rect = uvRect(source, visual.page_size);
    command.color = node.color;
    command.widget_id = node.path;
    out.push_back(std::move(command));
}

void emitVisual(const DocumentNode &node, const LayoutRecord &layout,
                const UiModule::ResolvedVisual &visual, const ViewportTransform &viewport,
                std::vector<QuadCommand> &commands) {
    if (!node.nine_patch) {
        appendQuad(commands, node, layout, layout.rect_ui, visual.source_rect, visual, viewport);
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
                   {sx[x], sy[y], sx[x + 1], sy[y + 1]}, visual, viewport);
    }
}

void visitNodes(const DocumentNode &node, const std::map<std::string, const LayoutRecord *, std::less<>> &layouts,
                const std::map<std::string, UiModule::ResolvedVisual, std::less<>> &visuals,
                const ViewportTransform &viewport, std::vector<QuadCommand> &commands) {
    const auto layout = layouts.find(node.path);
    if (node.visibility == Visibility::Visible && node.color[3] != 0 && layout != layouts.end() &&
        (node.type == "panel" || node.type == "image")) {
        const auto visual = visuals.find(node.sprite);
        if (visual == visuals.end()) throw std::runtime_error("pelican.ui v1 unresolved sprite: " + node.sprite);
        emitVisual(node, *layout->second, visual->second, viewport, commands);
    }
    for (const auto &child : node.children) visitNodes(child, layouts, visuals, viewport, commands);
}

} // namespace

UiModule::UiModule() : UiModule(nlohmann::json::parse(GET_MODULE(ProjectBasicConfig).uiConfigJson())) {}

UiModule::UiModule(const nlohmann::json &document_json) {
    ++parser_invocations;
    requireDocument(parseUiDocument(document_json), document);
    visuals.emplace("", ResolvedVisual{});
    loadVisuals(document.root);
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
            texture_pages.push_back({page_id,
                                     "atlas:" + fragment->path.generic_string() + "/page:" + std::to_string(sprite.page),
                                     page.image_path, page.size});
        } else page_id = found_page->id;
        const auto &registered_page = texture_pages.at(page_id - 1);
        visuals.emplace(node.sprite, ResolvedVisual{page_id, registered_page.stable_name, sprite.rect, page.size});
    }
    for (const auto &child : node.children) loadVisuals(child);
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
    visitNodes(document.root, layouts, visuals, viewport, commands);
    for (auto &command : commands) {
        const auto layout = layouts.at(command.widget_id);
        command.key.clip_id = clip_ids.at(layout->clip_ui);
    }
    return buildDrawBatch(std::move(commands), document.key);
}

} // namespace Pelican::ui
