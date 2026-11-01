#pragma once
#include "rc7c_device_test_facts.hpp"
#include "../src/project/physicaltargetplanwire.hpp"
#include "../src/core/renderingpass/rendertargetimagecontract.hpp"
#include <nlohmann/json.hpp>
namespace Pelican::TestSupport {
inline nlohmann::json rc7dConfig() {
    using J=nlohmann::json;
    J config={{"render_targets",J::array()},{"rendering_passes",J::array({{{"name","main"},{"passes",J::array()}}})}};
    for(const char *name:{"local_source","local_output","scratch","A","B"})
        config["render_targets"].push_back({{"name",name},{"format","R8G8B8A8_UNORM"},{"format_class","data"},
            {"extent_scale",1.0},{"usage",name==std::string{"scratch"}?J::array({"COLOR_ATTACHMENT"}):J::array({"COLOR_ATTACHMENT","SAMPLED"})}});
    auto &passes=config["rendering_passes"][0]["passes"];
    const auto add=[&](const char *name,const char *out,const char *in=nullptr,bool local=false) {
        J p={{"name",name},{"type","fullscreen"},{"output",{{"color",out},{"depth",nullptr}}},
            {"shader",{{"vertex","engine://fullscreen"},{"fragment","engine://passthrough"}}}};
        if(in) p["input"]={in};
        if(local) {p["input_footprints"][in]="same_pixel";p["resource_ports"]["input_color"]["resource"]=in;}
        if(!passes.empty())p["after"]={passes.back().at("name")};
        passes.push_back(p);
    };
    add("local_producer","local_source");add("local_consumer","local_output","local_source",true);
    add("local_present","swapchain","local_output");add("scratch_write","scratch");
    add("write_A","A");add("read_A","swapchain","A");add("write_B","B");add("read_B","swapchain","B");
    return config;
}
inline RenderingTargetPlanDeviceFacts rc7dFacts(int mode) {
    auto f=rc7cDeviceFacts();
    f.transient_attachments=true;f.dynamic_rendering_local_read=true;
    f.memory_types={{vk::MemoryPropertyFlagBits::eDeviceLocal,0}};
    if(mode==1)f.memory_types.push_back({vk::MemoryPropertyFlagBits::eDeviceLocal|vk::MemoryPropertyFlagBits::eLazilyAllocated,0});
    if(mode==2)f.memory_types.clear();
    f.query_image_format_capability=[](const auto &){return RenderingImageFormatCapability{true,{1},16,16,false,true,true};};
    f.image_memory_requirements=[mode](const vk::ImageCreateInfo &info) {
        auto q=rc7cDeviceRequirements(info);
        q->memory_type_bits=mode==1 && bool(info.usage & vk::ImageUsageFlagBits::eTransientAttachment)?3u:1u;
        return q;
    };
    return f;
}
}
