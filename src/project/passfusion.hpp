#pragma once

#include "logicalrendergraph.hpp"
#include "targetplanning.hpp"
#include "targetrenderplanning.hpp"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {

// PF-1 (docs/design_pass_fusion.md §1): the runs of fullscreen passes that
// could execute as one pass, decided from the logical graph's structure
// alone. A run ("chain") is a path of fullscreen passes in which every stage
// after the first reads the previous stage's output at its own pixel
// (LogicalReadFootprintKind::same_pixel, as the graph states it). The plan is
// structural: it does not look at the shaders, so a chain is a candidate the
// stitching stage (PF-2) narrows further. Nothing executes this plan yet.

// What the logical graph does not say about a node. Every node of the graph
// has an entry.
struct PassFusionNodeFacts {
    std::string node;
    // A fullscreen pass (only these can join a chain; material, raster,
    // compute and copy nodes are boundaries).
    bool fullscreen = false;
    // PS-1 `schedule: "once"`: recorded only on the frames it is due, so it
    // never shares a pass with a node that runs every frame.
    bool once = false;
};

// What the logical graph does not say about an image. An image without an
// entry is a boundary (resource_facts_missing) wherever a chain would need it.
struct PassFusionResourceFacts {
    std::string resource;
    // The physical-size rule of the target plan: fixed or output-relative.
    // A `display`-class target is the output image (its fixed size is the
    // output's), so it is output-relative 1 like the swapchain.
    ResourceExtentPlan extent;
    // The render target's format_class (scene, display, data,
    // explicit(<format>)); the swapchain is display.
    std::string format_class;
};

struct PassFusionGraphFacts {
    // The CompiledLogicalRenderGraph::name these facts describe.
    std::string graph;
    std::vector<PassFusionNodeFacts> nodes;
    std::vector<PassFusionResourceFacts> resources;
};

// Why a data edge that touches a fullscreen pass is not a link of a chain.
// The edge is producer -> consumer through one value. The checks run in this
// order and the first that fails names the boundary. The edge's own checks
// come first (not_fullscreen .. extent_mismatch); the rest depend on the
// chain being grown, which starts at the earliest eligible pass in execution
// order and links each stage to its earliest consumer that passes them.
enum class PassFusionBoundaryReason : std::uint8_t {
    // The producer or the consumer is not a fullscreen pass.
    not_fullscreen,
    // The producer or the consumer runs only on the frames it is due.
    scheduled_once,
    // target_planning.graphs.<graph>.nodes.<node>.fuse is false.
    fuse_disabled,
    // The two nodes run on a different view family or view selection.
    view_mismatch,
    // The consumer reads the value around its pixel / anywhere / from
    // another frame. An undeclared sampled read is arbitrary (the logical
    // graph's conservative default).
    read_neighborhood,
    read_arbitrary,
    read_temporal,
    // The consumer reads more than one value the producer writes.
    multiple_chain_inputs,
    // The consumer reads the value in place (a loaded attachment it writes
    // again), and another node reads the same value.
    in_place_read_outside,
    // The extent or format class of the value or of an output of the two
    // nodes is unknown.
    resource_facts_missing,
    // An output of the producer or the consumer has another extent than the
    // value (the fused pass has one render area; a stage that changes
    // resolution ends the chain).
    extent_mismatch,
    // The producer's chain already continues into another consumer.
    fan_out,
    // The consumer is already a stage of a chain (through another producer).
    fan_in,
    // The value has another format class than the value the chain already
    // hands on: one kind of value flows through a chain (§1-2, one value
    // per pixel). The first stage's inputs and the last stage's outputs may
    // be of any class.
    format_class_mismatch,
    // The consumer reads a value an earlier stage (not the previous one)
    // of the chain writes.
    reads_other_member_output,
    // A node outside the chain would have to run between two of its stages
    // (it depends on an earlier stage and the consumer depends on it). The
    // order counts data, after / before (the composed config chains each
    // canonical phase's passes in their declared order, so only consecutive
    // passes fuse) and the reads of an image before a later write of it.
    // Anchors are not nodes here: a pass before a phase boundary and the
    // first pass after it are consecutive.
    outside_dependency,
    // Fusing would make a dependency cycle with a chain already found.
    cycle_with_other_chain,
};

std::string_view passFusionBoundaryReasonName(
    PassFusionBoundaryReason reason);
// Every reason, in the order above.
std::span<const PassFusionBoundaryReason> passFusionBoundaryReasons();

// The value one stage hands the next (it is no longer read from memory).
struct PassFusionChainResource {
    LogicalValueId value;
    std::string producer;
    std::string consumer;
};

// A value an earlier stage writes that the fused pass still writes, as an
// extra color output (§1-3): an outside node reads it, it outlives the
// frame, or it is an output of the stage other than the value it hands on.
struct PassFusionKeptOutput {
    LogicalValueId value;
    std::string producer;
    // The nodes outside the chain that read it, in execution order.
    std::vector<std::string> readers;
    // The last value of the frame of an image that is not virtual (history,
    // external, or kept by the target plan), so it survives the frame.
    bool live_out = false;
    // An outside reader reads it other than at the same pixel, or it is
    // live out: it cannot stay tile-local.
    bool materialized = false;
};

struct PassFusionChain {
    std::string graph;
    // In execution order. Two or more.
    std::vector<std::string> members;
    // members[i] hands chain_resources[i] to members[i + 1].
    std::vector<PassFusionChainResource> chain_resources;
    std::vector<PassFusionKeptOutput> kept_outputs;
    std::size_t stage_count = 0;
    // One per handed value: the next stage's read of it.
    std::size_t eliminated_reads = 0;
    // One per handed value that is not a kept output.
    std::size_t eliminated_writes = 0;
};

struct PassFusionBoundary {
    std::string graph;
    std::string producer;
    std::string consumer;
    LogicalValueId value;
    PassFusionBoundaryReason reason =
        PassFusionBoundaryReason::not_fullscreen;
    std::string detail;
};

struct PassFusionPlan {
    // Graph by graph in input order; within a graph by the first stage's
    // execution position.
    std::vector<PassFusionChain> chains;
    // Every data edge that touches a fullscreen pass and is not a link,
    // graph by graph, by (producer, consumer) execution position and value.
    std::vector<PassFusionBoundary> boundaries;
};

// `facts` has exactly one entry per graph (matched by name). The fuse
// constraints are those of `target_planning` whose graph name is the
// logical graph's; an entry with fuse false must name a node of that graph.
PassFusionPlan planPassFusion(
    std::span<const CompiledLogicalRenderGraph> graphs,
    std::span<const PassFusionGraphFacts> facts,
    const TargetPlanningPolicy &target_planning = {});

struct PassFusionSummary {
    std::size_t chains = 0;
    // Passes that are a stage of some chain.
    std::size_t fused_passes = 0;
    // fused_passes minus chains: the passes fusion removes.
    std::size_t eliminated_passes = 0;
    std::size_t eliminated_reads = 0;
    std::size_t eliminated_writes = 0;
    std::size_t boundaries = 0;
    // The reasons that occur, in passFusionBoundaryReasons() order.
    std::vector<std::pair<PassFusionBoundaryReason, std::size_t>>
        boundary_reasons;
};

PassFusionSummary summarizePassFusionPlan(const PassFusionPlan &plan);

// The plan as get_frame_plan will carry it (`fused_nodes[]`, memo §1):
//   {schema: "pelican.pass_fusion_plan", version: 1, stage: "structural",
//    fused_nodes: [{graph, members, stage_count,
//                   chain_resources: [{resource, version, producer, consumer}],
//                   kept_outputs: [{resource, version, producer, readers,
//                                   live_out, materialized}],
//                   eliminated_reads, eliminated_writes}],
//    boundaries: [{graph, producer, consumer, resource, version, reason,
//                  detail}],
//    summary: {chains, fused_passes, eliminated_passes, eliminated_reads,
//              eliminated_writes, boundaries, boundary_reasons: {reason: n}}}
// `stage: "structural"` says the shaders were not consulted.
nlohmann::ordered_json passFusionPlanToJson(const PassFusionPlan &plan);

} // namespace Pelican
