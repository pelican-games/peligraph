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
    case vk::Format::eR16G16Sfloat:
        return {"rg16f", "vec4"};
    case vk::Format::eR8G8B8A8Unorm:
        return {"rgba8", "vec4"};
    case vk::Format::eR16G16B16A16Sfloat:
        return {"rgba16f", "vec4"};
    case vk::Format::eR32Sfloat:
        return {"r32f", "vec4"};
    case vk::Format::eR32G32Sfloat:
        return {"rg32f", "vec4"};
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
    case ShaderResourceDescriptorKind::storage_image:
        return "storage image";
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
    case ShaderResourceDescriptorKind::storage_image:
        return vk::DescriptorType::eStorageImage;
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
            << "ivec2 pelican_size_" << name
            << "() { return textureSize(" << variable
            << ", 0); }\n";
        return;
    }
    stream
        << "vec4 pelican_sample_" << name
        << "(vec2 uv, uint view_index) { return texture("
        << variable
        << ", vec3(uv, float(view_index))); }\n"
        << "ivec2 pelican_size_" << name
        << "() { return textureSize(" << variable
        << ", 0).xy; }\n"
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
    }
    stream << "ivec2 pelican_size_" << name
           << "() { return imageSize(" << variable
           << ").xy; }\n";
    if (array) {
        stream << "uint pelican_view_count_" << name
               << "() { return uint(imageSize(" << variable
               << ").z); }\n";
    }
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

    if (physical_view ==
        VulkanResourceViewLayout::shared_2d) {
        throw std::runtime_error(
            prefix +
            " requires per_view, but the physical target plan selected "
            "shared_2d");
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
    for (const auto &binding : bindings) {
        const auto variable =
            shaderResourcePortVariableName(
                binding.port.name);
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
        if (found->image_view_dimension !=
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
    }
}

} // namespace Pelican
