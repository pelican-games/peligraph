#include "../src/core/vkcore/render_target_layout_tracker.hpp"
#include "../src/project/targetrendermemory.hpp"
#include "../src/core/vkcore/util.hpp"
#include "../src/core/vkcore/deletionqueue.hpp"
#include "../src/core/renderingpass/vulkanrendercompilerpackage.hpp"

namespace Pelican {
TEST_CASE("RC-7c fix2 batch reuses live relative seven mip region images at a smaller base", "[rc7c][gpu][fix2]") {
    setupLogger(); FastModuleContainer modules; Project project; startHeadless(project,{64,64});
    auto &targets=GET_MODULE(RenderTargetContainer);
    RenderTargetDefinition d; d.name="live_seven_mips"; d.mip_levels.count=7;
    d.usage=ImageUsageBit::color_attachment|ImageUsageBit::sampled;
    auto contract=std::make_shared<RuntimeTargetMemoryContract>();
    contract->inputs.resources={{d.name}}; contract->inputs.order={d.name};
    targets.registerMemoryTargets({d},{64,64},contract);
    const auto id=targets.getRenderTargetIdByName(d.name);
    const auto image=targets.getImage(id).image.get();
    const auto allocation=targets.getImage(id).allocation;
    const auto before=targets.actualPlacement(std::array{id});
    REQUIRE_NOTHROW(targets.registerMemoryTargets({d},{32,32},contract));
    CHECK(targets.getRenderTargetIdByName(d.name)==id);
    CHECK(targets.getImage(id).image.get()==image);
    CHECK(targets.getImage(id).allocation==allocation);
    CHECK(targets.getImage(id).extent==vk::Extent3D{64,64,1});
    CHECK(targets.getImage(id).mip_levels==7);
    CHECK(targets.actualPlacement(std::array{id})==before);
}

TEST_CASE("RC-7c fix1 runtime required groups belong to their declaring documents", "[rc7c][gpu][fix1]") {
    setupLogger(); FastModuleContainer modules; Project project; startHeadless(project,{64,64});
    Json targets=Json::array({target("scene","R16G16B16A16_SFLOAT"),
        target("depth","D32_SFLOAT",Json::array({"DEPTH_STENCIL_ATTACHMENT"}))});
    Json passes=Json::array({Json{{"name","draw"},{"type","material"},{"material_contract","forward_opaque_v1"},
        {"output",{{"color",Json::array({"scene"})},{"depth","depth"}}}},pass("present",{"scene"},"swapchain")});
    Json features=Json::array();
    for(const std::string prefix:{"a","b"}) {
        Json ft=Json::array(), fp=Json::array(); addChain(ft,fp,prefix,"R8G8B8A8_UNORM",true,true);
        Json inserted=Json::array(); for(auto &p:fp) inserted.push_back({{"insert","end"},{"pass",p}});
        const Json group={{"value","scratch"},{"strength","require"}};
        const Json resources={{prefix+"1",{{"alias_group",group}}},{prefix+"2",{{"alias_group",group}}},
            {prefix+"_mid",{{"no_alias",true}}},{prefix+"_end",{{"no_alias",true}}}};
        const auto name=prefix+".json";
        std::ofstream{project.root/name}<<Json{{"schema","pelican.render_feature"},{"version",1},{"name",prefix},
            {"render_targets",ft},{"passes",inserted},{"target_planning",{{"graphs",{{"main",{{"resources",resources}}}}}}}}.dump(2);
        features.push_back("project://"+name);
    }
    const auto root=TestSupport::renderGraphRoot(Json{{"features",features},{"render_targets",targets},
        {"rendering_passes",Json::array({{{"name","main"},{"passes",passes}}})}});
    RenderingPassConfigRegistrationDependencies::Options options;
    options.validate_prepared_generation=[](const RendererRuntimeGeneration &generation) {
        std::map<std::string,std::vector<std::string>> groups;
        for(const auto &[id,program]:generation.programs) for(const auto &e:program.frame_graph.target_plan->policy) {
            if(e.key=="alias_group") {
                REQUIRE(e.status==PlanningPolicyStatus::adopted);
                REQUIRE(e.strength==PlanningStrength::require);
                groups[e.origin.document].push_back(e.target);
            }
        }
        REQUIRE(groups.size()==2);
        for(const auto &[doc,names]:groups) CHECK(names.size()==2);
    };
    options.runtime_image_requirements_for_testing=[](std::string_view name,vk::Extent2D,ImageMemoryRequirements r){
        if(name=="a1" || name=="a2") r.memory_type_bits=3;
        if(name=="b1" || name=="b2") r.memory_type_bits=1;
        return r;
    };
    REQUIRE_NOTHROW(registerRenderingPassConfigFromJsonData(root.dump(),{64,64},registrationDependencies(std::move(options))));
    auto &registry=GET_MODULE(RenderTargetContainer);
    const auto a=registry.getRenderTargetIdByName("a1"), b=registry.getRenderTargetIdByName("b1");
    CHECK(registry.sharesAllocation(a,registry.getRenderTargetIdByName("a2")));
    CHECK(registry.sharesAllocation(b,registry.getRenderTargetIdByName("b2")));
    CHECK_FALSE(registry.sharesAllocation(a,b));
    CHECK(registry.getImage(a).memory_offset==registry.getImage(registry.getRenderTargetIdByName("a2")).memory_offset);
    CHECK(registry.getImage(b).memory_offset==registry.getImage(registry.getRenderTargetIdByName("b2")).memory_offset);
}

namespace {
// RC-7b HEAD 9cd7bb971, example, flat, 1920x1080. This inventory is
// independent of the generation being measured (including non-region images).
std::vector<RenderTargetDefinition> rc7bExampleReference() {
    std::vector<RenderTargetDefinition> result;
    const auto sampled=ImageUsageBit::color_attachment|ImageUsageBit::sampled;
    const auto add=[&](std::string name,ImageFormat format,ResolvedResourceExtent extent,ImageUsage usage) {
        RenderTargetDefinition d; d.name=std::move(name); d.format=format;
        d.fixed_extent=extent; d.usage=usage; d.mip_levels.count=1; d.array_layers=1;
        result.push_back(std::move(d));
    };
    const ResolvedResourceExtent full{1920,1080};
    add("gbuffer_albedo",ImageFormat::b8g8r8a8_srgb,full,sampled);
    add("gbuffer_normal",ImageFormat::r16g16b16a16_sfloat,full,sampled);
    add("gbuffer_material",ImageFormat::r8g8b8a8_unorm,full,sampled);
    add("g_emissive",ImageFormat::b8g8r8a8_srgb,full,sampled);
    add("gbuffer_worldpos",ImageFormat::r16g16b16a16_sfloat,full,sampled);
    add("offscreen_depth",ImageFormat::d32_sfloat,full,ImageUsageBit::depth_stencil_attachment|ImageUsageBit::transfer_src);
    add("lit_color",ImageFormat::r16g16b16a16_sfloat,full,sampled|ImageUsageBit::transfer_src);
    add("opaque_color",ImageFormat::r16g16b16a16_sfloat,full,ImageUsageBit::transfer_dst|ImageUsageBit::sampled);
    add("opaque_depth",ImageFormat::d32_sfloat,full,ImageUsageBit::transfer_dst|ImageUsageBit::sampled);
    add("ssao_output",ImageFormat::r8_unorm,full,sampled);
    add("ssao_blur",ImageFormat::r8_unorm,full,sampled);
    add("display",ImageFormat::b8g8r8a8_srgb,full,sampled|ImageUsageBit::transfer_src);
    add("Bloom_Threshold_RT",ImageFormat::b8g8r8a8_srgb,full,sampled);
    const std::array<ResolvedResourceExtent,4> levels{{{960,540},{480,270},{240,135},{120,67}}};
    for(std::size_t i=0;i<levels.size();++i) {
        add("Bloom_Downsample_H_"+std::to_string(i)+"_RT",ImageFormat::b8g8r8a8_srgb,levels[i],sampled);
        add("Bloom_Upsample_V_"+std::to_string(i)+"_RT",ImageFormat::b8g8r8a8_srgb,levels[i],sampled);
    }
    return result;
}

Json rc7cImageInventory(const std::vector<RenderTargetDefinition> &definitions,std::uint32_t queue) {
    Json result=Json::object();
    for(const auto &d:definitions) {
        const auto ci=renderTargetImageContract(d,{1920,1080},queue).image;
        result[d.name]={{"format",static_cast<int>(ci.format)},{"extent",{ci.extent.width,ci.extent.height,ci.extent.depth}},
            {"mips",ci.mipLevels},{"layers",ci.arrayLayers},{"usage",static_cast<VkImageUsageFlags>(ci.usage)},
            {"samples",static_cast<int>(ci.samples)},{"dimension",static_cast<int>(ci.imageType)}};
    }
    return result;
}

bool matchesRc7bExample(const std::vector<RenderTargetDefinition> &definitions,std::uint32_t queue) {
    return rc7cImageInventory(definitions,queue)==rc7cImageInventory(rc7bExampleReference(),queue);
}

struct Rc7cValidationCapture {
    vk::Instance instance;
    VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy=nullptr;
    std::vector<std::string> messages;
    explicit Rc7cValidationCapture(VulkanManageCore &core):instance(core.getInstance()) {
        const auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
        destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pUserData=this;
        info.pfnUserCallback=[](VkDebugUtilsMessageSeverityFlagBitsEXT,VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT *d,void *p)->VkBool32 {
            static_cast<Rc7cValidationCapture*>(p)->messages.emplace_back(d->pMessage); return VK_FALSE;
        };
        REQUIRE(create); REQUIRE(destroy);
        REQUIRE(create(instance,&info,nullptr,&messenger)==VK_SUCCESS);
    }
    ~Rc7cValidationCapture(){destroy(instance,messenger,nullptr);}
};
}

TEST_CASE("RC-7c C1 example actual VMA allocations and bytes beat RC-7b reference", "[rc7c][gpu][runtime][memory]") {
    setupLogger(); FastModuleContainer modules; Project project;
    startHeadless(project,{1920,1080});
    auto &core=GET_MODULE(VulkanManageCore);
    std::ofstream{project.root/"lighting.json"}<<project.paths.loadText("engine://render_fragments/deferred_lighting.json");
    const auto root=Json::parse(std::ifstream{std::filesystem::path{__FILE__}.parent_path().parent_path()/"projects/example/passes/main_rendering_config.json"});
    const GraphTransformRegistrySnapshot transforms;
    const RenderStrategyRegistrySnapshot strategies;
    const SubgraphReplacementRegistrySnapshot replacements;
    const VulkanRenderCompilerBackendContext backend{vk::Format::eB8G8R8A8Srgb,{1920,1080},core.getPhysDevice(),{},core.getRuntimeCapabilities(),
        VulkanRenderCompilerDevicePlanningMode::device_required,core.getDevice(),core.getGraphicsQueueFamilyIndex()};
    const std::vector<RenderCompilerProgramVariantRequest> variants{{.graph_variant=RenderPipelineGraphVariant::flat}};
    const auto compiled=runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(),{
        .rendering_config=root,.path_resolver=project.paths,.runtime_shader_compiler_enabled=true,
        .graph_transforms=transforms,.render_strategies=strategies,.subgraph_replacements=replacements,.backend_context=backend,.variants=variants});
    const auto &physical=requireVulkanRenderCompilerPhysicalPackage(*compiled.variants.front().physical_package);
    const auto &plan=*physical.target_plan_compilation.plans.front();
    // RC-7e E3: the example declares no target its graphs leave unused.
    CHECK(unallocatedRenderTargets(compiled.generation_resources,vulkanRuntimeVariantTargetPlans(compiled.variants),
        engineHeldRenderTargetNames()).empty());
    auto contract=std::make_shared<RuntimeTargetMemoryContract>(); contract->inputs=plan.memory_packing;
    std::vector<RenderTargetDefinition> definitions;
    for(const auto &image:compiled.generation_resources.images) definitions.push_back(renderTargetDefinitionOf(image));
    const auto reference=rc7bExampleReference();
    REQUIRE(definitions.size()==reference.size());
    REQUIRE(matchesRc7bExample(definitions,core.getGraphicsQueueFamilyIndex()));
    // A missing non-region image must fail before allocation-count/byte comparisons.
    auto missing=definitions;
    std::erase_if(missing,[](const auto &d){return d.name=="opaque_depth";});
    CHECK_FALSE(matchesRc7bExample(missing,core.getGraphicsQueueFamilyIndex()));
    Json rows=Json::array();
    std::uint64_t reference_bytes=0, payload_bytes=0, threshold_size=0, emissive_size=0;
    std::size_t reference_count=0;
    for(auto d:reference) {
        d.alias_group.reset(); d.planned_memory_region.reset();
        if(d.name=="g_emissive" || d.name=="Bloom_Threshold_RT") d.alias_group="rc7b_reference";
        REQUIRE(d.samples==1); REQUIRE_FALSE(d.history);
        const auto ci=renderTargetImageContract(d,{1920,1080},core.getGraphicsQueueFamilyIndex());
        const auto standalone=core.createUnboundImage(ci.createInfo());
        const auto req=core.imageRequirements(standalone);
        reference_bytes+=req.size; ++reference_count;
        const ResolvedResourceExtent extent{standalone.extent.width,standalone.extent.height};
        const auto payload=imageResourceBytes({d.format,extent,standalone.mip_levels,d.array_layers,d.samples,d.history?2u:1u});
        payload_bytes+=payload;
        if(d.name=="Bloom_Threshold_RT") threshold_size=req.size;
        if(d.name=="g_emissive") emissive_size=req.size;
        const auto *image=compiled.generation_resources.findImage(d.name); REQUIRE(image);
        rows.push_back({{"resource",d.name},{"extent",{extent.width,extent.height}},{"payload",payload},
            {"standalone_requirement",req.size},{"alignment",req.alignment},{"padding",req.size-payload},
            {"planned_region",image->memory_region?Json(*image->memory_region):Json(nullptr)},{"planned_offset",image->memory_offset}});
    }
    REQUIRE(threshold_size>0); REQUIRE(emissive_size==threshold_size);
    reference_bytes-=threshold_size; --reference_count;
    auto &targets=GET_MODULE(RenderTargetContainer);
    targets.registerMemoryTargets(definitions,{1920,1080},contract);
    std::vector<GlobalRenderTargetId> ids;
    for(const auto &d:definitions) ids.push_back(targets.getRenderTargetIdByName(d.name));
    const auto actual=targets.actualPlacement(ids);
    for(const auto &d:definitions) {
        const auto found=plan.memory_requirements.find(d.name);
        if(found!=plan.memory_requirements.end() && found->second)
            CHECK(core.imageRequirements(targets.getImage(targets.getRenderTargetIdByName(d.name)))==*found->second);
    }
    CHECK(actual.at("allocation_count").get<std::size_t>()<reference_count);
    CHECK(actual.at("allocation_bytes").get<std::uint64_t>()<reference_bytes);
    CHECK(actual.at("allocation_count").get<std::size_t>()<=4);
    CHECK(actual.at("allocation_bytes").get<std::uint64_t>()<=128286720);
    std::ofstream{"<作業ディレクトリ>/rc7d_d4_example.json"}<<Json{{"rc7c_count",4},{"rc7c_bytes",128286720},{"actual",actual},
        {"resources",rows},{"plan",vulkanTargetPlanToJson(plan)}}.dump(2);
    std::ofstream{"<作業ディレクトリ>/rc7c3_c1_measurements.json"}<<Json{{"reference_count",reference_count},{"reference_bytes",reference_bytes},
        {"payload_bytes",payload_bytes},{"actual",actual},{"resources",rows},{"device",std::string{core.getPhysDevice().getProperties().deviceName.data()}},
        {"driver_version",core.getPhysDevice().getProperties().driverVersion},{"plan",vulkanTargetPlanToJson(plan)}}.dump(2);
}

TEST_CASE("RC-7c C3 real ranges preserve disjoint images and synchronize partial overlaps", "[rc7c][gpu][runtime][ranges]") {
    setupLogger(); FastModuleContainer modules; Project project;
    GET_MODULE(EngineLaunchConfig).vulkan_validation=VulkanValidationMode::enabled;
    GET_MODULE(EngineLaunchConfig).gpu_labels=true;
    startHeadless(project,{64,64});
    auto &core=GET_MODULE(VulkanManageCore); Rc7cValidationCapture validation{core};
    REQUIRE(core.getVulkanValidationStatus().synchronization_validation);
    auto &targets=GET_MODULE(RenderTargetContainer); auto &utils=GET_MODULE(VulkanUtils);
    std::vector<RenderTargetDefinition> definitions;
    for(const auto &[name,extent]:std::vector<std::pair<std::string,ResolvedResourceExtent>>{{"range_A",{256,128}},{"range_B",{128,256}},{"range_C",{256,128}},{"range_D",{128,128}}}) {
        RenderTargetDefinition d; d.name=name; d.fixed_extent=extent;
        d.usage=ImageUsageBit::transfer_dst|ImageUsageBit::transfer_src|ImageUsageBit::sampled|ImageUsageBit::color_attachment;
        definitions.push_back(d);
    }
    auto contract=std::make_shared<RuntimeTargetMemoryContract>();
    contract->inputs.resources={{"range_A"},{"range_B"},{"range_C"},{"range_D"}};
    contract->inputs.order={"range_A","range_B","range_D","range_C"};
    contract->inputs.forbidden_pairs={{"range_A","range_B"},{"range_C","range_D"}};
    targets.registerMemoryTargets(definitions,{64,64},contract);
    const auto a=targets.getRenderTargetIdByName("range_A"), b=targets.getRenderTargetIdByName("range_B"), c=targets.getRenderTargetIdByName("range_C");
    const auto &ia=targets.getImage(a), &ib=targets.getImage(b), &ic=targets.getImage(c);
    REQUIRE(targets.sharesAllocation(a,b)); REQUIRE(targets.sharesAllocation(a,c)); REQUIRE(ia.image.get()!=ib.image.get());
    CHECK(ib.memory_offset>=ia.memory_size); CHECK(ic.memory_offset>ia.memory_offset);
    CHECK(ic.memory_offset<ia.memory_size); CHECK(ic.memory_offset+ic.memory_size>ib.memory_offset);
    RenderTargetLayoutTracker tracker; std::vector<RenderTargetLayoutTracker::AliasTransition> trace;
    tracker.alias_transition_observer=[&](const auto &e){trace.push_back(e);};
    const std::uint64_t image_bytes=256*128*4;
    auto staging=core.allocBuf(image_bytes*6,vk::BufferUsageFlagBits::eTransferDst,vma::MemoryUsage::eAutoPreferHost,vma::AllocationCreateFlagBits::eHostAccessRandom);
    const auto write=[&](vk::CommandBuffer cmd,GlobalRenderTargetId id,std::array<float,4> color) {
        tracker.transition(cmd,targets,utils,id,vk::ImageLayout::eTransferDstOptimal);
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor,0,1,0,1};
        cmd.clearColorImage(targets.getImage(id).image.get(),vk::ImageLayout::eTransferDstOptimal,vk::ClearColorValue{color},{range});
    };
    const auto copy=[&](vk::CommandBuffer cmd,GlobalRenderTargetId id,std::uint64_t slot) {
        tracker.transition(cmd,targets,utils,id,vk::ImageLayout::eTransferSrcOptimal);
        vk::BufferImageCopy region; region.bufferOffset=slot*image_bytes;
        region.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1}; region.imageExtent=targets.getImage(id).extent;
        cmd.copyImageToBuffer(targets.getImage(id).image.get(),vk::ImageLayout::eTransferSrcOptimal,staging.buffer.get(),{region});
    };
    for(int frame=0;frame<3;++frame) {
        tracker.beginFrame(); trace.clear();
        utils.executeOneTimeCmd([&](vk::CommandBuffer cmd){
            write(cmd,a,{1,0,0,1}); write(cmd,b,{0,1,0,1});
            CHECK(tracker.currentLayout(a)==vk::ImageLayout::eTransferDstOptimal);
            copy(cmd,a,0); copy(cmd,b,1);
            write(cmd,c,{0,0,1,1}); copy(cmd,c,2);
            write(cmd,a,{1,0,0,1}); write(cmd,b,{0,1,0,1}); copy(cmd,a,3); copy(cmd,b,4);
            write(cmd,c,{0,0,1,1}); copy(cmd,c,5);
            vk::MemoryBarrier host{vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eHostRead};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{}, {host},{},{});
        },true);
        CHECK(trace.front().dependency);
        REQUIRE(trace.size()==12);
        for(const auto index:{6u,7u,10u}) {
            const auto &event=trace[index];
            CHECK(event.dependency);
            CHECK(event.old_layout==vk::ImageLayout::eUndefined);
            CHECK(event.src_stage==vk::PipelineStageFlags{vk::PipelineStageFlagBits::eAllCommands});
            CHECK(event.dst_stage==event.src_stage);
            CHECK(event.src_access==(vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite));
            CHECK(event.dst_access==event.src_access);
        }
        const auto bytes=core.readBuf(staging,image_bytes*6);
        for(std::size_t slot=0;slot<6;++slot) for(std::size_t i=0;i<image_bytes;i+=4) {
            const std::size_t base=slot*image_bytes+i;
            REQUIRE(bytes[base+(slot%3)]==255);
            REQUIRE(bytes[base+((slot+1)%3)]==0);
            REQUIRE(bytes[base+((slot+2)%3)]==0); REQUIRE(bytes[base+3]==255);
        }
        // A frame whose previous overlapping occupant does not run.
        tracker.beginFrame();
        utils.executeOneTimeCmd([&](vk::CommandBuffer cmd){write(cmd,a,{1,0,0,1});},true);
    }
    // Binding guards reject an out-of-range, misaligned or unsupported type before Vulkan.
    const auto ci=renderTargetImageContract(definitions[0],{64,64},core.getGraphicsQueueFamilyIndex());
    auto unbound=core.createUnboundImage(ci.createInfo()); const auto req=core.imageRequirements(unbound);
    CHECK_THROWS_WITH(core.bindImageRegion(unbound,ia.allocation,core.imageAllocationBytes(ia),req,"range_bad_bounds"),Catch::Matchers::ContainsSubstring("memory_bind_invalid@1: range_bad_bounds"));
    CHECK_THROWS_WITH(core.bindImageRegion(unbound,ia.allocation,1,req,"range_bad_alignment"),Catch::Matchers::ContainsSubstring("memory_bind_invalid@1: range_bad_alignment"));
    auto wrong=req; wrong.memory_type_bits=0;
    CHECK_THROWS_WITH(core.bindImageRegion(unbound,ia.allocation,0,wrong,"range_bad_type"),Catch::Matchers::ContainsSubstring("memory_bind_invalid@1: range_bad_type"));
    // Two real submissions remain blocked on a host timeline gate. Resetting
    // the tracker between them models a generation/layout reset on live images.
    // The frame-first rule must still cover the preceding queue submission.
    {
        auto commands=core.allocCmdBufs(2);
        vk::SemaphoreTypeCreateInfo type;type.semaphoreType=vk::SemaphoreType::eTimeline;
        vk::SemaphoreCreateInfo create;create.pNext=&type;
        auto gate=core.getDevice().createSemaphoreUnique(create);
        const auto signal=[&]{vk::SemaphoreSignalInfo info;info.semaphore=gate.get();info.value=1;core.getDevice().signalSemaphore(info);};
        struct Release {std::function<void()> signal;bool done=false;~Release(){if(!done)signal();}} release{signal};
        std::vector<vk::Fence> fences;
        const auto leases=targets.resourceLeases();
        for(auto &command:commands) {
            tracker.reset();tracker.beginFrame();command.recordBegin();
            write(*command,a,{1,0,0,1});write(*command,b,{0,1,0,1});
            write(*command,c,{0,0,1,1});write(*command,a,{1,0,0,1});write(*command,b,{0,1,0,1});
            command.recordEnd();
            const auto cmd=*command;const auto semaphore=gate.get();
            const vk::PipelineStageFlags stage=vk::PipelineStageFlagBits::eAllCommands;
            const std::uint64_t value=1;vk::TimelineSemaphoreSubmitInfo timeline;
            timeline.waitSemaphoreValueCount=1;timeline.pWaitSemaphoreValues=&value;
            vk::SubmitInfo submit;submit.pNext=&timeline;submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&semaphore;
            submit.pWaitDstStageMask=&stage;submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
            core.getDevice().resetFences({command.getFence()});
            core.getGraphicsQueue().submit(submit,command.getFence());fences.push_back(command.getFence());
        }
        for(const auto fence:fences) CHECK(core.getDevice().getFenceStatus(fence)==vk::Result::eNotReady);
        signal();release.done=true;
        REQUIRE(core.getDevice().waitForFences(fences,VK_TRUE,UINT64_MAX)==vk::Result::eSuccess);
    }
    for(const auto &message:validation.messages){INFO(message);CHECK(message.empty());}
    auto observation=targets.actualPlacement(std::array{a,b,c});
    observation["validation_message_count"]=validation.messages.size();observation["pending_fences_tested"]=2;
    std::ofstream{"<作業ディレクトリ>/rc7c3_c3_ranges.json"}<<observation.dump(2);
}

TEST_CASE("RC-7c runtime resize rebuilds dedicated keys and rejects required repack atomically", "[rc7c][gpu][runtime][resize]") {
    setupLogger(); FastModuleContainer modules; Project project; startHeadless(project,{64,64});
    auto &targets=GET_MODULE(RenderTargetContainer);
    RenderTargetDefinition a; a.name="repack_A"; a.usage=ImageUsageBit::color_attachment|ImageUsageBit::sampled;
    auto b=a; b.name="repack_B";
    auto contract=std::make_shared<RuntimeTargetMemoryContract>();
    contract->inputs.resources={{a.name},{b.name}}; contract->inputs.order={a.name,b.name}; contract->inputs.same_offset_sets={{a.name,b.name}};
    contract->requirements_for_testing=[](std::string_view name,vk::Extent2D extent,ImageMemoryRequirements r){if(name=="repack_B" && extent.width>64)r.requires_dedicated=true;return r;};
    targets.registerMemoryTargets({a,b},{64,64},contract);
    const auto aid=targets.getRenderTargetIdByName(a.name), bid=targets.getRenderTargetIdByName(b.name);
    REQUIRE(targets.sharesAllocation(aid,bid));
    const auto old=targets.getImage(aid).image.get();
    const auto leases=targets.resourceLeases();
    const auto pending=GET_MODULE(DeletionQueue).pendingCountForTesting();
    auto prepared=targets.prepareForExtent({96,64}); targets.publishPreparedExtent(std::move(prepared));
    CHECK_FALSE(targets.sharesAllocation(aid,bid)); CHECK(targets.getImage(aid).image.get()!=old);
    CHECK(GET_MODULE(DeletionQueue).pendingCountForTesting()>pending);
    contract->requirements_for_testing=[](std::string_view name,vk::Extent2D,ImageMemoryRequirements r){
        if(name=="repack_B" && (r.memory_type_bits & (r.memory_type_bits-1))) r.memory_type_bits &= (~r.memory_type_bits+1u);
        return r;
    };
    auto changed_key=targets.prepareForExtent({128,64});targets.publishPreparedExtent(std::move(changed_key));
    const auto actual_bits=GET_MODULE(VulkanManageCore).imageRequirements(targets.getImage(aid)).memory_type_bits;
    if(actual_bits & (actual_bits-1)) {
        CHECK_FALSE(targets.sharesAllocation(aid,bid));CHECK(targets.getImage(bid).region_bound);
    }
    // A second generation keeps a required slot immutable from registration.
    auto required=std::make_shared<RuntimeTargetMemoryContract>();
    auto required_a=a, required_b=b; required_a.name="required_A"; required_b.name="required_B";
    required->inputs.resources={{required_a.name},{required_b.name}};
    required->inputs.order={required_a.name,required_b.name};
    required->inputs.same_offset_sets={{required_a.name,required_b.name}};
    required->required_sets=required->inputs.same_offset_sets;
    required->requirements_for_testing=[](std::string_view name,vk::Extent2D extent,ImageMemoryRequirements r){if(name=="required_B" && extent.width>64)r.requires_dedicated=true;return r;};
    targets.registerMemoryTargets({required_a,required_b},{64,64},required);
    const auto required_aid=targets.getRenderTargetIdByName(required_a.name);
    const auto required_bid=targets.getRenderTargetIdByName(required_b.name);
    REQUIRE(targets.sharesAllocation(required_aid,required_bid));
    const auto stable=targets.getImage(required_aid).image.get();
    CHECK_THROWS_WITH(targets.prepareForExtent({128,64}),Catch::Matchers::ContainsSubstring("alias_repack_unsatisfiable@1: required_B"));
    CHECK(targets.getImage(required_aid).image.get()==stable);
    const auto checkpoint=targets.checkpointRegistrations();
    targets.hideRegistrationName(required_a.name,required_aid); targets.hideRegistrationName(required_b.name,required_bid);
    CHECK_THROWS_WITH(targets.registerMemoryTargets({required_a,required_b},{128,64},required),Catch::Matchers::ContainsSubstring("alias_repack_unsatisfiable@1: required_B"));
    targets.rollbackRegistrations(checkpoint);
    CHECK(targets.getRenderTargetIdByName(required_a.name)==required_aid); CHECK(targets.getImage(required_aid).image.get()==stable);
}
} // namespace Pelican
