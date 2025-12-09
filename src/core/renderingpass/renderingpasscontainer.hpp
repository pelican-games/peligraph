#pragma once

#include "../container.hpp"
#include "renderingpass.hpp"
#include "rendertargetcontainer.hpp"
#include "../shader/shader.hpp"
#include "../vkcore/image.hpp"
#include <vulkan/vulkan.hpp>
#include <vector>
#include <string>
#include <unordered_map>

namespace Pelican {

// パスの種類
enum class PassType {
    eMaterial,      // マテリアルレンダリング
    eFullscreen,    // フルスクリーンポストプロセス
};

// 1つのパスの定義
struct PassDefinition {
    std::string name;
    PassType type;
    
    // 出力ターゲット
    GlobalRenderTargetId output_color;
    GlobalRenderTargetId output_depth;  // -1なら深度なし
    
    // 入力ターゲット（フルスクリーンパス用）
    std::vector<GlobalRenderTargetId> input_targets;
    
    // フルスクリーンパスの場合のパイプライン情報
    struct {
        GlobalShaderId vert_shader;
        GlobalShaderId frag_shader;
    } fullscreen_info;
    
    // レンダリング設定
    vk::AttachmentLoadOp color_load_op = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp color_store_op = vk::AttachmentStoreOp::eStore;
    vk::ClearColorValue clear_color = vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}};
};

// レンダリングパス全体の定義
struct RenderingPassDefinition {
    std::string name;
    std::vector<PassDefinition> passes;
};

DECLARE_MODULE(RenderingPassContainer) {
    
    struct InternalRenderingPass {
        RenderingPassDefinition definition;
        std::vector<PassId> pass_ids;  // 各パスの実行時ID（フルスクリーンパスのPipelineIdなど）
    };
    
    ResourceContainer<RenderingPassId, InternalRenderingPass> rendering_passes;
    std::unordered_map<std::string, RenderingPassId> name_to_id;

  public:
    RenderingPassContainer();
    ~RenderingPassContainer();

    // レンダリングパスの登録
    RenderingPassId registerRenderingPass(const RenderingPassDefinition& definition);

    // jsonからレンダリングパスを登録
    void registerRenderingPassFromJson(const std::string& json_path);
    
    // 名前からIDを取得
    RenderingPassId getRenderingPassIdByName(const std::string& name) const;
    
    // パスのリストを取得
    std::span<const PassId> getPasses(RenderingPassId rendering_pass_id) const;
    
    // パスの定義を取得
    const PassDefinition& getPassDefinition(RenderingPassId rendering_pass_id, size_t pass_index) const;
    
    // パス数を取得
    size_t getPassCount(RenderingPassId rendering_pass_id) const;
};

} // namespace Pelican