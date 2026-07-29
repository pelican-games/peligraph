#include "shaderresourceinterface.hpp"

#include "../loader/engineresources.hpp"
#include "../../project/targetrenderplanning.hpp"
#include "pelican_sets.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace Pelican {

namespace {

struct StorageImageFormat {
    std::string_view qualifier;
    std::string_view value_type;
};

StorageImageFormat storageImageFormat(
    vk::Format format, const ShaderResourcePortDefinition &port) {
    switch (format) {
    case vk::Format::eR8Unorm:
        return {"r8", "vec4"};
    case vk::Format::eR8Snorm:
        return {"r8_snorm", "vec4"};
    case vk::Format::eR8Uint:
        return {"r8ui", "uvec4"};
    case vk::Format::eR8Sint:
        return {"r8i", "ivec4"};
    case vk::Format::eR8G8Unorm:
        return {"rg8", "vec4"};
    case vk::Format::eR8G8Snorm:
        return {"rg8_snorm", "vec4"};
    case vk::Format::eR8G8Uint:
        return {"rg8ui", "uvec4"};
    case vk::Format::eR8G8Sint:
        return {"rg8i", "ivec4"};
    case vk::Format::eR8G8B8A8Unorm:
        return {"rgba8", "vec4"};
    case vk::Format::eR8G8B8A8Snorm:
        return {"rgba8_snorm", "vec4"};
    case vk::Format::eR8G8B8A8Uint:
        return {"rgba8ui", "uvec4"};
    case vk::Format::eR8G8B8A8Sint:
        return {"rgba8i", "ivec4"};
    case vk::Format::eR16Unorm:
        return {"r16", "vec4"};
    case vk::Format::eR16Snorm:
        return {"r16_snorm", "vec4"};
    case vk::Format::eR16Uint:
        return {"r16ui", "uvec4"};
    case vk::Format::eR16Sint:
        return {"r16i", "ivec4"};
    case vk::Format::eR16Sfloat:
        return {"r16f", "vec4"};
    case vk::Format::eR16G16Unorm:
        return {"rg16", "vec4"};
    case vk::Format::eR16G16Snorm:
        return {"rg16_snorm", "vec4"};
    case vk::Format::eR16G16Uint:
        return {"rg16ui", "uvec4"};
    case vk::Format::eR16G16Sint:
        return {"rg16i", "ivec4"};
    case vk::Format::eR16G16Sfloat:
        return {"rg16f", "vec4"};
    case vk::Format::eR16G16B16A16Unorm:
        return {"rgba16", "vec4"};
    case vk::Format::eR16G16B16A16Snorm:
        return {"rgba16_snorm", "vec4"};
    case vk::Format::eR16G16B16A16Uint:
        return {"rgba16ui", "uvec4"};
    case vk::Format::eR16G16B16A16Sint:
        return {"rgba16i", "ivec4"};
    case vk::Format::eR16G16B16A16Sfloat:
        return {"rgba16f", "vec4"};
    case vk::Format::eR32Uint:
        return {"r32ui", "uvec4"};
    case vk::Format::eR32Sint:
        return {"r32i", "ivec4"};
    case vk::Format::eR32Sfloat:
        return {"r32f", "vec4"};
    case vk::Format::eR32G32Uint:
        return {"rg32ui", "uvec4"};
    case vk::Format::eR32G32Sint:
        return {"rg32i", "ivec4"};
    case vk::Format::eR32G32Sfloat:
        return {"rg32f", "vec4"};
    case vk::Format::eR32G32B32A32Uint:
        return {"rgba32ui", "uvec4"};
    case vk::Format::eR32G32B32A32Sint:
        return {"rgba32i", "ivec4"};
    case vk::Format::eR32G32B32A32Sfloat:
        return {"rgba32f", "vec4"};
    default:
        throw std::runtime_error(
            "Shader resource port '" + port.name +
            "' (resource '" + port.resource +
            "') has no generated storage-image declaration for format " +
            vk::to_string(format));
    }
}

std::string descriptorName(
    ShaderResourceDescriptorKind kind) {
    switch (kind) {
    case ShaderResourceDescriptorKind::
        combined_image_sampler:
        return "combined image sampler";
    case ShaderResourceDescriptorKind::
        input_attachment:
        return "input attachment";
    case ShaderResourceDescriptorKind::storage_image:
        return "storage image";
    case ShaderResourceDescriptorKind::storage_buffer:
        return "storage buffer";
    }
    throw std::runtime_error(
        "unknown shader resource descriptor kind");
}

vk::DescriptorType descriptorType(
    ShaderResourceDescriptorKind kind) {
    switch (kind) {
    case ShaderResourceDescriptorKind::
        combined_image_sampler:
        return vk::DescriptorType::
            eCombinedImageSampler;
    case ShaderResourceDescriptorKind::
        input_attachment:
        return vk::DescriptorType::
            eInputAttachment;
    case ShaderResourceDescriptorKind::storage_image:
        return vk::DescriptorType::eStorageImage;
    case ShaderResourceDescriptorKind::storage_buffer:
        return vk::DescriptorType::eStorageBuffer;
    }
    throw std::runtime_error(
        "unknown shader resource descriptor kind");
}

std::string imageType(
    ShaderResourceDescriptorKind descriptor,
    ReflectedImageViewDimension dimension) {
    const auto sampled =
        descriptor ==
        ShaderResourceDescriptorKind::
            combined_image_sampler;
    if (dimension ==
        ReflectedImageViewDimension::two_d) {
        return sampled ? "sampler2D" : "image2D";
    }
    if (dimension ==
        ReflectedImageViewDimension::two_d_array) {
        return sampled ? "sampler2DArray"
                       : "image2DArray";
    }
    throw std::runtime_error(
        "generated shader resource ports support only 2D and "
        "2D-array image views");
}

void writeImageSubresourceAccessors(
    std::ostringstream &stream,
    const ShaderResourceInterfaceBinding &binding) {
    const auto base_mip =
        binding.port.subresource
            ? binding.port.subresource
                  ->base_mip_level
            : 0u;
    const auto base_layer =
        binding.port.subresource
            ? binding.port.subresource
                  ->base_array_layer
            : 0u;
    stream << "uint pelican_base_mip_"
           << binding.port.name
           << "() { return " << base_mip
           << "u; }\n"
           << "uint pelican_base_layer_"
           << binding.port.name
           << "() { return " << base_layer
           << "u; }\n";
}

void writeInputAttachmentAccessors(
    std::ostringstream &stream,
    const ShaderResourceInterfaceBinding &binding,
    std::string_view variable) {
    const auto &name = binding.port.name;
    stream
        << "vec4 pelican_sample_" << name
        << "(vec2 uv) { return subpassLoad(" << variable
        << "); }\n"
        << "vec4 pelican_sample_" << name
        << "(vec2 uv, uint view_index) { return subpassLoad("
        << variable << "); }\n"
        << "vec4 pelican_sample_lod_" << name
        << "(vec2 uv, float lod) { return subpassLoad("
        << variable << "); }\n"
        << "vec4 pelican_sample_lod_" << name
        << "(vec2 uv, uint view_index, float lod) { return subpassLoad("
        << variable << "); }\n"
        << "ivec2 pelican_size_" << name
        << "() { return ivec2(pelicanResolution.render_resolution.xy); }\n"
        << "ivec2 pelican_size_lod_" << name
        << "(int lod) { return ivec2(pelicanResolution.render_resolution.xy); }\n"
        << "uint pelican_mip_count_" << name
        << "() { return 1u; }\n"
        << "uint pelican_view_count_" << name
        << "() { return pelican_view_count(); }\n";
}

void writeSampledAccessors(
    std::ostringstream &stream,
    const ShaderResourceInterfaceBinding &binding,
    std::string_view variable) {
    const auto &name = binding.port.name;
    if (binding.image_view_dimension ==
        ReflectedImageViewDimension::two_d) {
        stream
            << "vec4 pelican_sample_" << name
            << "(vec2 uv) { return texture(" << variable
            << ", uv); }\n"
            << "vec4 pelican_sample_" << name
            << "(vec2 uv, uint view_index) { return texture(" << variable
            << ", uv); }\n"
            << "vec4 pelican_sample_lod_" << name
            << "(vec2 uv, float lod) { return textureLod("
            << variable << ", uv, lod); }\n"
            << "vec4 pelican_sample_lod_" << name
            << "(vec2 uv, uint view_index, float lod) { return textureLod("
            << variable << ", uv, lod); }\n"
            << "ivec2 pelican_size_" << name
            << "() { return textureSize(" << variable
            << ", 0); }\n"
            << "ivec2 pelican_size_lod_" << name
            << "(int lod) { return textureSize(" << variable
            << ", lod); }\n"
            << "uint pelican_mip_count_" << name
            << "() { return uint(textureQueryLevels(" << variable
            << ")); }\n"
            << "uint pelican_view_count_" << name
            << "() { return 1u; }\n";
        return;
    }
    stream
        << "vec4 pelican_sample_" << name
        << "(vec2 uv, uint view_index) { return texture("
        << variable
        << ", vec3(uv, float(view_index))); }\n"
        << "vec4 pelican_sample_lod_" << name
        << "(vec2 uv, uint view_index, float lod) { return textureLod("
        << variable
        << ", vec3(uv, float(view_index)), lod); }\n"
        << "ivec2 pelican_size_" << name
        << "() { return textureSize(" << variable
        << ", 0).xy; }\n"
        << "ivec2 pelican_size_lod_" << name
        << "(int lod) { return textureSize(" << variable
        << ", lod).xy; }\n"
        << "uint pelican_mip_count_" << name
        << "() { return uint(textureQueryLevels(" << variable
        << ")); }\n"
        << "uint pelican_view_count_" << name
        << "() { return uint(textureSize(" << variable
        << ", 0).z); }\n";
}

void writeStorageAccessors(
    std::ostringstream &stream,
    const ShaderResourceInterfaceBinding &binding,
    std::string_view variable,
    std::string_view value_type) {
    const auto &name = binding.port.name;
    const auto array =
        binding.image_view_dimension ==
        ReflectedImageViewDimension::two_d_array;
    if (binding.readable) {
        stream << value_type << " pelican_load_" << name
               << "(" << (array
                                ? "ivec2 coordinate, uint view_index"
                                : "ivec2 coordinate")
               << ") { return imageLoad(" << variable << ", "
               << (array
                       ? "ivec3(coordinate, int(view_index))"
                       : "coordinate")
               << "); }\n";
        if (!array) {
            stream << value_type << " pelican_load_" << name
                   << "(ivec2 coordinate, uint view_index) { return imageLoad("
                   << variable << ", coordinate); }\n";
        }
    }
    if (binding.writable) {
        stream << "void pelican_store_" << name
               << "(" << (array
                                ? "ivec2 coordinate, uint view_index, "
                                : "ivec2 coordinate, ")
               << value_type << " value) { imageStore("
               << variable << ", "
               << (array
                       ? "ivec3(coordinate, int(view_index))"
                       : "coordinate")
               << ", value); }\n";
        if (!array) {
            stream << "void pelican_store_" << name
                   << "(ivec2 coordinate, uint view_index, "
                   << value_type << " value) { imageStore("
                   << variable << ", coordinate, value); }\n";
        }
    }
    stream << "ivec2 pelican_size_" << name
           << "() { return imageSize(" << variable
           << ").xy; }\n";
    if (array) {
        stream << "uint pelican_view_count_" << name
               << "() { return uint(imageSize(" << variable
               << ").z); }\n";
    } else {
        stream << "uint pelican_view_count_" << name
               << "() { return 1u; }\n";
    }
}

void writeBufferAccessors(
    std::ostringstream &stream,
    const ShaderResourceInterfaceBinding &binding,
    std::string_view variable) {
    const auto &name = binding.port.name;
    const auto element =
        shaderResourceBufferElementName(
            binding.buffer_element);
    if (binding.readable) {
        stream << element << " pelican_load_" << name
               << "(uint index) { return " << variable
               << ".values[index]; }\n";
    }
    if (binding.writable) {
        stream << "void pelican_store_" << name
               << "(uint index, " << element
               << " value) { " << variable
               << ".values[index] = value; }\n";
    }
    stream << "uint pelican_count_" << name
           << "() { return uint(" << variable
           << ".values.length()); }\n";
}

bool writeStageGuardBegin(
    std::ostringstream &stream,
    vk::ShaderStageFlags stages) {
    const auto vertex =
        vk::ShaderStageFlagBits::eVertex;
    const auto fragment =
        vk::ShaderStageFlagBits::eFragment;
    if (stages == vertex) {
        stream
            << "#if defined(PELICAN_SURFACE_STAGE_VERTEX)\n";
        return true;
    }
    if (stages == fragment) {
        stream
            << "#if defined(PELICAN_SURFACE_STAGE_FRAGMENT)\n";
        return true;
    }
    if (stages == (vertex | fragment)) {
        stream
            << "#if defined(PELICAN_SURFACE_STAGE_VERTEX) || "
               "defined(PELICAN_SURFACE_STAGE_FRAGMENT)\n";
        return true;
    }
    return false;
}

bool singleSurfaceStage(
    vk::ShaderStageFlags stages) {
    return stages == vk::ShaderStageFlagBits::eVertex ||
           stages == vk::ShaderStageFlagBits::eFragment;
}

void writeInactiveStageAccessors(
    std::ostringstream &stream,
    const ShaderResourceInterfaceBinding &binding) {
    const auto &name = binding.port.name;
    if (binding.descriptor ==
        ShaderResourceDescriptorKind::storage_buffer) {
        const auto element =
            shaderResourceBufferElementName(
                binding.buffer_element);
        if (binding.readable) {
            stream << element << " pelican_load_" << name
                   << "(uint index) { return " << element
                   << "(0); }\n";
        }
        stream << "uint pelican_count_" << name
               << "() { return 0u; }\n";
        return;
    }
    if (binding.descriptor ==
        ShaderResourceDescriptorKind::
            combined_image_sampler) {
        if (binding.image_view_dimension ==
            ReflectedImageViewDimension::two_d) {
            stream << "vec4 pelican_sample_" << name
                   << "(vec2 uv) { return vec4(0.0); }\n"
                   << "vec4 pelican_sample_" << name
                   << "(vec2 uv, uint view_index) { return vec4(0.0); }\n"
                   << "vec4 pelican_sample_lod_" << name
                   << "(vec2 uv, float lod) { return vec4(0.0); }\n"
                   << "vec4 pelican_sample_lod_" << name
                   << "(vec2 uv, uint view_index, float lod) { return vec4(0.0); }\n"
                   << "uint pelican_view_count_" << name
                   << "() { return 1u; }\n";
        } else {
            stream << "vec4 pelican_sample_" << name
                   << "(vec2 uv, uint view_index) { return vec4(0.0); }\n"
                   << "vec4 pelican_sample_lod_" << name
                   << "(vec2 uv, uint view_index, float lod) { return vec4(0.0); }\n"
                   << "uint pelican_view_count_" << name
                   << "() { return 0u; }\n";
        }
        stream << "ivec2 pelican_size_" << name
               << "() { return ivec2(0); }\n"
               << "ivec2 pelican_size_lod_" << name
               << "(int lod) { return ivec2(0); }\n"
               << "uint pelican_mip_count_" << name
               << "() { return 0u; }\n";
        writeImageSubresourceAccessors(
            stream, binding);
        return;
    }
    if (binding.descriptor ==
        ShaderResourceDescriptorKind::
            input_attachment) {
        stream << "vec4 pelican_sample_" << name
               << "(vec2 uv) { return vec4(0.0); }\n"
               << "vec4 pelican_sample_" << name
               << "(vec2 uv, uint view_index) { return vec4(0.0); }\n"
               << "vec4 pelican_sample_lod_" << name
               << "(vec2 uv, float lod) { return vec4(0.0); }\n"
               << "vec4 pelican_sample_lod_" << name
               << "(vec2 uv, uint view_index, float lod) { return vec4(0.0); }\n"
               << "ivec2 pelican_size_" << name
               << "() { return ivec2(0); }\n"
               << "ivec2 pelican_size_lod_" << name
               << "(int lod) { return ivec2(0); }\n"
               << "uint pelican_mip_count_" << name
               << "() { return 1u; }\n"
               << "uint pelican_view_count_" << name
               << "() { return 0u; }\n";
        writeImageSubresourceAccessors(
            stream, binding);
        return;
    }
    const auto value_type =
        storageImageFormat(
            binding.storage_format, binding.port)
            .value_type;
    const auto array =
        binding.image_view_dimension ==
        ReflectedImageViewDimension::two_d_array;
    if (binding.readable) {
        stream << value_type << " pelican_load_" << name
               << "("
               << (array
                       ? "ivec2 coordinate, uint view_index"
                       : "ivec2 coordinate")
               << ") { return " << value_type
               << "(0); }\n";
        if (!array) {
            stream << value_type << " pelican_load_" << name
                   << "(ivec2 coordinate, uint view_index) { return "
                   << value_type << "(0); }\n";
        }
    }
    stream << "ivec2 pelican_size_" << name
           << "() { return ivec2(0); }\n";
    if (array) {
        stream << "uint pelican_view_count_" << name
               << "() { return 0u; }\n";
    } else {
        stream << "uint pelican_view_count_" << name
               << "() { return 1u; }\n";
    }
    writeImageSubresourceAccessors(
        stream, binding);
}

} // namespace

ReflectedImageViewDimension
resolveShaderResourceImageViewDimension(
    const ShaderResourcePortDefinition &port,
    VulkanResourceViewLayout physical_view,
    ShaderResourceConsumerView consumer) {
    const auto prefix =
        "Shader resource port '" + port.name +
        "' (resource '" + port.resource + "')";
    if (port.view ==
        ShaderResourcePortView::shared_2d) {
        if (physical_view !=
            VulkanResourceViewLayout::shared_2d) {
            throw std::runtime_error(
                prefix +
                " requires shared_2d, but the physical target plan selected " +
                std::string{
                    vulkanResourceViewLayoutName(
                        physical_view)});
        }
        return ReflectedImageViewDimension::two_d;
    }
    if (port.view ==
        ShaderResourcePortView::family_array) {
        if (physical_view !=
            VulkanResourceViewLayout::
                family_2d_array) {
            throw std::runtime_error(
                prefix +
                " requires family_array, but the physical target plan selected " +
                std::string{
                    vulkanResourceViewLayoutName(
                        physical_view)});
        }
        return ReflectedImageViewDimension::
            two_d_array;
    }
    if (physical_view ==
        VulkanResourceViewLayout::
            family_2d_array) {
        throw std::runtime_error(
            prefix +
            " must declare family_array to consume a producer-owned "
            "view-family array");
    }

    if (physical_view ==
        VulkanResourceViewLayout::shared_2d) {
        // A mono graph is the scalar physical representation of a per-view
        // contract. This keeps one authored shader valid for flat and XR
        // variants; stereo compute/multiview consumers still receive an
        // indexed array view below.
        return ReflectedImageViewDimension::two_d;
    }
    if (consumer ==
            ShaderResourceConsumerView::graphics_multiview &&
        physical_view ==
            VulkanResourceViewLayout::sequential_2d) {
        throw std::runtime_error(
            prefix +
            " cannot feed a multiview shader from a sequential_2d "
                "physical view");
    }
    if (consumer ==
            ShaderResourceConsumerView::
                compute_per_view &&
        physical_view ==
            VulkanResourceViewLayout::
                sequential_2d) {
        // Sequential per-view compute runs inside the selected logical view
        // and the physical image is intentionally reused between eyes.
        return ReflectedImageViewDimension::two_d;
    }
    if (consumer ==
            ShaderResourceConsumerView::graphics_sequential) {
        return ReflectedImageViewDimension::two_d;
    }
    return ReflectedImageViewDimension::two_d_array;
}

std::string generateShaderResourcePortInclude(
    std::span<const ShaderResourceInterfaceBinding> bindings) {
    std::ostringstream stream;
    stream
        << "#ifndef PELICAN_RESOURCE_PORTS_GLSL\n"
        << "#define PELICAN_RESOURCE_PORTS_GLSL\n"
        << "#include \"pelican_sets.glsl\"\n";
    if (std::any_of(
            bindings.begin(), bindings.end(),
            [](const auto &binding) {
                return binding.descriptor ==
                       ShaderResourceDescriptorKind::
                           input_attachment;
            })) {
        stream << "#include \"pelican_frame.glsl\"\n";
    }
    for (const auto &binding : bindings) {
        const auto guarded =
            writeStageGuardBegin(
                stream, binding.expected_stages);
        const auto variable =
            shaderResourcePortVariableName(
                binding.port.name);
        if (binding.descriptor ==
            ShaderResourceDescriptorKind::
                storage_buffer) {
            stream
                << "layout(std430, set = PELICAN_SET_PASS_INPUT, binding = "
                << binding.binding << ") ";
            if (binding.readable && !binding.writable) {
                stream << "readonly ";
            } else if (!binding.readable &&
                       binding.writable) {
                stream << "writeonly ";
            }
            stream << "buffer PelicanResource_"
                   << binding.port.name
                   << "_Block { "
                   << shaderResourceBufferElementName(
                          binding.buffer_element)
                   << " values[]; } " << variable
                   << ";\n";
            writeBufferAccessors(
                stream, binding, variable);
            if (guarded) {
                if (singleSurfaceStage(
                        binding.expected_stages)) {
                    stream << "#else\n";
                    writeInactiveStageAccessors(
                        stream, binding);
                }
                stream << "#endif\n";
            }
            continue;
        }
        if (binding.descriptor ==
            ShaderResourceDescriptorKind::
                combined_image_sampler) {
            stream
                << "layout(set = PELICAN_SET_PASS_INPUT, binding = "
                << binding.binding << ") uniform "
                << imageType(
                       binding.descriptor,
                       binding.image_view_dimension)
                << " " << variable << ";\n";
            writeSampledAccessors(
                stream, binding, variable);
            writeImageSubresourceAccessors(
                stream, binding);
            if (guarded) {
                if (singleSurfaceStage(
                        binding.expected_stages)) {
                    stream << "#else\n";
                    writeInactiveStageAccessors(
                        stream, binding);
                }
                stream << "#endif\n";
            }
            continue;
        }
        if (binding.descriptor ==
            ShaderResourceDescriptorKind::
                input_attachment) {
            if (binding.expected_stages !=
                vk::ShaderStageFlagBits::eFragment) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    binding.port.name +
                    "' input attachment is fragment-only");
            }
            if (!binding.input_attachment_index) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    binding.port.name +
                    "' input attachment has no physical index");
            }
            if (binding.image_view_dimension !=
                ReflectedImageViewDimension::two_d) {
                throw std::runtime_error(
                    "Shader resource port '" +
                    binding.port.name +
                    "' input attachment requires a 2D shader view");
            }
            stream
                << "layout(input_attachment_index = "
                << *binding.input_attachment_index
                << ", set = PELICAN_SET_PASS_INPUT, binding = "
                << binding.binding
                << ") uniform subpassInput "
                << variable << ";\n";
            writeInputAttachmentAccessors(
                stream, binding, variable);
            writeImageSubresourceAccessors(
                stream, binding);
            if (guarded) {
                if (singleSurfaceStage(
                        binding.expected_stages)) {
                    stream << "#else\n";
                    writeInactiveStageAccessors(
                        stream, binding);
                }
                stream << "#endif\n";
            }
            continue;
        }

        const auto format =
            storageImageFormat(
                binding.storage_format, binding.port);
        stream << "layout(" << format.qualifier
               << ", set = PELICAN_SET_PASS_INPUT, binding = "
               << binding.binding << ") ";
        if (binding.readable && !binding.writable) {
            stream << "readonly ";
        } else if (!binding.readable &&
                   binding.writable) {
            stream << "writeonly ";
        }
        stream << "uniform "
               << imageType(
                      binding.descriptor,
                      binding.image_view_dimension)
               << " " << variable << ";\n";
        writeStorageAccessors(
            stream, binding, variable,
            format.value_type);
        writeImageSubresourceAccessors(
            stream, binding);
        if (guarded) {
            if (singleSurfaceStage(
                    binding.expected_stages)) {
                stream << "#else\n";
                writeInactiveStageAccessors(
                    stream, binding);
            }
            stream << "#endif\n";
        }
    }
    stream << "#endif\n";
    return stream.str();
}

std::pair<std::string, std::string>
makeShaderResourcePortVirtualInclude(
    std::span<const ShaderResourceInterfaceBinding> bindings) {
    return {
        std::string{shaderResourcePortIncludeName},
        generateShaderResourcePortInclude(bindings),
    };
}

std::vector<std::pair<std::string, std::string>>
makeShaderResourcePortVirtualIncludes(
    std::span<const ShaderResourceInterfaceBinding> bindings) {
    std::vector<std::pair<std::string, std::string>>
        result;
    result.reserve(4);
    result.push_back(
        makeShaderResourcePortVirtualInclude(
            bindings));
    for (const auto name :
         {"pelican_sets.glsl",
          "pelican_view.glsl",
          "pelican_frame.glsl"}) {
        result.emplace_back(
            name,
            engineResourceOrThrow(
                "shaders/include/" +
                std::string{name}));
    }
    return result;
}

void validateShaderResourceInterfaceReflection(
    std::span<const ShaderResourceInterfaceBinding> bindings,
    const ShaderReflection &reflection,
    std::uint32_t expected_set) {
    for (const auto &expected : bindings) {
        const auto found = std::find_if(
            reflection.bindings.begin(),
            reflection.bindings.end(),
            [&](const ReflectedBinding &candidate) {
                return candidate.set == expected_set &&
                       candidate.binding ==
                           expected.binding;
            });
        const auto prefix =
            "Shader resource port '" +
            expected.port.name + "' (resource '" +
            expected.port.resource + "')";
        if (found == reflection.bindings.end()) {
            throw std::runtime_error(
                prefix +
                " is absent from shader reflection; include <" +
                std::string{shaderResourcePortIncludeName} +
                "> and use its generated accessor");
        }
        const auto expected_type =
            descriptorType(expected.descriptor);
        if (found->type != expected_type ||
            found->count != 1) {
            throw std::runtime_error(
                prefix + " requires " +
                descriptorName(expected.descriptor) +
                ", reflected " +
                vk::to_string(found->type));
        }
        const auto expected_name =
            shaderResourcePortVariableName(
                expected.port.name);
        if (found->name != expected_name) {
            throw std::runtime_error(
                prefix + " reflected unexpected descriptor '" +
                found->name + "' at binding " +
                std::to_string(expected.binding));
        }
        // SPIR-V reflects subpassInput as DimSubpassData rather than as the
        // concrete 2D/2D-array image view bound by the runtime. The physical
        // view policy is validated when the material descriptor is built.
        if (expected.descriptor !=
                ShaderResourceDescriptorKind::
                    input_attachment &&
            found->image_view_dimension !=
                expected.image_view_dimension) {
            throw std::runtime_error(
                prefix + " requires " +
                std::string{
                    reflectedImageViewDimensionName(
                        expected.image_view_dimension)} +
                " view, reflected " +
                std::string{
                    reflectedImageViewDimensionName(
                        found->image_view_dimension)});
        }
        if (expected.descriptor ==
            ShaderResourceDescriptorKind::
                input_attachment) {
            if (!expected.input_attachment_index) {
                throw std::logic_error(
                    prefix +
                    " has no compiler-owned input-attachment index");
            }
            if (found->input_attachment_index !=
                expected.input_attachment_index) {
                throw std::runtime_error(
                    prefix +
                    " reflected input-attachment index " +
                    (found->input_attachment_index
                         ? std::to_string(
                               *found->input_attachment_index)
                         : std::string{"<missing>"}) +
                    ", expected " +
                    std::to_string(
                        *expected.input_attachment_index));
            }
        }
        if (expected.expected_stages &&
            found->stages != expected.expected_stages) {
            throw std::runtime_error(
                prefix +
                " stage visibility does not match the declared interface");
        }
    }
}

} // namespace Pelican
