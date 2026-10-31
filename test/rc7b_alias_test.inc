namespace {
struct Rc7bValidationCapture {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy = nullptr;
    std::vector<std::pair<std::string, std::string>> messages;
    std::mutex mutex;
    std::string phase = "shared";
    explicit Rc7bValidationCapture(VulkanManageCore &core) : instance{core.getInstance()} {
        const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (!create || !destroy) throw std::runtime_error("RC-7b validation capture unavailable");
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pUserData = this;
        info.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                                 const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) -> VkBool32 {
            auto &self = *static_cast<Rc7bValidationCapture *>(user);
            const std::lock_guard lock{self.mutex};
            self.messages.emplace_back(self.phase, data->pMessage ? data->pMessage : "");
            return VK_FALSE;
        };
        if (create(instance, &info, nullptr, &messenger) != VK_SUCCESS) throw std::runtime_error("RC-7b validation messenger creation failed");
    }
    ~Rc7bValidationCapture() { if (messenger) destroy(instance, messenger, nullptr); }
};
}
TEST_CASE("RC-7b B1 B2 B3 B5 expanded image sharing survives frames and edits", "[rc7b][rc7b-gpu][gpu][headless]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    for (const std::string format : {"R8G8B8A8_UNORM", "B8G8R8A8_UNORM", "D32_SFLOAT"}) {
        CAPTURE(format);
        setupLogger();
        const auto directory = makeTempProjectDir();
        FastModuleContainer modules;
        RuntimeTeardownGuard teardown{RuntimeTeardownMode::terminal_shutdown};
        writeTextFile(directory / "scene.json", R"({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})");
        writeTextFile(directory / "assets.json", R"({"schema":"pelican.asset_data","version":2,"models":[]})");
        writeTextFile(directory / "shaders/alias_fullscreen.vert", gpuArenaFullscreenVertexShader());
        const bool depth = format == "D32_SFLOAT";
        writeTextFile(directory / "shaders/alias_producer_a.frag", depth ?
            "#version 450\nvoid main(){ gl_FragDepth=0.25; }" : aliasProducerAFragmentShader());
        writeTextFile(directory / "shaders/alias_producer_b.frag", depth ?
            "#version 450\nvoid main(){ gl_FragDepth=0.75; }" : aliasProducerBFragmentShader());
        writeTextFile(directory / "shaders/alias_copy.frag", depth ? R"(#version 450
layout(set=1,binding=0) uniform sampler2D inputTexture;
layout(location=0) in vec2 inUV;
layout(location=0) out vec4 outColor;
void main(){ float d=texture(inputTexture,inUV).r; outColor=d<0.5?vec4(1,0,0,1):vec4(0,1,0,1); }
)" : gpuArenaCopyFragmentShader());
        auto config = aliasLifetimeRenderingConfig();
        config["rendering_passes"][0]["name"] = "main_render";
        for (auto &target : config["render_targets"]) {
            if (target["name"] == "alias_sink") target["usage"].push_back("TRANSFER_SRC");
            else {
                target["format"] = format;
                target["usage"] = depth ? nlohmann::json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"}) :
                    nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"});
            }
        }
        auto c = config["render_targets"][0]; c["name"] = "alias_c";
        config["render_targets"].push_back(c);
        auto sink_b = config["render_targets"][2]; sink_b["name"] = "alias_sink_b";
        config["render_targets"].push_back(sink_b);
        auto &passes = config["rendering_passes"][0]["passes"];
        passes[3]["output"]["color"] = "alias_sink_b";
        auto produce_c = passes[0]; produce_c["name"] = "alias_produce_c"; produce_c["after"] = {"alias_consume_b"};
        produce_c["output"]["color"] = "alias_c";
        auto consume_c = passes[3]; consume_c["name"] = "alias_consume_c"; consume_c["input"] = {"alias_c"};
        consume_c["input_footprints"] = {{"alias_c", "arbitrary"}};
        consume_c["output"]["color"] = "swapchain";
        passes.push_back(produce_c); passes.push_back(consume_c);
        if (depth) for (auto &p : passes) if (p["name"].get<std::string>().starts_with("alias_produce")) {
            p["type"] = "raster"; p["draw"] = {{"vertex_count", 6}};
            p["output"]["depth"] = p["output"]["color"]; p["output"]["color"] = nullptr;
            p["depth_load_op"] = "clear"; p["depth_store_op"] = "store";
            p["raster_state"] = {{"depth_test", true}, {"depth_write", true}, {"depth_compare", "always"}};
        }
        auto root = TestSupport::renderGraphRoot(config);
        TestSupport::retainObserved(root, "main_render", {"alias_sink", "alias_sink_b"});
        writeTextFile(directory / "hybrid.json", root.dump(2));
        configureRayQueryTestRuntime(directory);
        GET_MODULE(EngineLaunchConfig).vulkan_validation = VulkanValidationMode::enabled;
        GET_MODULE(EngineLaunchConfig).gpu_labels = true;
        auto &core = GET_MODULE(VulkanManageCore);
        Rc7bValidationCapture validation_messages{core};
        const GraphTransformRegistrySnapshot transforms;
        const RenderStrategyRegistrySnapshot strategies;
        const SubgraphReplacementRegistrySnapshot replacements;
        const VulkanRenderCompilerBackendContext backend{vk::Format::eB8G8R8A8Srgb, {32, 32},
            core.getPhysDevice(), {}, core.getRuntimeCapabilities(), VulkanRenderCompilerDevicePlanningMode::device_required, core.getDevice(), core.getGraphicsQueueFamilyIndex()};
        const std::vector<RenderCompilerProgramVariantRequest> variants{{.graph_variant = RenderPipelineGraphVariant::flat}};
        const auto compiled = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), {
            .rendering_config = root, .path_resolver = GET_MODULE(PathResolver), .runtime_shader_compiler_enabled = true,
            .graph_transforms = transforms, .render_strategies = strategies, .subgraph_replacements = replacements,
            .backend_context = backend, .variants = variants});
        const auto *placed_a = compiled.generation_resources.findImage("alias_a");
        REQUIRE(placed_a); REQUIRE(placed_a->alias_group);
        for (const auto *name : {"alias_b", "alias_c"}) {
            const auto *placed = compiled.generation_resources.findImage(name);
            REQUIRE(placed); CHECK(placed->alias_group == placed_a->alias_group);
        }
        auto &renderer = GET_MODULE(Renderer);
        const auto &validation = core.getVulkanValidationStatus();
        REQUIRE(validation.enabled);
        REQUIRE(validation.synchronization_validation);
        auto &targets = GET_MODULE(RenderTargetContainer);
        const auto ids = [&] { return std::array{targets.getRenderTargetIdByName("alias_a"),
            targets.getRenderTargetIdByName("alias_b"), targets.getRenderTargetIdByName("alias_c")}; };
        const auto initial_ids = ids();
        const auto initial_token = targets.aliasGroup(initial_ids[0]);
        REQUIRE(initial_token);
        REQUIRE(targets.sharesAllocation(initial_ids[0], initial_ids[1]));
        REQUIRE(targets.sharesAllocation(initial_ids[1], initial_ids[2]));
        REQUIRE(targets.getImage(initial_ids[0]).image.get() != targets.getImage(initial_ids[1]).image.get());
        const auto initial_allocation = targets.getImage(initial_ids[0]).allocation;
        const auto allocation_stats = [&](const auto &values) {
            std::set<const vma::UniqueAllocation *> allocations;
            vk::DeviceSize bytes = 0;
            for (const auto id : values) {
                const auto &image = targets.getImage(id);
                if (allocations.insert(image.allocation.get()).second) bytes += core.imageAllocationBytes(image);
            }
            return std::pair{allocations.size(), bytes};
        };
        const auto shared_stats = allocation_stats(initial_ids);
        REQUIRE(shared_stats.first == 1);
        std::vector<RenderTargetLayoutTracker::AliasTransition> trace;
        auto &tracker = RendererTestAccess::imageLayoutTracker(renderer);
        tracker.alias_transition_observer = [&](const auto &event) { trace.push_back(event); };
        const auto assert_dependency = [&](const auto &event) {
            CHECK(event.dependency);
            CHECK(event.token == *initial_token);
            CHECK(event.src_stage == vk::PipelineStageFlags{vk::PipelineStageFlagBits::eAllCommands});
            CHECK(event.dst_stage == event.src_stage);
            CHECK(event.src_access == (vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite));
            CHECK(event.dst_access == event.src_access);
            CHECK(event.old_layout == vk::ImageLayout::eUndefined);
        };
        const auto observe = [&] {
            for (const auto &[name, green] : std::array{std::pair{"alias_sink", false}, std::pair{"alias_sink_b", true}}) {
                const auto image = RendererTestAccess::readRenderTarget(renderer, name, {});
                REQUIRE_FALSE(image.bytes.empty());
                for (std::size_t i = 0; i < image.bytes.size(); i += 4) {
                    REQUIRE(image.bytes[i] == (green ? 0 : 255));
                    REQUIRE(image.bytes[i + 1] == (green ? 255 : 0));
                    REQUIRE(image.bytes[i + 2] == 0);
                    REQUIRE(image.bytes[i + 3] == 255);
                }
            }
        };
        // Three consecutive submissions, without readback or idle between them.
        for (int frame = 0; frame < 3; ++frame) {
            trace.clear(); GET_MODULE(EngineTime).advance(); renderer.render();
            REQUIRE_FALSE(trace.empty());
            assert_dependency(trace.front());
            const auto b = std::find_if(trace.begin(), trace.end(), [&](const auto &e) { return e.resource == initial_ids[1]; });
            REQUIRE(b != trace.end()); assert_dependency(*b);
        }
        core.waitIdle(); observe();
        // Explicit reset reuses the old token through pending.
        tracker.reset(); trace.clear(); renderer.render();
        REQUIRE_FALSE(trace.empty()); assert_dependency(trace.front());
        core.waitIdle(); observe();
        // The recording sequence produced when every other resident is
        // skipped: the same writer, same layout, consecutive frames, then a
        // frame recording no residents at all. Record real attachment clears.
        auto &utils = GET_MODULE(VulkanUtils);
        const auto record_only_a = [&] {
            tracker.beginFrame(); trace.clear();
            utils.executeOneTimeCmd([&](vk::CommandBuffer command) {
                const auto layout = depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal : vk::ImageLayout::eColorAttachmentOptimal;
                tracker.transition(command, targets, utils, initial_ids[0], layout);
                vk::RenderingAttachmentInfo attachment;
                attachment.imageView = targets.getImageView(initial_ids[0]); attachment.imageLayout = layout;
                attachment.loadOp = vk::AttachmentLoadOp::eClear; attachment.storeOp = vk::AttachmentStoreOp::eStore;
                if (depth) attachment.clearValue.depthStencil = vk::ClearDepthStencilValue{0.25f, 0};
                else attachment.clearValue.color = vk::ClearColorValue{std::array{1.0f, 0.0f, 0.0f, 1.0f}};
                vk::RenderingInfo info; info.renderArea = vk::Rect2D{{0, 0}, {32, 32}}; info.layerCount = 1;
                if (depth) info.pDepthAttachment = &attachment;
                else info.setColorAttachments(attachment);
                command.beginRendering(info); command.endRendering();
            });
            REQUIRE(trace.size() == 1); assert_dependency(trace.front());
        };
        record_only_a(); record_only_a();
        const auto program = GET_MODULE(FrameGraphRuntimeContainer).findProgram(
            GET_MODULE(RenderingPassContainer).getRenderingPassIdByName("main_render"));
        REQUIRE(program);
        auto skipped_frame = program->frame_graph;
        auto skipped_plan = std::make_shared<VulkanTargetPlan>(*skipped_frame.target_plan);
        for (auto &node : skipped_frame.nodes) { node.view_family = "$capture/cube"; node.view_select = {{"face", 1}}; }
        for (auto &scope : skipped_plan->scopes) scope.view_select = {{"face", 1}};
        skipped_frame.target_plan = skipped_plan;
        const std::vector<RenderViewAxisCoordinates> axes{{{"face", 0}}};
        const std::array families{LogicalFrameViewFamilyCardinality{mainRenderViewFamilyId, 1},
            LogicalFrameViewFamilyCardinality{"$capture/cube", 1, axes}};
        const auto skipped = buildLogicalFrameViewFamilySchedule(skipped_frame, families);
        REQUIRE_FALSE(skipped.empty());
        tracker.beginFrame(); trace.clear();
        utils.executeOneTimeCmd([&](vk::CommandBuffer) {
            for (const auto &invocation : skipped) REQUIRE(invocation.no_selected_view);
        });
        REQUIRE(trace.empty());
        record_only_a();
        // Once readers can touch an aliased image without retaining an output.
        // Use the production once scheduler to make the intervening empty frame.
        const std::array once_nodes{OnceScheduleNode{"once_reader", {"alias_a"}, {}}};
        const OnceResourceFacts once_facts{.identity = [](std::string_view) -> std::optional<std::uint64_t> { return 1; },
            .host_digest = [](std::string_view) -> std::optional<std::uint64_t> { return std::nullopt; }};
        const std::map<std::string, OnceRunRecord, std::less<>> last_once{{"once_reader", onceRunRecord(once_nodes[0], 1, 1, once_facts)}};
        const auto once = planOnceRuns(once_nodes, last_once, 1, once_facts, {});
        REQUIRE_FALSE(once.runs(0));
        tracker.beginFrame(); trace.clear();
        utils.executeOneTimeCmd([&](vk::CommandBuffer) { REQUIRE_FALSE(once.runs(0)); });
        REQUIRE(trace.empty()); record_only_a();
        core.waitIdle();
        // B2 observes every output of two further frames individually.
        for (int frame = 0; frame < 2; ++frame) { renderer.render(); core.waitIdle(); observe(); }
        auto &runtime = GET_MODULE(FrameGraphRuntimeContainer);
        const auto held_snapshot = runtime.snapshot();
        const auto reload = [&] {
            writeTextFile(directory / "hybrid.json", root.dump(2));
            return GET_MODULE(watch::ReloadService).applyRequestForTesting({.key = watch::makeAssetKey("hybrid.json")});
        };
        // Split {a,b,c} -> {a,c}+{b}, then remove all sharing, then merge.
        root["rendering"]["target_planning"]["graphs"]["main_render"]["resources"]["alias_b"]["no_alias"] = true;
        REQUIRE(reload()); renderer.render();
        const auto split = ids();
        const auto split_snapshot = runtime.snapshot();
        const auto split_image = targets.getImage(split[0]).image.get();
        CHECK(targets.sharesAllocation(split[0], split[2]));
        CHECK_FALSE(targets.sharesAllocation(split[0], split[1]));
        CHECK(targets.getImage(initial_ids[0]).allocation == initial_allocation);
        for (const auto *name : {"alias_a", "alias_c"}) root["rendering"]["target_planning"]["graphs"]["main_render"]["resources"][name]["no_alias"] = true;
        validation_messages.phase = "unshared_control";
        REQUIRE(reload());
        CHECK(split_snapshot != runtime.snapshot());
        CHECK(targets.getImage(split[0]).image.get() == split_image);
        renderer.render();
        const auto unshared = allocation_stats(ids());
        CHECK(unshared.first == 3); CHECK(unshared.second == shared_stats.second * 3);
        renderer.render(); renderer.render();
        core.waitIdle(); observe();
        validation_messages.phase = "shared";
        for (const auto *name : {"alias_a", "alias_b", "alias_c"}) root["rendering"]["target_planning"]["graphs"]["main_render"]["resources"][name].erase("no_alias");
        REQUIRE(reload()); trace.clear(); renderer.render();
        REQUIRE(targets.sharesAllocation(ids()[0], ids()[1]));
        CHECK(targets.aliasGroup(ids()[0]) != initial_token);
        REQUIRE_FALSE(trace.empty()); CHECK(trace.front().dependency); CHECK(trace.front().old_layout == vk::ImageLayout::eUndefined);
        const auto stable = runtime.snapshot(); const auto stable_ids = ids();
        root["rendering"]["target_planning"]["graphs"]["main_render"]["resources"]["alias_b"]["no_alias"] = true;
        root["rendering"]["target_planning"]["graphs"]["main_render"]["resources"]["alias_sink"]["retain"] = false;
        RenderingPassConfigRegistrationDependencies::Options failed_options;
        failed_options.fault_point = RenderPipelineGpuRegistrationFaultPoint::after_runtime_prepare;
        REQUIRE_THROWS_WITH(registerRenderingPassConfigFromJsonData(root.dump(), {32, 32},
            gpuArenaRegistrationDependencies(std::move(failed_options))), "Injected render pipeline GPU registration failure");
        CHECK(runtime.snapshot() == stable); CHECK(ids() == stable_ids);
        CHECK(targets.sharesAllocation(stable_ids[0], stable_ids[1]));
        REQUIRE_NOTHROW(RendererTestAccess::readRenderTarget(renderer, "alias_sink", {}));
        root["rendering"]["rendering_passes"][0]["passes"][0]["shader"]["fragment"] = "project://shaders/missing";
        CHECK_FALSE(reload()); CHECK(runtime.snapshot() == stable); CHECK(ids() == stable_ids);
        core.waitIdle(); observe();
        REQUIRE(held_snapshot);
        tracker.alias_transition_observer = {};
        if (const auto *path = std::getenv("RC7B_VALIDATION_PATH"))
            std::ofstream{path, std::ios::app} << nlohmann::json{{"summary", true}, {"format", format},
                {"validation", validation.enabled}, {"synchronization", validation.synchronization_validation},
                {"shared_allocations", shared_stats.first}, {"shared_bytes", shared_stats.second},
                {"unshared_allocations", unshared.first}, {"unshared_bytes", unshared.second},
                {"messages", validation_messages.messages.size()}}.dump() << '\n';
        for (const auto &[phase, message] : validation_messages.messages) {
            if (const auto *path = std::getenv("RC7B_VALIDATION_PATH"))
                std::ofstream{path, std::ios::app} << nlohmann::json{{"format", format}, {"phase", phase}, {"message", message}}.dump() << '\n';
            const bool shared_image = message.find("/alias_a/") != std::string::npos || message.find("/alias_b/") != std::string::npos || message.find("/alias_c/") != std::string::npos;
            if (phase == "shared" && shared_image) { INFO(message); CHECK(message.empty()); }
        }
        std::filesystem::remove_all(directory);
    }
#else
    SKIP("runtime shader compiler unavailable");
#endif
}
