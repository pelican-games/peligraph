#include "../src/core/renderingpass/memoryregionpacking.hpp"
#include "../src/core/renderingpass/rendertargetimagecontract.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/vulkanimagevocabulary.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <map>
#include <tuple>

namespace Pelican {
namespace {
MemoryRegionResident resident(const MemoryPackingResult &result, const std::string &name) {
    for (const auto &region : result.regions) for (const auto &r : region.residents)
        if (r.resource == name) return r;
    FAIL("missing resident " << name);
    return {};
}
void legal(const MemoryPackingInputs &inputs, const MemoryPackingRequirements &requirements,
           const MemoryPackingResult &result) {
    for (const auto &region : result.regions) {
        std::uint64_t end = 0, alignment = 1;
        for (const auto &r : region.residents) {
            const auto &q = *requirements.at(r.resource);
            CHECK_FALSE(q.requires_dedicated);
            CHECK(region.memory_type_bits == q.memory_type_bits);
            CHECK(r.size == q.size);
            CHECK(r.alignment == q.alignment);
            CHECK(r.offset % r.alignment == 0);
            end = std::max(end, r.offset + r.size);
            alignment = std::max(alignment, r.alignment);
            for (const auto &[a, b] : inputs.forbidden_pairs) {
                if (a != r.resource) continue;
                const auto other = std::ranges::find(region.residents, b, &MemoryRegionResident::resource);
                if (other != region.residents.end())
                    CHECK((r.offset + r.size <= other->offset || other->offset + other->size <= r.offset));
            }
        }
        CHECK(region.size == end);
        CHECK(region.alignment == alignment);
    }
    for (const auto &set : inputs.same_offset_sets) for (const auto &name : set)
        CHECK(resident(result, name).offset == resident(result, set.front()).offset);
}
// Frozen pre-RC7c registration builder. Compare every VkImageCreateInfo field;
// pointer addresses/padding are not Vulkan inputs; pointed-to queue bits are.
vk::ImageCreateInfo legacyInfo(const RenderTargetDefinition &d, vk::Extent2D base,
    const std::uint32_t *queue, bool attachment,
    const vk::ImageCreateInfo *unused = nullptr) {
    const auto extent = resolveResourceExtent(d.fixed_extent
        ? ResourceExtentPlan{.kind = ResourceExtentKind::fixed, .width = d.fixed_extent->width, .height = d.fixed_extent->height}
        : ResourceExtentPlan{.kind = ResourceExtentKind::output_relative, .scale_x = d.extent_scale, .scale_y = d.extent_scale},
        {base.width, base.height});
    const auto usage = attachment ? d.usage & (ImageUsageBit::color_attachment | ImageUsageBit::depth_stencil_attachment)
        : engineImageUsage({.declared = d.usage, .history = d.history, .storage = d.storage_mode});
    vk::ImageCreateInfo ci;
    ci.flags = !attachment && d.alias_group ? vk::ImageCreateFlagBits::eAlias : vk::ImageCreateFlags{};
    if (d.dimension == ImageResourceDimension::cube) ci.flags |= vk::ImageCreateFlagBits::eCubeCompatible;
    ci.imageType = vk::ImageType::e2D;
    ci.format = toVulkanFormat(d.format);
    ci.extent = vk::Extent3D{extent.width, extent.height, 1};
    ci.mipLevels = attachment ? 1 : resolveImageMipLevels(d.mip_levels, extent.width, extent.height);
    ci.arrayLayers = d.array_layers;
    ci.samples = attachment ? static_cast<vk::SampleCountFlagBits>(d.samples) : vk::SampleCountFlagBits::e1;
    ci.tiling = vk::ImageTiling::eOptimal;
    ci.usage = toVulkanImageUsage(usage);
    ci.sharingMode = vk::SharingMode::eExclusive;
    ci.initialLayout = vk::ImageLayout::eUndefined;
    ci.queueFamilyIndexCount = 1;
    ci.pQueueFamilyIndices = queue;
    return ci;
}
}

TEST_CASE("RC-7c contract preserves all legacy image create-info bits", "[rc7c][contract]") {
    for (int kind = 0; kind < 12; ++kind) {
        CAPTURE(kind);
        RenderTargetDefinition d;
        d.name = "contract";
        d.usage = ImageUsageBit::color_attachment | ImageUsageBit::sampled;
        d.extent_scale = 0.5f;
        if (kind == 1) d.fixed_extent = ResolvedResourceExtent{31, 17};
        if (kind == 2) d.mip_levels = {.mode = ImageMipLevelMode::full_chain};
        if (kind == 3) { d.dimension = ImageResourceDimension::cube; d.array_layers = 6; }
        if (kind == 4 || kind == 8) d.alias_group = "group";
        if (kind == 5) d.history = true;
        if (kind == 6) { d.storage_mode = RenderTargetStorageMode::transient_attachment; d.usage = ImageUsageBit::color_attachment; }
        if (kind == 7) { d.samples = 4; d.array_layers = 3; }
        if (kind == 9) d.storage_mode = RenderTargetStorageMode::tile_local_attachment;
        if (kind == 10) { d.mip_levels.count = 3; d.usage |= ImageUsageBit::storage; }
        if (kind == 11) { d.dimension = ImageResourceDimension::cube; d.array_layers = 6; d.alias_group = "cube-alias"; }
        const std::uint32_t queue = 7;
        const auto owner = renderTargetImageContract(d, {128, 128}, queue);
        const auto legacy_owner = legacyInfo(d, {128, 128}, &queue, false);
        const auto contract = renderTargetImageContract(d, {128, 128}, queue,
            kind == 7);
        const auto actual = contract.createInfo();
        const auto expected = legacyInfo(d, {128, 128}, actual.pQueueFamilyIndices,
            kind == 7);
        CHECK(actual == expected);
        CHECK(actual.pNext == nullptr);
        { REQUIRE(actual.pQueueFamilyIndices); CHECK(*actual.pQueueFamilyIndices == 7); }
        // Copy lifetime: returned info points into its own value.
        const auto copy = contract;
        if (copy.queue_family) CHECK(copy.createInfo().pQueueFamilyIndices != actual.pQueueFamilyIndices);
    }
}

TEST_CASE("RC-7c region residents resolve independent extents before and after resize", "[rc7c][contract][regression]") {
    RenderTargetDefinition owner;
    owner.name = "owner";
    owner.fixed_extent = ResolvedResourceExtent{64, 64};
    owner.alias_group = "group";
    owner.usage = ImageUsageBit::color_attachment | ImageUsageBit::sampled;
    auto member = owner;
    member.name = "member";
    member.fixed_extent.reset();
    member.extent_scale = 1;
    const std::uint32_t queue = 7;
    for (const auto base : {vk::Extent2D{64, 64}, vk::Extent2D{1024, 1024}}) {
        CAPTURE(base.width, base.height);
        const auto physical_owner = renderTargetImageContract(owner, base, queue);
        const auto legacy_owner = legacyInfo(owner, base, &queue, false);
        const auto actual = renderTargetImageContract(member, base, queue);
        const auto expected = legacyInfo(member, base, actual.createInfo().pQueueFamilyIndices, false);
        CHECK(actual.createInfo() == expected);
        CHECK(actual.image.extent == (vk::Extent3D{base.width, base.height, 1}));
        CHECK(physical_owner.image.extent == (vk::Extent3D{64,64,1}));
    }
}

TEST_CASE("RC-7c live fixed seven mips reuse the original extent at a smaller base", "[rc7c][contract][regression]") {
    RenderTargetDefinition d;
    d.name = "live-seven-mips";
    d.extent_scale = 1;
    d.usage = ImageUsageBit::color_attachment | ImageUsageBit::sampled;
    d.mip_levels.count = 7;
    const auto live = renderTargetImageContract(d, {64, 64}, 2);
    const auto actual = renderTargetImageContract(d, {32, 32}, 2, false, &live);
    // Legacy re-registration returned the already registered image unchanged.
    const auto expected = legacyInfo(d, {64, 64}, actual.createInfo().pQueueFamilyIndices, false);
    CHECK(actual.createInfo() == expected);
}

TEST_CASE("RC-7c contract returns the live usage and layer superset", "[rc7c][contract]") {
    RenderTargetDefinition d;
    d.name = "live";
    d.usage = ImageUsageBit::color_attachment | ImageUsageBit::sampled;
    d.array_layers = 4;
    const auto live = renderTargetImageContract(d, {64, 64}, 2);
    d.usage = ImageUsageBit::color_attachment;
    d.array_layers = 1;
    const auto reused = renderTargetImageContract(d, {64, 64}, 2, false, &live);
    auto expected = live.createInfo();
    expected.pQueueFamilyIndices = reused.createInfo().pQueueFamilyIndices;
    CHECK(reused.createInfo() == expected);
    d.usage |= ImageUsageBit::storage;
    CHECK_THROWS_WITH(renderTargetImageContract(d, {64, 64}, 2, false, &live),
        Catch::Matchers::ContainsSubstring("usage expansion"));
    d.usage = ImageUsageBit::color_attachment;
    d.array_layers = 5;
    CHECK_THROWS_WITH(renderTargetImageContract(d, {64, 64}, 2, false, &live),
        Catch::Matchers::ContainsSubstring("array_layers expansion"));
}

TEST_CASE("RC-7c packing prohibits range overlap and aligns first-fit", "[rc7c][packing]") {
    MemoryPackingInputs inputs{{{"a", 3}, {"b", 3}, {"c", 3}},
        {{"a", "b"}, {"b", "c"}}, {}, {"a", "b", "c"}};
    MemoryPackingRequirements q{{"a", ImageMemoryRequirements{13, 1, 3}},
        {"b", ImageMemoryRequirements{19, 16, 3}}, {"c", ImageMemoryRequirements{37, 8, 3}}};
    const auto original = inputs;
    const auto packed = packMemoryRegions(inputs, q);
    legal(inputs, q, packed);
    REQUIRE(packed.regions.size() == 1);
    CHECK(resident(packed, "a").offset == 0);
    CHECK(resident(packed, "b").offset == 16);
    CHECK(resident(packed, "c").offset == 40);
    CHECK(packed.regions[0].size == 77);
    CHECK(inputs == original);
}

TEST_CASE("RC-7c packing same-offset slot wins over first-fit resource order", "[rc7c][packing][same-offset]") {
    // A[0,0], X[1,3], B[2,2]: without slot aggregation X steals offset 0
    // after A, then B is incorrectly placed after X.
    MemoryPackingInputs inputs{{{"A", 1}, {"X", 1}, {"B", 1}},
        {{"X", "B"}}, {{"A", "B"}}, {"A", "X", "B"}};
    MemoryPackingRequirements q{{"A", ImageMemoryRequirements{16, 4, 1}},
        {"X", ImageMemoryRequirements{16, 4, 1}}, {"B", ImageMemoryRequirements{16, 4, 1}}};
    const auto packed = packMemoryRegions(inputs, q);
    legal(inputs, q, packed);
    CHECK(resident(packed, "A").offset == 0);
    CHECK(resident(packed, "B").offset == 0);
    CHECK(resident(packed, "X").offset == 16);
    // The slot's maximum size/alignment and all members' prohibitions count.
    q["B"] = ImageMemoryRequirements{49, 32, 1};
    const auto larger = packMemoryRegions(inputs, q);
    legal(inputs, q, larger);
    CHECK(resident(larger, "X").offset == 52);
    CHECK(larger.regions[0].size == 68);
}

TEST_CASE("RC-7c packing fake facts yield partial overlaps for different image shapes and formats", "[rc7c][packing][facts]") {
    MemoryPackingInputs inputs;
    MemoryPackingRequirements q;
    RenderingTargetPlanDeviceFacts facts;
    facts.memory_types = {{vk::MemoryPropertyFlagBits::eDeviceLocal, 0}};
    facts.image_memory_requirements = [](const vk::ImageCreateInfo &ci) {
        return std::optional{ImageMemoryRequirements{ci.extent.width, 8, 1}};
    };
    for (const auto &[name, width, format] : std::vector<std::tuple<std::string, std::uint32_t, ImageFormat>>{
        {"a", 24, ImageFormat::r8_unorm}, {"b", 40, ImageFormat::r16_sfloat},
        {"c", 48, ImageFormat::r8g8b8a8_unorm}, {"d", 56, ImageFormat::d32_sfloat}}) {
        RenderTargetDefinition d;
        d.name = name; d.format = format; d.fixed_extent = ResolvedResourceExtent{width, width / 2};
        d.usage = ImageUsageBit::sampled;
        const auto contract = renderTargetImageContract(d, {1, 1}, 0);
        q[name] = facts.image_memory_requirements(contract.createInfo());
        inputs.resources.push_back({name, 1}); inputs.order.push_back(name);
    }
    inputs.forbidden_pairs = {{"a", "b"}, {"a", "c"}};
    const auto packed = packMemoryRegions(inputs, q);
    legal(inputs, q, packed);
    CHECK(resident(packed, "a").offset == 0);
    CHECK(resident(packed, "b").offset == 24);
    CHECK(resident(packed, "c").offset == 24);
    CHECK(resident(packed, "d").offset == 0);
    // d=[0,56) partially overlaps both b=[24,64) and c=[24,72).
    CHECK(packed.regions[0].size == 72);
    CHECK_FALSE(RenderingTargetPlanDeviceFacts{}.image_memory_requirements);
}

TEST_CASE("RC-7c packing separates exact keys and excludes dedicated or unknown facts", "[rc7c][packing][facts]") {
    MemoryPackingInputs inputs{{{"a", 3}, {"b", 7}, {"c", 3}, {"d", 3}, {"e", 3}}, {}, {}, {"a", "b", "c", "d", "e"}};
    MemoryPackingRequirements q{{"a", ImageMemoryRequirements{16, 4, 3}},
        {"b", ImageMemoryRequirements{16, 4, 7}}, {"c", ImageMemoryRequirements{16, 4, 3, true}},
        {"d", std::nullopt}, {"e", ImageMemoryRequirements{8, 8, 3, false, true}}};
    const auto packed = packMemoryRegions(inputs, q);
    legal(inputs, q, packed);
    REQUIRE(packed.regions.size() == 2);
    CHECK(packed.regions[0].memory_type_bits == 3);
    CHECK(packed.regions[1].memory_type_bits == 7);
    CHECK(packed.excluded == std::vector<std::string>{"c", "d"});
    CHECK(packMemoryRegions(inputs, {}).regions.empty());
    CHECK(packMemoryRegions(inputs, {}).excluded.size() == 5);
    // Resize: requirements' key, not the saved plan snapshot, is authoritative.
    q["b"] = ImageMemoryRequirements{24, 8, 3};
    CHECK(packMemoryRegions(inputs, q).regions.size() == 1);
}

TEST_CASE("RC-7c packing seeds zero through seven reproduce legal slot placements", "[rc7c][packing][seed]") {
    MemoryPackingInputs inputs{{{"a", 1}, {"b", 1}, {"c", 1}, {"d", 1}, {"e", 2}},
        {{"a", "b"}, {"b", "c"}, {"c", "d"}}, {{"a", "d"}}, {"a", "b", "c", "d", "e"}};
    MemoryPackingRequirements q{{"a", ImageMemoryRequirements{13, 4, 1}},
        {"b", ImageMemoryRequirements{39, 16, 1}}, {"c", ImageMemoryRequirements{27, 8, 1}},
        {"d", ImageMemoryRequirements{29, 32, 1}}, {"e", ImageMemoryRequirements{18, 8, 2}}};
    for (std::uint32_t seed = 0; seed < 8; ++seed) {
        CAPTURE(seed); inputs.seed = seed;
        const auto packed = packMemoryRegions(inputs, q);
        CHECK(packed == packMemoryRegions(inputs, q));
        legal(inputs, q, packed);
    }
}

TEST_CASE("RC-7c packing identical requirements retain the RC-7b lifetime grouping", "[rc7c][packing][facts]") {
    MemoryPackingInputs inputs{{{"a", 1}, {"c", 1}, {"b", 1}},
        {{"a", "c"}, {"c", "b"}}, {}, {"a", "c", "b"}};
    const ImageMemoryRequirements r{16, 8, 1};
    MemoryPackingRequirements q{{"a", r}, {"b", r}, {"c", r}};
    const auto packed = packMemoryRegions(inputs, q);
    legal(inputs, q, packed);
    CHECK(resident(packed, "a").offset == resident(packed, "b").offset);
    CHECK(resident(packed, "c").offset == 16);
    CHECK(packed.regions[0].size == 32);
}

TEST_CASE("RC-7c packing rejects contradictory slots and invalid contracts", "[rc7c][packing]") {
    MemoryPackingInputs inputs{{{"a", 1}, {"b", 1}}, {}, {{"a", "b"}}, {"a", "b"}};
    MemoryPackingRequirements q{{"a", ImageMemoryRequirements{16, 8, 1}},
        {"b", ImageMemoryRequirements{16, 8, 3}}};
    CHECK_THROWS_WITH(packMemoryRegions(inputs, q),
        Catch::Matchers::ContainsSubstring("alias_generation_contract@1"));
    q["b"] = ImageMemoryRequirements{16, 8, 1};
    inputs.forbidden_pairs = {{"a", "b"}};
    CHECK_THROWS_WITH(packMemoryRegions(inputs, q),
        Catch::Matchers::ContainsSubstring("forbidden same-offset pair"));
    inputs.forbidden_pairs.clear();
    inputs.order = {"a", "a"};
    CHECK_THROWS_WITH(packMemoryRegions(inputs, q),
        Catch::Matchers::ContainsSubstring("duplicate_order_resource"));
}
} // namespace Pelican

namespace Pelican {
TEST_CASE("RC-7c runtime repack refreshes keys dedicated and required slots", "[rc7c][packing][runtime]") {
    MemoryPackingInputs input{{{"A",1},{"B",1},{"C",1}},{{"A","B"}},{{"A","C"}},{"A","B","C"}};
    MemoryPackingRequirements requirements{{"A",ImageMemoryRequirements{512,256,1}},{"B",ImageMemoryRequirements{256,256,1}},{"C",ImageMemoryRequirements{768,256,1}}};
    std::map<std::string,std::string,std::less<>> reasons;
    CHECK(repackMemoryRegions(input,requirements,{},reasons).regions.size()==1);
    requirements.at("C")->memory_type_bits=2;
    const auto split=repackMemoryRegions(input,requirements,{},reasons);
    CHECK(split.regions.size()==2); CHECK(reasons.at("C")=="same_offset_memory_key_changed");
    CHECK_THROWS_WITH(repackMemoryRegions(input,requirements,{{"A","C"}},reasons),Catch::Matchers::ContainsSubstring("render_target.alias_repack_unsatisfiable@1: C"));
    requirements.at("C")->memory_type_bits=1; requirements.at("C")->requires_dedicated=true;
    const auto dedicated=repackMemoryRegions(input,requirements,{},reasons);
    CHECK(dedicated.excluded==std::vector<std::string>{"C"});
    CHECK_THROWS_WITH(repackMemoryRegions(input,requirements,{{"A","C"}},reasons),Catch::Matchers::ContainsSubstring("render_target.alias_repack_unsatisfiable@1: C"));
}

} // namespace Pelican
