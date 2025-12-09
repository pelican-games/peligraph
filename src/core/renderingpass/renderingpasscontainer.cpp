#include "renderingpasscontainer.hpp"
#include "rendertargetcontainer.hpp"
#include "../fullscreenpass/fullscreenpasscontainer.hpp"
#include "../shader/shadercontainer.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/rendertarget.hpp"

#include "../loader/basicconfig.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

namespace Pelican {

// フォーマット文字列をvk::Formatに変換
vk::Format stringToFormat(const std::string& format_str) {
    static const std::unordered_map<std::string, vk::Format> format_map = {
        {"B8G8R8A8_UNORM", vk::Format::eB8G8R8A8Unorm},
        {"R8G8B8A8_UNORM", vk::Format::eR8G8B8A8Unorm},
        {"D32_SFLOAT", vk::Format::eD32Sfloat},
        {"D24_UNORM_S8_UINT", vk::Format::eD24UnormS8Uint},
        {"D16_UNORM", vk::Format::eD16Unorm},
    };
    
    auto it = format_map.find(format_str);
    if (it != format_map.end()) {
        return it->second;
    }
    throw std::runtime_error("Unknown format: " + format_str);
}

// 使用フラグ文字列をvk::ImageUsageFlagsに変換
vk::ImageUsageFlags stringToUsageFlags(const std::vector<std::string>& usage_strs) {
    vk::ImageUsageFlags flags;
    static const std::unordered_map<std::string, vk::ImageUsageFlagBits> usage_map = {
        {"COLOR_ATTACHMENT", vk::ImageUsageFlagBits::eColorAttachment},
        {"DEPTH_STENCIL_ATTACHMENT", vk::ImageUsageFlagBits::eDepthStencilAttachment},
        {"SAMPLED", vk::ImageUsageFlagBits::eSampled},
        {"STORAGE", vk::ImageUsageFlagBits::eStorage},
        {"TRANSFER_DST", vk::ImageUsageFlagBits::eTransferDst},
        {"TRANSFER_SRC", vk::ImageUsageFlagBits::eTransferSrc},
    };
    
    for (const auto& usage_str : usage_strs) {
        auto it = usage_map.find(usage_str);
        if (it != usage_map.end()) {
            flags |= it->second;
        }
    }
    return flags;
}

// PassType文字列をPassTypeに変換
PassType stringToPassType(const std::string& type_str) {
    if (type_str == "material") {
        return PassType::eMaterial;
    } else if (type_str == "fullscreen") {
        return PassType::eFullscreen;
    }
    throw std::runtime_error("Unknown pass type: " + type_str);
}

RenderingPassContainer::RenderingPassContainer() {}

RenderingPassContainer::~RenderingPassContainer() {}

RenderingPassId RenderingPassContainer::registerRenderingPass(const RenderingPassDefinition& definition) {
    // 既存チェック
    if (auto it = name_to_id.find(definition.name); it != name_to_id.end()) {
        return it->second;
    }

    InternalRenderingPass internal_pass;
    internal_pass.definition = definition;
    
    // 各パスの準備
    for (int i = 0; i < definition.passes.size(); ++i) {
        const auto& pass_def = definition.passes[i];
        
        if (pass_def.type == PassType::eFullscreen) {
            // フルスクリーンパスのパイプライン登録
            // スワップチェーン出力の場合はスキップ（-2の場合）
            auto& rt_module = GET_MODULE(RenderTarget);
            auto& rt_container = GET_MODULE(RenderTargetContainer);

            vk::Format color_fmt{};
            if (pass_def.output_color.value >= 0) {
                const auto& output_rt = rt_container.get(pass_def.output_color);
                color_fmt = output_rt.image.format;
            } else { // swapchain
                color_fmt = rt_module.getSwapchainFormat();
            }

            auto& shader_container = GET_MODULE(ShaderContainer);
            auto vert_shader = shader_container.getShader(pass_def.fullscreen_info.vert_shader);
            auto frag_shader = shader_container.getShader(pass_def.fullscreen_info.frag_shader);

            auto& fs_container = GET_MODULE(FullscreenPassContainer);
            auto pipeline_id = fs_container.registerFullscreenPass(color_fmt, vert_shader, frag_shader);

            internal_pass.pass_ids.push_back(PassId{static_cast<int>(pipeline_id.value)});
        } else {
            // ジオメトリパスの場合はインデックスをそのまま使用
            internal_pass.pass_ids.push_back(PassId{i});
        }
    }
    
    auto id = rendering_passes.reg(std::move(internal_pass));
    name_to_id.emplace(definition.name, id);
    return id;
}

void RenderingPassContainer::registerRenderingPassFromJson(const std::string& json_path) {
    auto &config = GET_MODULE(ProjectBasicConfig);
    auto& rt_container = GET_MODULE(RenderTargetContainer);
    auto& shader_container = GET_MODULE(ShaderContainer);

    // JSONファイルを読み込み (scene.cppと同じパターン)
    std::string json_str;
    {
        const auto sz = std::filesystem::file_size(json_path);
        std::ifstream f{json_path, std::ios_base::binary};
        if (!f.is_open()) {
            throw std::runtime_error("Failed to open rendering pass JSON: " + json_path);
        }
        json_str.resize(sz, '\0');
        f.read(json_str.data(), sz);
    }

    const auto rendering_pass_data = nlohmann::json::parse(json_str);

    // ベースとなるウィンドウサイズを取得
    const auto window_size = config.initialWindowSize();
    const vk::Extent2D base_extent(
        static_cast<uint32_t>(window_size.width), 
        static_cast<uint32_t>(window_size.height)
    );

    // render_targets セクションを処理
    if (rendering_pass_data.contains("render_targets")) {
        const auto& render_targets = rendering_pass_data.at("render_targets");
        for (const auto& rt_json : render_targets) {
            const std::string name = rt_json.at("name");
            const float extent_scale = rt_json.at("extent_scale");
            const std::string format_str = rt_json.at("format");
            const std::vector<std::string> usage_strs = rt_json.at("usage");
            
            // 初期解像度（スケール適用後に変更可能）
            vk::Extent2D extent;
            extent.width = static_cast<uint32_t>(base_extent.width * extent_scale);
            extent.height = static_cast<uint32_t>(base_extent.height * extent_scale);
            
            rt_container.registerRenderTarget(
                name,
                extent,
                stringToFormat(format_str),
                stringToUsageFlags(usage_strs),
                vma::MemoryUsage::eAutoPreferDevice
            );
        }
    }

    // rendering_passes セクションを処理
    if (rendering_pass_data.contains("rendering_passes")) {
        const auto& rendering_passes_json = rendering_pass_data.at("rendering_passes");
        for (const auto& pass_set_json : rendering_passes_json) {
            RenderingPassDefinition pass_def;
            pass_def.name = pass_set_json.at("name");
            
            const auto& passes = pass_set_json.at("passes");
            for (const auto& pass_json : passes) {
                PassDefinition individual_pass;
                individual_pass.name = pass_json.at("name");
                individual_pass.type = stringToPassType(pass_json.at("type"));
                
                // 出力ターゲット
                const auto& output = pass_json.at("output");
                const std::string output_color_name = output.at("color");
                
                if (output_color_name != "swapchain") {
                    individual_pass.output_color = rt_container.getRenderTargetIdByName(output_color_name);
                    if (individual_pass.output_color.value < 0) {
                        throw std::runtime_error("Render target not found: " + output_color_name);
                    }
                } else {
                    // スワップチェーン用の特殊ID（-2など、-1以外の無効値）
                    individual_pass.output_color = GlobalRenderTargetId{-2};
                }
                
                // 深度ターゲット（nullable）
                if (!output.at("depth").is_null()) {
                    const std::string output_depth_name = output.at("depth");
                    individual_pass.output_depth = rt_container.getRenderTargetIdByName(output_depth_name);
                } else {
                    individual_pass.output_depth = GlobalRenderTargetId{-1};
                }
                
                // フルスクリーンパスの場合
                if (individual_pass.type == PassType::eFullscreen) {
                    // 入力ターゲット
                    if (pass_json.contains("input")) {
                        const auto& inputs = pass_json.at("input");
                        for (const auto& input_name_json : inputs) {
                            const std::string input_name = input_name_json;
                            individual_pass.input_targets.push_back(
                                rt_container.getRenderTargetIdByName(input_name)
                            );
                        }
                    }

                    if (!individual_pass.input_targets.empty()) {
                        GET_MODULE(FullscreenPassContainer).setInputTexture(individual_pass.input_targets[0]);
                    }
                    
                    // シェーダーファイルパスからシェーダーを読み込んで登録
                    if (pass_json.contains("shader")) {
                        const auto& shader = pass_json.at("shader");
                        const std::string vert_shader_path = shader.at("vertex");
                        const std::string frag_shader_path = shader.at("fragment");
                        
                        // 頂点シェーダーを読み込み
                        {
                            const auto vert_sz = std::filesystem::file_size(vert_shader_path);
                            std::ifstream vert_f{vert_shader_path, std::ios_base::binary};
                            if (!vert_f.is_open()) {
                                throw std::runtime_error("Failed to open vertex shader: " + vert_shader_path);
                            }
                            std::vector<char> vert_data(vert_sz);
                            vert_f.read(vert_data.data(), vert_sz);
                            individual_pass.fullscreen_info.vert_shader = shader_container.registerShader(
                                vert_sz,
                                vert_data.data()
                            );
                        }
                        
                        // フラグメントシェーダーを読み込み
                        {
                            const auto frag_sz = std::filesystem::file_size(frag_shader_path);
                            std::ifstream frag_f{frag_shader_path, std::ios_base::binary};
                            if (!frag_f.is_open()) {
                                throw std::runtime_error("Failed to open fragment shader: " + frag_shader_path);
                            }
                            std::vector<char> frag_data(frag_sz);
                            frag_f.read(frag_data.data(), frag_sz);
                            individual_pass.fullscreen_info.frag_shader = shader_container.registerShader(
                                frag_sz,
                                frag_data.data()
                            );
                        }
                    }
                }   
                pass_def.passes.push_back(std::move(individual_pass));
            }
            registerRenderingPass(pass_def);
        }
    }
}

RenderingPassId RenderingPassContainer::getRenderingPassIdByName(const std::string& name) const {
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        return it->second;
    }
    return RenderingPassId{-1};
}

std::span<const PassId> RenderingPassContainer::getPasses(RenderingPassId rendering_pass_id) const {
    const auto& pass = rendering_passes.get(rendering_pass_id);
    return pass.pass_ids;
}

const PassDefinition& RenderingPassContainer::getPassDefinition(RenderingPassId rendering_pass_id, size_t pass_index) const {
    const auto& pass = rendering_passes.get(rendering_pass_id);
    return pass.definition.passes[pass_index];
}

size_t RenderingPassContainer::getPassCount(RenderingPassId rendering_pass_id) const {
    const auto& pass = rendering_passes.get(rendering_pass_id);
    return pass.definition.passes.size();
}

} // namespace Pelican