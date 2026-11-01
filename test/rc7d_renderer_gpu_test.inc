TEST_CASE("RC-7d D3 lazy allocation fallback preserves pixels", "[rc7d][rc7d-d3][gpu][headless]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();const auto dir=makeTempProjectDir();
    FastModuleContainer modules;RuntimeTeardownGuard teardown{RuntimeTeardownMode::terminal_shutdown};
    writeTextFile(dir/"scene.json",R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})");
    writeTextFile(dir/"assets.json",R"({"schema":"pelican.asset_data","version":2,"models":[]})");
    writeTextFile(dir/"shaders/local_read_fullscreen.vert",gpuArenaFullscreenVertexShader());
    writeTextFile(dir/"shaders/local_read_producer.frag",localReadProducerFragmentShader());
    writeTextFile(dir/"shaders/local_read_copy.frag",localReadCopyFragmentShader());
    writeTextFile(dir/"shaders/local_read_present.frag",gpuArenaCopyFragmentShader());
    auto config=localReadRenderingConfig();config["rendering_passes"][0]["name"]="main_render";
    config["target_planning"]["graphs"]["main_render"]["resources"]["local_source"]["no_alias"]=true;
    auto root=TestSupport::renderGraphRoot(config);
    auto initial=root.dump();
    for(const auto &[from,to]:std::vector<std::pair<std::string,std::string>>{{"local_source","initial_source"},{"local_output","initial_output"}})
        for(std::size_t pos=0;(pos=initial.find(from,pos))!=std::string::npos;pos+=to.size())initial.replace(pos,from.size(),to);
    writeTextFile(dir/"hybrid.json",initial);configureRayQueryTestRuntime(dir);
    auto &launch=GET_MODULE(EngineLaunchConfig);launch.vulkan_validation=VulkanValidationMode::enabled;launch.gpu_labels=true;
    auto &core=GET_MODULE(VulkanManageCore);Rc7bValidationCapture validation{core};
    REQUIRE(core.getRuntimeCapabilities().dynamic_rendering_local_read);
    auto &renderer=GET_MODULE(Renderer);auto &targets=GET_MODULE(RenderTargetContainer);
    RenderingPassConfigRegistrationDependencies::Options options;
    options.runtime_memory_types_for_testing=[](std::string_view name,vk::Extent2D,std::vector<ImageMemoryType> types) {
        if(name=="local_source")for(auto &t:types)t.property_flags|=0x10u;
        return types;
    };
    const auto replacement=registerRenderingPassConfigFromJsonData(root.dump(),{32,32},gpuArenaRegistrationDependencies(std::move(options)));
    RendererTestAccess::selectProgram(renderer,replacement.rendering_pass_ids.front());
    const auto source=targets.getRenderTargetIdByName("local_source");
    const auto actual=targets.actualPlacement(std::array{source});const auto &a=actual.at("regions").at(0);
    CHECK(a.at("kind")=="lazy");
    CHECK(a.at("vma_usage")==static_cast<std::uint32_t>(vma::MemoryUsage::eGpuLazilyAllocated));
    CHECK(a.at("vma_required_flags")==static_cast<std::uint32_t>(vk::MemoryPropertyFlagBits::eLazilyAllocated));
    CHECK(a.at("vma_dedicated")==true);CHECK(a.at("fallback_reason")=="lazy_type_unavailable");
    CHECK((a.at("property_flags").get<std::uint32_t>()&0x10u)==0);
    renderer.render();core.waitIdle();
    const auto pixels=GET_MODULE(RenderTarget).readbackLastFrameRGBA8();REQUIRE(pixels.size()==32u*32u*4u);
    std::size_t wrong=0;for(std::size_t i=0;i<pixels.size();i+=4)if(pixels[i]!=231 || pixels[i+1]!=170 || pixels[i+2]!=124 || pixels[i+3]!=255)++wrong;
    CHECK(wrong==0);
    std::ofstream{"<作業ディレクトリ>/rc7d_d3.json"}<<actual.dump(2);
    for(const auto &[phase,message]:validation.messages){INFO(message);CHECK(message.empty());}
    std::filesystem::remove_all(dir);
#endif
}
TEST_CASE("RC-7d D4 ordinary transient and tile local share disjoint scope ranges", "[rc7d][rc7d-d4][gpu][headless]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger();const auto dir=makeTempProjectDir();
    FastModuleContainer modules;RuntimeTeardownGuard teardown{RuntimeTeardownMode::terminal_shutdown};
    writeTextFile(dir/"scene.json",R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})");
    writeTextFile(dir/"assets.json",R"({"schema":"pelican.asset_data","version":2,"models":[]})");
    writeTextFile(dir/"shaders/local_read_fullscreen.vert",gpuArenaFullscreenVertexShader());
    writeTextFile(dir/"shaders/local_read_producer.frag",R"(#version 450
layout(location=0) out vec4 color;layout(location=1) out vec4 scratch;
void main(){color=vec4(.2,.4,.8,1);scratch=vec4(.3,.5,.7,1);})");
    writeTextFile(dir/"shaders/local_read_copy.frag",R"(#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(location=0) in vec2 inUV;layout(location=0) out vec4 color;
void main(){vec4 value=pelican_sample_input_color(inUV);color=vec4(value.b,value.g,value.r,1);})");
    writeTextFile(dir/"shaders/local_read_present.frag",gpuArenaCopyFragmentShader());
    auto config=localReadRenderingConfig();config["rendering_passes"][0]["name"]="main_render";
    auto scratch=config["render_targets"][0];scratch["name"]="scratch";scratch["usage"]={"COLOR_ATTACHMENT"};config["render_targets"].push_back(scratch);
    config["rendering_passes"][0]["passes"][0]["output"]["color"]={"local_source","scratch"};
    config["rendering_passes"][0]["passes"][0]["type"]="raster";
    config["rendering_passes"][0]["passes"][0]["draw"]={{"vertex_count",6}};
    writeTextFile(dir/"hybrid.json",TestSupport::renderGraphRoot(config).dump(2));configureRayQueryTestRuntime(dir);
    auto &launch=GET_MODULE(EngineLaunchConfig);launch.vulkan_validation=VulkanValidationMode::enabled;launch.gpu_labels=true;
    auto &core=GET_MODULE(VulkanManageCore);Rc7bValidationCapture validation{core};
    REQUIRE(core.getVulkanValidationStatus().synchronization_validation);
    auto &renderer=GET_MODULE(Renderer);auto &targets=GET_MODULE(RenderTargetContainer);
    const auto source=targets.getRenderTargetIdByName("local_source"),out=targets.getRenderTargetIdByName("local_output"),temp=targets.getRenderTargetIdByName("scratch");
    CHECK(targets.getMetadata(source).storage_mode==RenderTargetStorageMode::tile_local_attachment);
    CHECK(targets.getMetadata(temp).storage_mode==RenderTargetStorageMode::transient_attachment);
    const auto check=[&] {
        const auto &m=targets.getImage(out);
        for(const auto id:{source,temp}) {
            const auto &t=targets.getImage(id);REQUIRE(t.region_bound);REQUIRE(t.allocation==m.allocation);
            CHECK(t.memory_kind=="ordinary");CHECK(t.memory_offset!=m.memory_offset);
            CHECK((t.memory_offset+t.memory_size<=m.memory_offset || m.memory_offset+m.memory_size<=t.memory_offset));
        }
    };check();
    auto &tracker=RendererTestAccess::imageLayoutTracker(renderer);std::vector<RenderTargetLayoutTracker::AliasTransition> trace;
    tracker.alias_transition_observer=[&](const auto &e){trace.push_back(e);};
    for(int i=0;i<3;++i){renderer.render();core.waitIdle();}
    REQUIRE_FALSE(trace.empty());std::size_t dependencies=0,inside=0;
    for(const auto &e:trace)if(e.dependency){++dependencies;if(e.inside_rendering)++inside;}
    CHECK(dependencies>0);CHECK(inside==0);
    auto pixels=GET_MODULE(RenderTarget).readbackLastFrameRGBA8();REQUIRE(pixels.size()==32u*32u*4u);
    const auto checkPixels=[](const auto &bytes) {
        std::size_t wrong=0;for(std::size_t i=0;i<bytes.size();i+=4)if(bytes[i]!=231 || bytes[i+1]!=170 || bytes[i+2]!=124 || bytes[i+3]!=255)++wrong;
        CHECK(wrong==0);
    };checkPixels(pixels);
    RendererTestAccess::recreateRenderTargetsAndRebind(renderer,{48,48});check();renderer.render();core.waitIdle();
    checkPixels(GET_MODULE(RenderTarget).readbackLastFrameRGBA8());
    dependencies=inside=0;for(const auto &e:trace)if(e.dependency){++dependencies;if(e.inside_rendering)++inside;}
    CHECK(inside==0);
    tracker.alias_transition_observer={};
    for(const auto &[phase,message]:validation.messages){INFO(message);CHECK(message.empty());}
    std::ofstream{"<作業ディレクトリ>/rc7d_d4.json"}<<nlohmann::json{{"actual",targets.actualPlacement(std::array{source,out,temp})},
        {"dependencies",dependencies},{"inside_rendering_dependencies",inside},{"validation_messages",validation.messages.size()}}.dump(2);
    std::filesystem::remove_all(dir);
#endif
}
