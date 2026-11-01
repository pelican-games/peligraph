// RC-7d D1 on a real tiler's device facts: the memory types and the image
// requirements measured on a Snapdragon 8 Elite (Adreno 830), copied into
// test/fixtures/device_facts/adreno830_sm8750.json. rc7d_test_support.hpp is
// included by rc7d_compiler_test.inc.
#include <filesystem>
#include <fstream>
namespace Pelican {
namespace {
struct Adreno830DeviceFacts {
    std::vector<RenderingTargetPlanDeviceFacts::MemoryType> memory_types;
    std::map<std::string, ImageMemoryRequirements> images;
};
Adreno830DeviceFacts adreno830DeviceFacts() {
    std::ifstream file{std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "device_facts" /
                       "adreno830_sm8750.json"};
    REQUIRE(file);
    const auto json = nlohmann::json::parse(file);
    Adreno830DeviceFacts result;
    for (const auto &type : json.at("memory_types"))
        result.memory_types.push_back({vk::MemoryPropertyFlags{type.at("property_flags").get<std::uint32_t>()},
                                       type.at("heap_index").get<std::uint32_t>()});
    for (const auto &[name, image] : json.at("images").items())
        result.images.emplace(name, ImageMemoryRequirements{image.at("size").get<std::uint64_t>(),
                                                            image.at("alignment").get<std::uint64_t>(),
                                                            image.at("memory_type_bits").get<std::uint32_t>(),
                                                            image.at("requires_dedicated").get<bool>(),
                                                            image.at("prefers_dedicated").get<bool>()});
    return result;
}
} // namespace

TEST_CASE("RC-7d D1 Adreno 830 device facts keep transient and tile-local images lazy outside regions",
          "[rendercompiler][rc7d][rc7d-d1][rc7d-adreno830]") {
    const auto device = adreno830DeviceFacts();
    // The fixture as measured: type 8 is the one LAZILY_ALLOCATED type, a
    // TRANSIENT image may live in type 0 or 8, a materialized one in type 0.
    REQUIRE(device.memory_types.size() == 9);
    for (std::size_t i = 0; i < device.memory_types.size(); ++i)
        CHECK(bool(device.memory_types[i].property_flags & vk::MemoryPropertyFlagBits::eLazilyAllocated) == (i == 8));
    REQUIRE(device.memory_types[8].property_flags ==
            (vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eLazilyAllocated));
    for (const auto *name : {"transient_color", "transient_depth", "tile_local_color"})
        REQUIRE(device.images.at(name) == ImageMemoryRequirements{8392704, 4096, 257, false, false});
    REQUIRE(device.images.at("materialized_color") == ImageMemoryRequirements{8392704, 4096, 1, false, false});

    // Every image is the fixture's shape: 1920x1080 RGBA8, the scratch depth D32.
    const vk::Extent2D extent{1920, 1080};
    auto config = TestSupport::rc7dConfig();
    config["render_targets"].push_back({{"name", "scratch_depth"}, {"format", "D32_SFLOAT"}, {"format_class", "data"},
                                        {"extent_scale", 1.0}, {"usage", nlohmann::json::array({"DEPTH_STENCIL_ATTACHMENT"})}});
    for (auto &pass : config["rendering_passes"][0]["passes"])
        if (pass["name"] == "scratch_write") {
            pass["type"] = "raster"; // A fullscreen pass has no depth output.
            pass["output"]["depth"] = "scratch_depth";
            pass["draw"] = {{"vertex_count", 3}};
        }

    auto facts = TestSupport::rc7dFacts(0);
    facts.planning_extent = extent;
    facts.memory_types = device.memory_types;
    // The fixture's requirements by the query's TRANSIENT usage (and, for a
    // TRANSIENT one, its depth or input attachment usage: the same values).
    // display, the canonical output the engine adds, is B8G8R8A8_SRGB:
    // materialized, 4 bytes a texel like the fixture's RGBA8, so the
    // materialized values apply to it as measured.
    std::vector<std::string> foreign_queries;
    facts.image_memory_requirements = [&](const vk::ImageCreateInfo &info) -> std::optional<ImageMemoryRequirements> {
        const bool depth = info.format == vk::Format::eD32Sfloat;
        const bool display = info.format == vk::Format::eB8G8R8A8Srgb &&
                             !(info.usage & vk::ImageUsageFlagBits::eTransientAttachment);
        if ((info.format != vk::Format::eR8G8B8A8Unorm && !depth && !display) ||
            info.extent != vk::Extent3D{1920, 1080, 1} || info.mipLevels != 1 || info.arrayLayers != 1)
            foreign_queries.push_back(std::to_string(static_cast<int>(info.format)) + " " + std::to_string(info.extent.width) + "x" +
                                      std::to_string(info.extent.height));
        if (!(info.usage & vk::ImageUsageFlagBits::eTransientAttachment)) return device.images.at("materialized_color");
        if (depth) return device.images.at("transient_depth");
        if (info.usage & vk::ImageUsageFlagBits::eInputAttachment) return device.images.at("tile_local_color");
        return device.images.at("transient_color");
    };
    const auto backend = [&] {
        VulkanRenderCompilerBackendContext context{vk::Format::eB8G8R8A8Srgb, extent, {}, {}, {},
                                                   VulkanRenderCompilerDevicePlanningMode::compiler_only};
        context.planning_facts = facts;
        return context;
    };
    ProgramInputFixture fixture;
    fixture.useAuthoredConfig(config, "rc7d-adreno830");
    fixture.backend_context = backend();
    fixture.variants = {{.graph_variant = RenderPipelineGraphVariant::flat}};
    auto input = fixture.input();
    input.runtime_shader_compiler_enabled = true;
    const auto result = runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), input);
    CHECK(foreign_queries == std::vector<std::string>{});
    const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*result.variants[0].physical_package);
    const auto &plan = *physical.target_plans.at("main");
    const auto resource = [&](const char *name) -> const auto & {
        const auto found = std::find_if(plan.resources.begin(), plan.resources.end(),
                                        [&](const auto &r) { return r.logical_resource == name; });
        REQUIRE(found != plan.resources.end());
        return *found;
    };
    REQUIRE(resource("scratch").representation == VulkanResourceRepresentation::transient_attachment);
    REQUIRE(resource("scratch_depth").representation == VulkanResourceRepresentation::transient_attachment);
    REQUIRE(resource("local_source").representation == VulkanResourceRepresentation::tile_local_attachment);
    const auto resident = [&](const char *name) {
        for (const auto &region : plan.memory_regions)
            for (const auto &r : region.residents)
                if (r.resource == name) return true;
        return false;
    };
    const auto decided = [&](const char *name, const char *selected) {
        return std::any_of(plan.decisions.begin(), plan.decisions.end(),
                           [&](const auto &v) { return v.subject == name && v.selected == selected; });
    };
    for (const auto *name : {"scratch", "scratch_depth", "local_source"}) {
        CAPTURE(name);
        CHECK_FALSE(resident(name));
        CHECK(decided(name, "lazy_memory"));
    }
    for (const auto *name : {"A", "B", "local_output"}) {
        CAPTURE(name);
        CHECK(resident(name));
        CHECK_FALSE(decided(name, "lazy_memory"));
    }
    const auto &ctx = physical.target_plan_compilation.verification_contexts.front();
    REQUIRE_NOTHROW(verifyVulkanCompletePhysicalPlanPackage(*ctx.logical_graph, ctx.topology, plan, ctx.node_order,
                                                            ejectVulkanCompletePhysicalPlanPackage(plan),
                                                            ctx.format_capabilities));
    const nlohmann::json wire = vulkanTargetPlanToJson(plan);
    CHECK(validatePhysicalTargetPlanWire(&wire).available());

    // A required alias group naming a lazy image is refused by name.
    auto required = config;
    const nlohmann::json group{{"value", "lazy"}, {"strength", "require"}};
    required["target_planning"]["graphs"]["main"]["resources"]["scratch"]["alias_group"] = group;
    required["target_planning"]["graphs"]["main"]["resources"]["A"]["alias_group"] = group;
    ProgramInputFixture required_fixture;
    required_fixture.useAuthoredConfig(required, "rc7d-adreno830-require");
    required_fixture.backend_context = backend();
    required_fixture.variants = fixture.variants;
    auto required_input = required_fixture.input();
    required_input.runtime_shader_compiler_enabled = true;
    CHECK_THROWS_WITH(runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(), required_input),
                      Catch::Matchers::ContainsSubstring("alias.lazy_memory@1"));
}

} // namespace Pelican
