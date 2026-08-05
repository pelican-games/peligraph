#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

struct FramePlanAttachmentOps {
    std::string resource;
    std::string aspect;
    std::string load_op;
    std::string store_op;

    bool operator==(const FramePlanAttachmentOps &) const = default;
};

struct FramePlanResourceUse {
    std::string resource;
    std::string epoch;
    std::string access;
    std::string intent;
    std::string footprint;

    bool operator==(const FramePlanResourceUse &) const = default;
};

struct FramePlanMaterialFilter {
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    std::string filter_id;
    std::string resolution_state;
    std::string resolution_provenance;
    std::optional<std::size_t> resolved_draw_count;

    bool operator==(const FramePlanMaterialFilter &) const = default;
};

struct FramePlanNode {
    std::string name;
    std::string kind;
    std::size_t declaration_index = 0;
    std::size_t order = 0;
    std::size_t level = 0;
    std::vector<std::string> reads;
    std::vector<std::string> history_reads;
    std::vector<std::string> writes;
    std::string view_family;
    std::string snapshot_after;
    std::size_t byte_size = 0;
    std::optional<FramePlanMaterialFilter> material_filter;
    std::string material_variant;

    // The public response may publish these directly. Current runtime output
    // carries the same high-level facts in physical_target_plan.attachments;
    // the builder normalizes both forms into these fields.
    std::string color_load_op;
    std::string color_store_op;
    std::string depth_load_op;
    std::string depth_store_op;
    std::vector<FramePlanAttachmentOps> attachments;

    // Backend-independent execution-plan facts. Backend-native target-plan
    // details intentionally do not enter this model.
    std::string semantic_dialect;
    std::string selected_implementation;
    std::string selected_endpoint;
    std::vector<std::string> required_capabilities;
    std::vector<FramePlanResourceUse> resource_uses;

    std::vector<std::size_t> incoming_barriers;
    std::vector<std::size_t> outgoing_barriers;

    bool operator==(const FramePlanNode &) const = default;
};

struct FramePlanBarrier {
    std::string kind;
    std::string resource;
    std::string from;
    std::string to;

    bool operator==(const FramePlanBarrier &) const = default;
};

struct FramePlanResource {
    std::string name;
    std::string format = "unknown";
    std::string dimension;
    std::optional<std::size_t> width;
    std::optional<std::size_t> height;
    std::vector<std::string> readers;
    std::vector<std::string> history_readers;
    std::vector<std::string> writers;
    std::string provider_feature;
    std::string provider_reference;
    std::string sampling;
    std::string view_policy;
    std::string fallback;
    std::vector<std::string> material_consumers;
    std::vector<std::string> fullscreen_consumers;

    bool operator==(const FramePlanResource &) const = default;
};

struct FramePlanMaterialRoute {
    std::string route;
    std::string pass;
    std::string contract;
    std::string shader_contract;
    std::string phase;

    bool operator==(const FramePlanMaterialRoute &) const = default;
};

struct FramePlanModel {
    std::string graph;
    std::optional<std::uint64_t> runtime_generation;
    std::size_t response_bytes = 0;
    std::vector<FramePlanNode> nodes;
    std::vector<FramePlanBarrier> barriers;
    std::vector<FramePlanResource> resources;
    std::vector<FramePlanMaterialRoute> material_routes;

    bool operator==(const FramePlanModel &) const = default;
};

// Builds the compact, view-independent projection used by Pelican Studio from
// the result value of get_frame_plan. Only pelican.frame_plan version 1 is
// accepted; old or future versions are not upgraded implicitly.
FramePlanModel buildFramePlanModel(std::string_view response_json);

} // namespace PelicanStudio
