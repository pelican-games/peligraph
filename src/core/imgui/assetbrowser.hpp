#pragma once

#include "../communication/editorcommandservice.hpp"
#include "../launchconfig.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace Pelican {

struct AssetBrowserPanelTrace {
    std::uint64_t panel_callback_calls = 0;
    std::uint64_t query_calls = 0;
    std::uint64_t edit_enqueue_calls = 0;
};

class AssetBrowserQueryAdapter {
    const EditorCommandService &service_;
    AssetBrowserPanelTrace &trace_;

  public:
    AssetBrowserQueryAdapter(const EditorCommandService &service, AssetBrowserPanelTrace &trace)
        : service_{service}, trace_{trace} {}

    EditorListAssetsResult listAssets(const EditorListAssetsRequest &request = {}) const;
};

// Uses the same deterministic-driver gate as the rest of the engine ImGui
// integration.  The trace increment is inside the gated callback so a closed
// gate proves that neither the panel nor its query adapter ran.
bool invokeAssetBrowserPanelCallback(const EngineLaunchConfig &config,
                                     AssetBrowserPanelTrace &trace,
                                     const std::function<void()> &callback);

class AssetBrowserPanel {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    AssetBrowserPanel(const EditorCommandService &service, AssetBrowserPanelTrace &trace);
    ~AssetBrowserPanel();

    void draw(bool *open);
};

} // namespace Pelican
