TEST_CASE("RC-7c fix3 storage compute is frame first after no selected raster view", "[rc7c][gpu][fix3]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger(); const auto directory=makeTempProjectDir();
    FastModuleContainer modules; RuntimeTeardownGuard teardown{RuntimeTeardownMode::terminal_shutdown};
    writeTextFile(directory/"scene.json",R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})");
    writeTextFile(directory/"assets.json",R"({"schema":"pelican.asset_data","version":2,"models":[]})");
    writeTextFile(directory/"shaders/alias_fullscreen.vert",gpuArenaFullscreenVertexShader());
    writeTextFile(directory/"shaders/alias_producer_a.frag",aliasProducerAFragmentShader());
    writeTextFile(directory/"shaders/alias_producer_b.frag",aliasProducerBFragmentShader());
    writeTextFile(directory/"shaders/alias_copy.frag",gpuArenaCopyFragmentShader());
    writeTextFile(directory/"shaders/frame_first.comp",R"(#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1) in;
void main(){ pelican_store_target(ivec2(gl_GlobalInvocationID.xy),vec4(1,0,0,1)); }
)");
    auto config=aliasLifetimeRenderingConfig(); config["rendering_passes"][0]["name"]="main_render";
    config["render_targets"][0]["usage"].push_back("STORAGE");
    config["render_targets"][0]["usage"].push_back("TRANSFER_SRC");
    config["compute_tasks"]=nlohmann::json::array({{{"name","frame_first"},{"shader","shaders/frame_first"},
        {"writes",{"alias_a"}},{"after",{"alias_consume_b"}},
        {"resource_ports",{{"target",{{"resource","alias_a"},{"access","storage"}}}}},
        {"dispatch",{{"groups",{32,32,1}}}},{"schedule","per_frame"}}});
    writeTextFile(directory/"hybrid.json",TestSupport::renderGraphRootWithTaskFeature(config,directory).dump(2));
    configureRayQueryTestRuntime(directory);
    GET_MODULE(EngineLaunchConfig).vulkan_validation=VulkanValidationMode::enabled;
    GET_MODULE(EngineLaunchConfig).gpu_labels=true;
    auto &renderer=GET_MODULE(Renderer); auto &core=GET_MODULE(VulkanManageCore);
    Rc7bValidationCapture validation{core};
    REQUIRE(core.getVulkanValidationStatus().synchronization_validation);
    auto &targets=GET_MODULE(RenderTargetContainer); auto &utils=GET_MODULE(VulkanUtils);
    auto &tasks=GET_MODULE(ComputeTaskContainer); auto &tracker=RendererTestAccess::imageLayoutTracker(renderer);
    const auto a=targets.getRenderTargetIdByName("alias_a"); REQUIRE(targets.getImage(a).region_bound);
    renderer.render(); core.waitIdle();
    REQUIRE(tracker.currentLayout(a,false,&targets)==vk::ImageLayout::eGeneral);
    const auto program=GET_MODULE(FrameGraphRuntimeContainer).findProgram(
        GET_MODULE(RenderingPassContainer).getRenderingPassIdByName("main_render")); REQUIRE(program);
    auto frame=program->frame_graph; auto plan=std::make_shared<VulkanTargetPlan>(*frame.target_plan);
    for(auto &node:frame.nodes) if(node.name!="frame_first") {node.view_family="$capture/cube";node.view_select={{"face",1}};}
    for(auto &scope:plan->scopes) if(scope.kind!=VulkanPhysicalScopeKind::compute) scope.view_select={{"face",1}};
    frame.target_plan=plan;
    const std::vector<RenderViewAxisCoordinates> axes{{{"face",0}}};
    const std::array families{LogicalFrameViewFamilyCardinality{mainRenderViewFamilyId,1},LogicalFrameViewFamilyCardinality{"$capture/cube",1,axes}};
    const auto schedule=buildLogicalFrameViewFamilySchedule(frame,families);
    REQUIRE(std::any_of(schedule.begin(),schedule.end(),[](const auto &i){return i.no_selected_view;}));
    const auto task=tasks.getComputeTaskIdByName("frame_first");
    FrameResources resources; resources.selectView(0,0);
    auto staging=core.allocBuf(32*32*4,vk::BufferUsageFlagBits::eTransferDst,vma::MemoryUsage::eAutoPreferHost,vma::AllocationCreateFlagBits::eHostAccessRandom);
    std::vector<RenderTargetLayoutTracker::AliasTransition> trace;
    tracker.alias_transition_observer=[&](const auto &e){trace.push_back(e);};
    tracker.beginFrame();
    utils.executeOneTimeCmd([&](vk::CommandBuffer cmd){
        // The raster invocations do not record. The normal compute resource
        // preparation sees GENERAL from the preceding frame.
        std::size_t skipped=0,dispatched=0;
        for(const auto &i:schedule) {
            if(i.no_selected_view) {++skipped;continue;}
            REQUIRE(frame.nodes.at(i.node_index).name=="frame_first");
            tasks.transitionResourcesForDispatch(cmd,task,targets,utils,tracker);
            tasks.dispatch(cmd,task,resources); ++dispatched;
        }
        CHECK(skipped>0); REQUIRE(dispatched==1);
        REQUIRE(trace.size()==1); const auto &first=trace.front();
        CHECK(first.resource==a); CHECK(first.dependency);
        CHECK(first.old_layout==vk::ImageLayout::eUndefined); CHECK(first.new_layout==vk::ImageLayout::eGeneral);
        CHECK(first.src_stage==vk::PipelineStageFlags{vk::PipelineStageFlagBits::eAllCommands}); CHECK(first.dst_stage==first.src_stage);
        CHECK(first.src_access==(vk::AccessFlagBits::eMemoryRead|vk::AccessFlagBits::eMemoryWrite)); CHECK(first.dst_access==first.src_access);
        // A second same-layout access is recorded without another frame-first dependency.
        tasks.transitionResourcesForDispatch(cmd,task,targets,utils,tracker);
        REQUIRE(trace.size()==2); CHECK_FALSE(trace.back().dependency);
        tracker.transition(cmd,targets,utils,a,vk::ImageLayout::eTransferSrcOptimal);
        vk::BufferImageCopy copy;copy.imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1};copy.imageExtent=vk::Extent3D{32,32,1};
        cmd.copyImageToBuffer(targets.getImage(a).image.get(),vk::ImageLayout::eTransferSrcOptimal,staging.buffer.get(),{copy});
        vk::MemoryBarrier host{vk::AccessFlagBits::eTransferWrite,vk::AccessFlagBits::eHostRead};
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,vk::PipelineStageFlagBits::eHost,{}, {host},{},{});
    },true);
    const auto pixels=core.readBuf(staging,32*32*4);
    CHECK(tracker.frameAliasDependencyCount()==1);
    CHECK_FALSE(renderer.currentFramePlanJson().at("memory_summary").at("actual_placement").contains("alias_barrier_count"));
    for(std::size_t i=0;i<pixels.size();i+=4) {REQUIRE(pixels[i]==255);REQUIRE(pixels[i+1]==0);REQUIRE(pixels[i+2]==0);REQUIRE(pixels[i+3]==255);}
    tracker.alias_transition_observer={};
    for(const auto &[phase,message]:validation.messages){INFO(message);CHECK(message.empty());}
    std::filesystem::remove_all(directory);
#endif
}

TEST_CASE("RC-7c C3 renderer ranges survive pending submits both resize paths replacement and rollback", "[rc7c][gpu][headless][runtime]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    setupLogger(); const auto directory=makeTempProjectDir();
    FastModuleContainer modules; RuntimeTeardownGuard teardown{RuntimeTeardownMode::terminal_shutdown};
    writeTextFile(directory/"scene.json",R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})");
    writeTextFile(directory/"assets.json",R"({"schema":"pelican.asset_data","version":2,"models":[]})");
    writeTextFile(directory/"shaders/alias_fullscreen.vert",gpuArenaFullscreenVertexShader());
    writeTextFile(directory/"shaders/alias_producer_a.frag",aliasProducerAFragmentShader());
    writeTextFile(directory/"shaders/alias_producer_b.frag",aliasProducerBFragmentShader());
    writeTextFile(directory/"shaders/alias_copy.frag",gpuArenaCopyFragmentShader());
    auto config=aliasLifetimeRenderingConfig(); config["rendering_passes"][0]["name"]="main_render";
    config["render_targets"][0]["width"]=32; config["render_targets"][0]["height"]=16; config["render_targets"][0]["extent_scale"]=1.0;
    config["render_targets"][1]["extent_scale"]=0.5;
    auto c=config["render_targets"][1]; c["name"]="alias_c";c["extent_scale"]=1.0;config["render_targets"].push_back(c);
    auto sink=config["render_targets"][2];sink["name"]="alias_sink_b";config["render_targets"].push_back(sink);
    auto &passes=config["rendering_passes"][0]["passes"];
    passes[2]["after"]={"alias_produce_a"}; passes[1]["after"]={"alias_produce_b"}; passes[3]["after"]={"alias_consume_a"};
    passes[3]["output"]["color"]="alias_sink_b";
    auto produce_c=passes[2]; produce_c["name"]="alias_produce_c"; produce_c["after"]={"alias_consume_b"}; produce_c["output"]["color"]="alias_c";
    auto consume_c=passes[3];consume_c["name"]="alias_consume_c";consume_c["after"]={"alias_produce_c"};consume_c["input"]={"alias_c"};
    consume_c["input_footprints"]={{"alias_c","arbitrary"}};consume_c["output"]["color"]="swapchain";
    passes.push_back(produce_c); passes.push_back(consume_c);
    auto root=TestSupport::renderGraphRoot(config);
    TestSupport::retainObserved(root,"main_render",{"alias_sink","alias_sink_b"});
    writeTextFile(directory/"hybrid.json",root.dump(2)); configureRayQueryTestRuntime(directory);
    auto &launch=GET_MODULE(EngineLaunchConfig);launch.vulkan_validation=VulkanValidationMode::enabled;launch.gpu_labels=true;
    auto &core=GET_MODULE(VulkanManageCore); Rc7bValidationCapture validation{core};
    auto &renderer=GET_MODULE(Renderer);auto &targets=GET_MODULE(RenderTargetContainer);auto &output=GET_MODULE(RenderTarget);
    auto &tracker=RendererTestAccess::imageLayoutTracker(renderer);
    std::vector<RenderTargetLayoutTracker::AliasTransition> trace;
    tracker.alias_transition_observer=[&](const auto &e){trace.push_back(e);};
    const auto ids=[&]{return std::array{targets.getRenderTargetIdByName("alias_a"),targets.getRenderTargetIdByName("alias_b"),targets.getRenderTargetIdByName("alias_c")};};
    const auto placement=[&]{const auto value=ids();const auto &a=targets.getImage(value[0]),&b=targets.getImage(value[1]),&c=targets.getImage(value[2]);
        REQUIRE(a.allocation==b.allocation);REQUIRE(a.allocation==c.allocation);REQUIRE(a.image.get()!=b.image.get());
        CHECK(a.memory_offset!=b.memory_offset);REQUIRE(a.extent!=b.extent);
        CHECK((a.memory_offset+a.memory_size<=b.memory_offset || b.memory_offset+b.memory_size<=a.memory_offset));
        const auto frame_plan=renderer.currentFramePlanJson();
        for(const auto &r:frame_plan.at("physical_target_plan").at("resources")) if(r.at("logical_resource")=="alias_a") {
            CHECK(r.at("bytes").get<std::uint64_t>()==core.imageAllocationBytes(a));
            CHECK(r.at("payload_bytes").get<std::uint64_t>()==32u*16u*4u);
        }
        const auto summary=frame_plan.at("memory_summary");
        const auto actual=summary.at("actual_placement");REQUIRE_FALSE(actual.is_null());
        REQUIRE(actual.at("allocation_count").get<std::size_t>()>0);
        std::uint64_t allocation_bytes=0;
        for(const auto &region:actual.at("regions")) {
            allocation_bytes+=region.at("size").get<std::uint64_t>();
            const auto type=region.at("memoryTypeIndex").get<std::uint32_t>();REQUIRE(type<32);
            for(const auto &r:region.at("residents")) {
                const auto offset=r.at("offset").get<std::uint64_t>(),size=r.at("size").get<std::uint64_t>();
                CHECK(offset+size<=region.at("size").get<std::uint64_t>());
                CHECK(offset%r.at("alignment").get<std::uint64_t>()==0);
                CHECK((r.at("memory_type_bits").get<std::uint32_t>()&(1u<<type))!=0);
            }
        }
        CHECK(summary.at("total_bytes").get<std::uint64_t>()==allocation_bytes);
        CHECK(summary.at("actual_allocation_bytes").get<std::uint64_t>()==allocation_bytes);
    };
    const auto observe=[&]{for(const auto &[name,channel]:std::array{std::pair{"alias_sink",0},std::pair{"alias_sink_b",1}}){
        const auto read=RendererTestAccess::readRenderTarget(renderer,name,{});REQUIRE_FALSE(read.bytes.empty());
        for(std::size_t i=0;i<read.bytes.size();i+=4){REQUIRE(read.bytes[i+channel]==255);REQUIRE(read.bytes[i+1-channel]==0);REQUIRE(read.bytes[i+2]==0);REQUIRE(read.bytes[i+3]==255);}
    }};
    for(int frame=0;frame<2;++frame){GET_MODULE(EngineTime).advance();renderer.render();placement();observe();}
    // A host-signaled timeline gate proves that both submit fences remain unsignaled
    // while the old images/heaps are resized or replaced.
    struct Gate {
        VulkanManageCore &core;vk::UniqueSemaphore semaphore;
        std::mutex mutex;std::condition_variable condition;bool signaled=false;
        std::atomic<bool> timed_out=false;std::thread watchdog;
        explicit Gate(VulkanManageCore &c):core(c){vk::SemaphoreTypeCreateInfo type;type.semaphoreType=vk::SemaphoreType::eTimeline;
            vk::SemaphoreCreateInfo info;info.pNext=&type;semaphore=core.getDevice().createSemaphoreUnique(info);
            watchdog=std::thread([this]{std::unique_lock lock{mutex};
                if(!condition.wait_for(lock,std::chrono::seconds{5},[this]{return signaled;})) {
                    timed_out=true; lock.unlock(); signal();
                }
            });
        }
        void signal(){std::lock_guard lock{mutex};if(!signaled){vk::SemaphoreSignalInfo info;info.semaphore=semaphore.get();info.value=1;core.getDevice().signalSemaphore(info);signaled=true;condition.notify_all();}}
        ~Gate(){signal();watchdog.join();core.waitIdle();CHECK_FALSE(timed_out.load());}
    };
    const auto pending=[&](Gate &gate){
        test_support::RenderTargetTestAccess::deferSubmissionWait(output,true,gate.semaphore.get());
        renderer.render();renderer.render();
        REQUIRE(test_support::RenderTargetTestAccess::outstandingSubmissions(output)==2);
        REQUIRE(test_support::RenderTargetTestAccess::unsignaledSubmissions(output)==2);
        REQUIRE(core.getDevice().getSemaphoreCounterValue(gate.semaphore.get())==0);
    };
    const auto finish=[&](Gate &gate){gate.signal();test_support::RenderTargetTestAccess::completeSubmissions(output);
        test_support::RenderTargetTestAccess::deferSubmissionWait(output,false);};
    {
        Gate first{core}, second{core};
        test_support::RenderTargetTestAccess::deferSubmissionWait(output,true,first.semaphore.get());renderer.render();
        test_support::RenderTargetTestAccess::deferSubmissionWait(output,true,second.semaphore.get());renderer.render();
        REQUIRE(test_support::RenderTargetTestAccess::unsignaledSubmissions(output)==2);
        const auto old_ids=ids();const auto old_allocation=std::weak_ptr{targets.getImage(old_ids[0]).allocation};
        const auto old_revision=targets.actualPlacement(old_ids).at("revision");
        auto old_output=test_support::RenderTargetTestAccess::resizeOffscreen(output,{48,32},false);
        first.signal(); // Reuse slot zero while the second old submit stays blocked.
        renderer.render(); // Executes the container-only resize, rebind, and submit.
        CHECK(core.getDevice().getSemaphoreCounterValue(second.semaphore.get())==0);
        CHECK(test_support::RenderTargetTestAccess::unsignaledSubmissions(output)==2);
        CHECK_FALSE(old_allocation.expired());
        CHECK(targets.getImage(ids()[1]).extent.width==24);
        const auto resized=targets.actualPlacement(ids());
        CHECK(resized.at("extent")==nlohmann::json{{"width",48},{"height",32}});
        CHECK(resized.at("revision")!=old_revision);
        finish(second);renderer.render();renderer.render();placement();observe();
    }
    {
        Gate gate{core}; pending(gate);
        const auto old=ids(); const auto old_heap=std::weak_ptr{targets.getImage(old[0]).allocation};
        const auto old_revision=targets.actualPlacement(old).at("revision");
        auto old_output=test_support::RenderTargetTestAccess::resizeOffscreen(output,{64,48},true);
        bool relowered=false;
        test_support::RenderTargetTestAccess::beforeNextBegin(output,[&]{
            CHECK(ids()!=old); CHECK_FALSE(old_heap.expired());
            const auto resized=targets.actualPlacement(ids());
            CHECK(resized.at("extent")==nlohmann::json{{"width",64},{"height",48}});
            CHECK(resized.at("revision")!=old_revision);
            CHECK(test_support::RenderTargetTestAccess::unsignaledSubmissions(output)==2);
            relowered=true; gate.signal();
        });
        renderer.render(); REQUIRE(relowered);
        finish(gate); renderer.render();renderer.render();placement();observe();
    }
    {
        Gate gate{core};pending(gate);
        const auto stable=GET_MODULE(FrameGraphRuntimeContainer).snapshot();const auto stable_ids=ids();
        const auto stable_placement=renderer.currentFramePlanJson().at("memory_summary").at("actual_placement");
        auto required=root;
        for(const auto *name:{"alias_a","alias_c"})
            required["rendering"]["target_planning"]["graphs"]["main_render"]["resources"][name]["alias_group"]="runtime_required";
        RenderingPassConfigRegistrationDependencies::Options options;
        options.runtime_image_requirements_for_testing=[](std::string_view name,vk::Extent2D,ImageMemoryRequirements r){if(name=="alias_c")r.requires_dedicated=true;return r;};
        REQUIRE_THROWS_WITH(registerRenderingPassConfigFromJsonData(required.dump(),{64,48},gpuArenaRegistrationDependencies(std::move(options))),
            Catch::Matchers::ContainsSubstring("render_target.alias_repack_unsatisfiable@1"));
        REQUIRE(GET_MODULE(FrameGraphRuntimeContainer).snapshot()==stable);REQUIRE(ids()==stable_ids);
        CHECK(renderer.currentFramePlanJson().at("memory_summary").at("actual_placement")==stable_placement);
        finish(gate);renderer.render();placement();observe();
    }
    {
        Gate gate{core};pending(gate);
        const auto stable=GET_MODULE(FrameGraphRuntimeContainer).snapshot();const auto stable_ids=ids();
        RenderingPassConfigRegistrationDependencies::Options options;options.fault_point=RenderPipelineGpuRegistrationFaultPoint::after_runtime_prepare;
        REQUIRE_THROWS_WITH(registerRenderingPassConfigFromJsonData(root.dump(),{64,48},gpuArenaRegistrationDependencies(std::move(options))),"Injected render pipeline GPU registration failure");
        REQUIRE(GET_MODULE(FrameGraphRuntimeContainer).snapshot()==stable);REQUIRE(ids()==stable_ids);
        finish(gate);renderer.render();placement();observe();
    }
    {
        Gate gate{core};pending(gate);const auto old=ids();const auto old_heap=std::weak_ptr{targets.getImage(old[0]).allocation};
        const auto replacement=registerRenderingPassConfigFromJsonData(root.dump(),{64,48},gpuArenaRegistrationDependencies({}));
        REQUIRE_FALSE(replacement.rendering_pass_ids.empty());
        RendererTestAccess::selectProgram(renderer,replacement.rendering_pass_ids.front());
        REQUIRE(ids()!=old);REQUIRE_FALSE(old_heap.expired());finish(gate);renderer.render();renderer.render();placement();observe();
    }
    tracker.alias_transition_observer={};
    for(const auto &[phase,message]:validation.messages){INFO(message);CHECK(message.empty());}
    std::ofstream{"<作業ディレクトリ>/rc7c3_c3_renderer.json"}<<nlohmann::json{{"memory_summary",renderer.currentFramePlanJson().at("memory_summary")},
        {"validation_message_count",validation.messages.size()},{"pending_fences_tested",2}}.dump(2);
    std::filesystem::remove_all(directory);
#else
    SKIP("runtime shader compiler unavailable");
#endif
}
