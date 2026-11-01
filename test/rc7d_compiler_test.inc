#include "rc7d_test_support.hpp"
namespace Pelican {
TEST_CASE("RC-7d D1 D2 compiler derives lazy kind and exact final usage", "[rendercompiler][rc7d][rc7d-d1][rc7d-d2]") {
    // Every image has a distinct (format, extent), so a memory query names its image.
    auto config=TestSupport::rc7dConfig();
    for(auto &t:config["render_targets"]) {
        if(t["name"]=="local_output")t["format"]="R16G16B16A16_SFLOAT";
        if(t["name"]=="scratch")t["extent_scale"]=0.5;
        if(t["name"]=="A")t["extent_scale"]=0.75;
        if(t["name"]=="B")t["extent_scale"]=0.25;
    }
    for(int mode:{0,1,2}) {
        CAPTURE(mode);
        ProgramInputFixture fixture;fixture.useAuthoredConfig(config,"rc7d");
        fixture.backend_context=VulkanRenderCompilerBackendContext{vk::Format::eB8G8R8A8Srgb,{64,64},{},{},{},VulkanRenderCompilerDevicePlanningMode::compiler_only};
        auto facts=TestSupport::rc7dFacts(mode);
        // The planner asks the format capability right before each memory
        // query; an agreement re-queries memory alone.
        struct Query {vk::Format format;vk::Extent3D extent;vk::ImageUsageFlags usage;bool agreement;};
        std::vector<Query> queries;bool capability_asked=false;
        const auto query=facts.image_memory_requirements;
        facts.image_memory_requirements=[&](const auto &ci){
            queries.push_back({ci.format,ci.extent,ci.usage,!capability_asked});capability_asked=false;return query(ci);};
        const auto capability=facts.query_image_format_capability;
        facts.query_image_format_capability=[&](const auto &d){capability_asked=true;return capability(d);};
        fixture.backend_context.planning_facts=facts;
        fixture.variants={{.graph_variant=RenderPipelineGraphVariant::flat}};
        auto input=fixture.input();input.runtime_shader_compiler_enabled=true;
        const auto result=runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(),input);
        // The final agreement: the queries after the last planner query.
        std::vector<Query> agreed;
        for(auto q=queries.rbegin();q!=queries.rend() && q->agreement;++q)agreed.push_back(*q);
        REQUIRE_FALSE(agreed.empty());
        const auto &physical=requireVulkanRenderCompilerPhysicalPackage(*result.variants[0].physical_package);
        const auto &plan=*physical.target_plans.at("main");
        const auto resource=[&](const char *name)->const auto & {return *std::find_if(plan.resources.begin(),plan.resources.end(),[&](const auto &r){return r.logical_resource==name;});};
        REQUIRE(resource("scratch").representation==VulkanResourceRepresentation::transient_attachment);
        REQUIRE(resource("local_source").representation==VulkanResourceRepresentation::tile_local_attachment);
        const auto resident=[&](const char *name){for(const auto &region:plan.memory_regions)for(const auto &r:region.residents)if(r.resource==name)return true;return false;};
        for(const auto *name:{"scratch","local_source"}) {
            CAPTURE(name);
            CHECK(resident(name)==(mode==0));
            const auto *image=result.generation_resources.findImage(name);REQUIRE(image);
            const auto definition=renderTargetDefinitionOf(*image);
            const auto ci=renderTargetImageContract(definition,{64,64},0).createInfo();
            CHECK(bool(ci.usage & vk::ImageUsageFlagBits::eTransientAttachment));
            std::size_t matched=0;
            for(const auto &q:agreed)if(q.format==ci.format && q.extent==ci.extent) {++matched;CHECK(q.usage==ci.usage);}
            CHECK(matched>0);
            if(mode!=0)CHECK(std::any_of(plan.decisions.begin(),plan.decisions.end(),[&](const auto &v){return v.subject==name && v.selected==(mode==1?"lazy_memory":"lazy_kind_unknown");}));
        }
        const auto &ctx=physical.target_plan_compilation.verification_contexts.front();
        REQUIRE_NOTHROW(verifyVulkanCompletePhysicalPlanPackage(*ctx.logical_graph,ctx.topology,plan,ctx.node_order,
            ejectVulkanCompletePhysicalPlanPackage(plan),ctx.format_capabilities));
        const nlohmann::json wire=vulkanTargetPlanToJson(plan);
        CHECK(validatePhysicalTargetPlanWire(&wire).available());
        CHECK(resident("A"));CHECK(resident("B"));CHECK(resident("local_output"));
        if(mode==0) {
            REQUIRE(plan.memory_regions.size()==1);
            const auto &residents=plan.memory_regions[0].residents;
            const auto a=std::find_if(residents.begin(),residents.end(),[](const auto &v){return v.resource=="local_source";});
            const auto b=std::find_if(residents.begin(),residents.end(),[](const auto &v){return v.resource=="local_output";});
            REQUIRE(a!=residents.end());REQUIRE(b!=residents.end());
            CHECK((a->offset+a->size<=b->offset || b->offset+b->size<=a->offset));
        }
        auto required=config;
        required["target_planning"]["graphs"]["main"]["resources"]["scratch"]["alias_group"]={{"value","lazy"},{"strength","require"}};
        required["target_planning"]["graphs"]["main"]["resources"]["A"]["alias_group"]={{"value","lazy"},{"strength","require"}};
        if(mode==1) {
            ProgramInputFixture required_fixture;required_fixture.useAuthoredConfig(required,"rc7d-require");
            required_fixture.backend_context=fixture.backend_context;required_fixture.variants=fixture.variants;
            auto required_input=required_fixture.input();required_input.runtime_shader_compiler_enabled=true;
            CHECK_THROWS_WITH(runRenderCompilerProgram(defaultVulkanRenderCompilerProgram(),required_input),Catch::Matchers::ContainsSubstring("alias.lazy_memory@1"));
        }
    }
}

}
