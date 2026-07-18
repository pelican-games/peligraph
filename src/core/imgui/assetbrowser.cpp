#include "assetbrowser.hpp"

#include "imguiruntime.hpp"

#include <imgui.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

EditorListAssetsResult
AssetBrowserQueryAdapter::listAssets(const EditorListAssetsRequest &request) const {
    ++trace_.query_calls;
    return service_.listAssets(request);
}

bool invokeAssetBrowserPanelCallback(const EngineLaunchConfig &config,
                                     AssetBrowserPanelTrace &trace,
                                     const std::function<void()> &callback) {
    return invokeImGuiRuntimeCallback(config, [&] {
        ++trace.panel_callback_calls;
        callback();
    });
}

namespace {

std::vector<std::string> distinctValues(const std::vector<EditorAssetQueryResult> &assets,
                                        std::string EditorAssetQueryResult::*member) {
    std::vector<std::string> values;
    values.reserve(assets.size());
    for (const auto &asset : assets) values.push_back(asset.*member);
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

void drawFilterCombo(const char *label, const std::vector<std::string> &values,
                     std::string &selected) {
    const auto *preview = selected.empty() ? "All" : selected.c_str();
    if (!ImGui::BeginCombo(label, preview)) return;
    if (ImGui::Selectable("All", selected.empty())) selected.clear();
    for (const auto &value : values) {
        const bool is_selected = selected == value;
        if (ImGui::Selectable(value.c_str(), is_selected)) selected = value;
        if (is_selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
}

} // namespace

struct AssetBrowserPanel::Impl {
    AssetBrowserQueryAdapter query;
    std::vector<EditorAssetQueryResult> assets;
    std::string kind_filter;
    std::string status_filter;
    std::string selected_reference;
    std::string error;
    bool initialized = false;

    Impl(const EditorCommandService &service, AssetBrowserPanelTrace &trace)
        : query{service, trace} {}

    void refresh() {
        try {
            assets = query.listAssets().assets;
            error.clear();
            if (selected_reference.empty() && !assets.empty()) {
                selected_reference = assets.front().id;
            }
            if (!selected_reference.empty() &&
                std::none_of(assets.begin(), assets.end(), [&](const auto &asset) {
                    return asset.id == selected_reference;
                })) {
                selected_reference.clear();
            }
        } catch (const EditorCommandError &query_error) {
            error = std::string{editorCommandErrorCodeName(query_error.code())} + ": " +
                    query_error.what();
        } catch (const std::exception &query_error) {
            error = query_error.what();
        }
        initialized = true;
    }

    bool visible(const EditorAssetQueryResult &asset) const {
        return (kind_filter.empty() || asset.kind == kind_filter) &&
               (status_filter.empty() || asset.status == status_filter);
    }

    const EditorAssetQueryResult *selectedAsset() const {
        const auto found = std::find_if(assets.begin(), assets.end(), [&](const auto &asset) {
            return asset.id == selected_reference;
        });
        return found == assets.end() ? nullptr : &*found;
    }
};

AssetBrowserPanel::AssetBrowserPanel(const EditorCommandService &service,
                                     AssetBrowserPanelTrace &trace)
    : impl{std::make_unique<Impl>(service, trace)} {}

AssetBrowserPanel::~AssetBrowserPanel() = default;

void AssetBrowserPanel::draw(bool *open) {
    if (!impl->initialized) impl->refresh();

    ImGui::SetNextWindowPos({38.0f, 72.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({900.0f, 440.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Pelican Asset Browser", open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Read-only - project asset references");
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) impl->refresh();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu assets", impl->assets.size());

    const auto kinds = distinctValues(impl->assets, &EditorAssetQueryResult::kind);
    const auto statuses = distinctValues(impl->assets, &EditorAssetQueryResult::status);
    ImGui::SetNextItemWidth(180.0f);
    drawFilterCombo("Kind", kinds, impl->kind_filter);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    drawFilterCombo("Status", statuses, impl->status_filter);

    if (!impl->error.empty()) {
        ImGui::TextColored({1.0f, 0.42f, 0.35f, 1.0f}, "Asset query error: %s",
                           impl->error.c_str());
    }

    auto table_height = ImGui::GetContentRegionAvail().y - 118.0f;
    table_height = std::max(table_height, 180.0f);
    if (ImGui::BeginTable("asset_inventory", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp,
                          {0.0f, table_height})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Reference", ImGuiTableColumnFlags_WidthStretch, 1.25f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Store", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Provenance path", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();

        for (const auto &asset : impl->assets) {
            if (!impl->visible(asset)) continue;
            ImGui::PushID(asset.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool selected = impl->selected_reference == asset.id;
            if (ImGui::Selectable(asset.id.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                impl->selected_reference = asset.id;
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(asset.kind.c_str());
            ImGui::TableNextColumn();
            if (asset.status == "missing") {
                ImGui::TextColored({1.0f, 0.45f, 0.35f, 1.0f}, "%s", asset.status.c_str());
            } else {
                ImGui::TextUnformatted(asset.status.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(asset.store.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(asset.path.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Selected reference and provenance");
    if (const auto *asset = impl->selectedAsset()) {
        ImGui::Text("reference: %s", asset->id.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Copy reference")) ImGui::SetClipboardText(asset->id.c_str());
        ImGui::Text("path: %s", asset->path.c_str());
        ImGui::Text("store: %s   kind: %s   status: %s", asset->store.c_str(),
                    asset->kind.c_str(), asset->status.c_str());
    } else {
        ImGui::TextDisabled("Select an asset to copy its reference.");
    }

    ImGui::End();
}

} // namespace Pelican
