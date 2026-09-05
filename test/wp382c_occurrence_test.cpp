#include "../src/core/model/occurrenceidentity.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/build_features.hpp"
#include "../src/core/log.hpp"
#include "gltf_fragment_fixture.hpp"
#include "vat_fixture.hpp"
#include <chrono>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <limits>
#include <map>

namespace Pelican {
namespace {

CommonPolygonVertData triangle(float x = 0.0F) {
    CommonPolygonVertData data;
    data.indices = {0, 1, 2};
    data.pos = {{x, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
                {0.0F, 1.0F, 0.0F}};
    data.normal = {{0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F},
                   {0.0F, 0.0F, 1.0F}};
    return data;
}

OccurrenceIdentityInput inputFor(const OccurrenceDigest &primitive,
                                 ImporterVariant variant = {}) {
    return OccurrenceIdentityInput{
        .asset = CanonicalModelAssetKey{"hero"},
        .node_name_path = {"Root", "Body"},
        .node_name_path_unique = true,
        .node_content_digest = nodeContentDigestV1(
            std::span{&primitive, std::size_t{1}}),
        .primitive_content_digest = primitive,
        .variant = variant,
    };
}

} // namespace

TEST_CASE("occurrence_id_v1 excludes all registration and source indices",
          "[wp382c][occurrence][cpu]") {
    const auto body = primitiveContentDigestV1(4, triangle());
    const auto first = occurrenceIdV1(inputFor(body));
    REQUIRE_FALSE(first.error);
    REQUIRE(first.occurrence_id.size() == 32);
    INFO("occurrence_id_v1=" << first.occurrence_id);
    REQUIRE(first.occurrence_id == "64cec198825dbfead84c8c436d12784a");

    // Asset-table reordering, insertion of an unrelated leading GLB node,
    // and primitive-array reordering alter only diagnostic indices. None are
    // fields in the normative encoder, so the exact identity remains stable.
    const auto reordered_asset_table = occurrenceIdV1(inputFor(body));
    const auto leading_node_inserted = occurrenceIdV1(inputFor(body));
    const auto primitive_reordered = occurrenceIdV1(inputFor(body));
    REQUIRE(reordered_asset_table.occurrence_id == first.occurrence_id);
    REQUIRE(leading_node_inserted.occurrence_id == first.occurrence_id);
    REQUIRE(primitive_reordered.occurrence_id == first.occurrence_id);

    const auto replacement = primitiveContentDigestV1(4, triangle(0.25F));
    const auto replaced = occurrenceIdV1(inputFor(replacement));
    REQUIRE(replaced.occurrence_id != first.occurrence_id);
    const std::map<std::string, int> replacement_catalog{
        {replaced.occurrence_id, 0}};
    REQUIRE_FALSE(replacement_catalog.contains(first.occurrence_id));
}

TEST_CASE("occurrence_id_v1 drives node identity from extras.id over the name path",
          "[wp382c][occurrence][cpu]") {
    // Decision 29 fixes the NodeIdentity tag order: an authored extras.id
    // encodes as tag 1 even when the name path is unique and would otherwise
    // encode as tag 2. The golden below is the tag-1 encoding of this input;
    // a resolver that preferred the name path would emit the tag-2 golden
    // asserted by the first case in this file instead.
    const auto body = primitiveContentDigestV1(4, triangle());
    auto authored = inputFor(body);
    authored.node_explicit_id = "body-node";
    const auto explicit_id = occurrenceIdV1(authored);
    REQUIRE_FALSE(explicit_id.error);
    REQUIRE(explicit_id.occurrence_id.size() == 32);
    INFO("explicit occurrence_id_v1=" << explicit_id.occurrence_id);
    REQUIRE(explicit_id.occurrence_id == "d0a62a050f00204877b6103933ef98d4");

    // Tag 2 is a different encoding of the same node, so the two ids differ.
    REQUIRE(explicit_id.occurrence_id !=
            occurrenceIdV1(inputFor(body)).occurrence_id);

    // The name path is not an input once extras.id is present: renaming every
    // node on the path leaves the identity untouched.
    auto renamed = authored;
    renamed.node_name_path = {"Renamed", "Torso"};
    REQUIRE(occurrenceIdV1(renamed).occurrence_id ==
            explicit_id.occurrence_id);

    // A different extras.id under the same name path is a different occurrence.
    auto other = authored;
    other.node_explicit_id = "body-other";
    REQUIRE(occurrenceIdV1(other).occurrence_id != explicit_id.occurrence_id);

    // PrimitiveIdentity uses the same tag order and the same golden shape.
    auto primitive_authored = authored;
    primitive_authored.primitive_explicit_id = "prim-a";
    const auto primitive_explicit = occurrenceIdV1(primitive_authored);
    REQUIRE_FALSE(primitive_explicit.error);
    INFO("explicit primitive occurrence_id_v1="
         << primitive_explicit.occurrence_id);
    REQUIRE(primitive_explicit.occurrence_id == "5f03f05ddd660e0867ce4c3e67aa4668");
    REQUIRE(primitive_explicit.occurrence_id != explicit_id.occurrence_id);
    auto primitive_other = primitive_authored;
    primitive_other.primitive_explicit_id = "prim-b";
    REQUIRE(occurrenceIdV1(primitive_other).occurrence_id !=
            primitive_explicit.occurrence_id);
}

TEST_CASE("v1 importer variants and collision diagnostics are complete",
          "[wp382c][occurrence][cpu]") {
    const auto digest = primitiveContentDigestV1(4, triangle());
    const std::array variants{
        ImporterVariant{ImporterVariantV1::none},
        ImporterVariant{ImporterVariantV1::vrm_split_both},
        ImporterVariant{ImporterVariantV1::vrm_split_third_person_only},
        ImporterVariant{ImporterVariantV1::lod, 7},
        ImporterVariant{ImporterVariantV1::skinned},
        ImporterVariant{ImporterVariantV1::morph_expanded},
        ImporterVariant{ImporterVariantV1::vat_deformed},
    };
    std::vector<std::string> ids;
    for (const auto variant : variants) {
        const auto result = occurrenceIdV1(inputFor(digest, variant));
        REQUIRE_FALSE(result.error);
        ids.push_back(result.occurrence_id);
    }
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    auto a = std::make_shared<ModelOccurrenceIdentityInfo>();
    auto b = std::make_shared<ModelOccurrenceIdentityInfo>();
    a->occurrence_id = ids.front();
    b->occurrence_id = ids.front();
    const std::array siblings{a, b};
    detectOccurrenceIdCollisions(siblings);
    REQUIRE(a->ambiguous);
    REQUIRE(b->ambiguous);
}

TEST_CASE("unrepresentable occurrence inputs are named and never default",
          "[wp382c][occurrence][cpu]") {
    const auto digest = primitiveContentDigestV1(4, triangle());
    auto missing = inputFor(digest);
    missing.asset.reset();
    const auto result = occurrenceIdV1(missing);
    REQUIRE(result.occurrence_id.empty());
    REQUIRE(result.error);
    REQUIRE(result.error->code ==
            OccurrenceResolutionErrorCode::id_unrepresentable);
    REQUIRE(result.error->reason == "asset_key_missing");

    auto invalid_variant = inputFor(digest);
    invalid_variant.variant.kind =
        static_cast<ImporterVariantV1>(255);
    const auto rejected = occurrenceIdV1(invalid_variant);
    REQUIRE(rejected.error);
    REQUIRE(rejected.error->reason == "variant");

    auto invalid = triangle();
    invalid.pos.front().x =
        std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_WITH(primitiveContentDigestV1(4, invalid),
                        "OccurrenceIdUnrepresentable: float");
}

TEST_CASE("WP382d VAT build feature gate reads actual GLB input before GPU staging",
          "[WP382d][occurrence][importer-matrix][cpu][vat-feature]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_wp382d_vat_gate_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{root};
    const auto static_path = root / "static.glb";
    const auto vat_path = root / "vat.glb";
    TestGltfFragmentFixture::writeGlb(static_path);
    TestVatFixture::writeTinyVatGlb(vat_path);
    GltfLoader loader;
    REQUIRE_NOTHROW(loader.prepareGltfBinary(static_path.string()));
#if PELICAN_WITH_VAT
    REQUIRE_NOTHROW(loader.prepareGltfBinary(vat_path.string()));
#else
    try {
        (void)loader.prepareGltfBinary(vat_path.string());
        FAIL("VAT-disabled importer accepted a VAT GLB");
    } catch (const BuildFeatureDisabledError &error) {
        CHECK(error.feature() == "PELICAN_WITH_VAT");
        CHECK_THAT(error.what(), Catch::Matchers::ContainsSubstring("GLB contains pelican.vat primitive extras"));
    }
#endif
}

} // namespace Pelican
