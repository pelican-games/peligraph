#include "spvlink.hpp"

#include <picosha2.h>
#include <spirv-tools/libspirv.h>
#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/linker.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv/unified1/spirv.h>
#include <spirv_reflect.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace Pelican {
namespace {

constexpr auto target_env = SPV_ENV_VULKAN_1_2;
constexpr std::string_view headers_revision = "09913f088a1197aba4aefd300a876b2ebbaa3391";
constexpr std::string_view tools_revision = "f289d047f49fb60488301ec62bafab85573668cc";
constexpr std::string_view reflect_revision = "c637858562fbce1b6f5dc7ca48d4e8a5bd117b70";

struct Instruction {
    std::vector<std::uint32_t> words;
    std::uint32_t parsed_result_id = 0;

    SpvOp opcode() const { return static_cast<SpvOp>(words.front() & SpvOpCodeMask); }
    std::uint32_t resultId() const {
        if (parsed_result_id != 0) return parsed_result_id;
        switch (opcode()) {
        case SpvOpTypeVoid:
        case SpvOpTypeBool:
        case SpvOpTypeInt:
        case SpvOpTypeFloat:
        case SpvOpTypeVector:
        case SpvOpTypeMatrix:
        case SpvOpTypeImage:
        case SpvOpTypeSampler:
        case SpvOpTypeSampledImage:
        case SpvOpTypeArray:
        case SpvOpTypeRuntimeArray:
        case SpvOpTypeStruct:
        case SpvOpTypeOpaque:
        case SpvOpTypePointer:
        case SpvOpTypeFunction:
            return words.size() > 1 ? words[1] : 0;
        default:
            break;
        }
        if (opcode() == SpvOpFunction || opcode() == SpvOpFunctionParameter) {
            return words.size() > 2 ? words[2] : 0;
        }
        return 0;
    }
};

struct ParsedModule {
    std::vector<std::uint32_t> header;
    std::vector<Instruction> instructions;
};

struct ContextDeleter {
    void operator()(spv_context context) const { spvContextDestroy(context); }
};
using UniqueContext = std::unique_ptr<std::remove_pointer_t<spv_context>, ContextDeleter>;

spv_result_t collectInstruction(void *user_data, const spv_parsed_instruction_t *parsed) {
    auto &instructions = *static_cast<std::vector<Instruction> *>(user_data);
    instructions.push_back({std::vector<std::uint32_t>{parsed->words,
                                                       parsed->words + parsed->num_words},
                            parsed->result_id});
    return SPV_SUCCESS;
}

ParsedModule parseModule(std::span<const std::uint32_t> words, std::string_view label) {
    if (words.size() < 5) {
        throw std::runtime_error(std::string{label} + " is not a SPIR-V module");
    }
    ParsedModule parsed;
    parsed.header.assign(words.begin(), words.begin() + 5);
    UniqueContext context{spvContextCreate(target_env)};
    spv_diagnostic diagnostic = nullptr;
    const auto result = spvBinaryParse(context.get(), &parsed.instructions, words.data(), words.size(),
                                       nullptr, collectInstruction, &diagnostic);
    if (result != SPV_SUCCESS) {
        std::string message = std::string{label} + " is not parseable SPIR-V";
        if (diagnostic != nullptr && diagnostic->error != nullptr) {
            message += ": ";
            message += diagnostic->error;
        }
        spvDiagnosticDestroy(diagnostic);
        throw std::runtime_error(message);
    }
    spvDiagnosticDestroy(diagnostic);
    return parsed;
}

std::vector<std::uint32_t> serialize(const ParsedModule &module) {
    std::size_t size = module.header.size();
    for (const auto &instruction : module.instructions) size += instruction.words.size();
    std::vector<std::uint32_t> words;
    words.reserve(size);
    words.insert(words.end(), module.header.begin(), module.header.end());
    for (const auto &instruction : module.instructions) {
        words.insert(words.end(), instruction.words.begin(), instruction.words.end());
    }
    return words;
}

std::string literalString(const Instruction &instruction, std::size_t first_word) {
    std::string result;
    for (std::size_t i = first_word; i < instruction.words.size(); ++i) {
        const auto word = instruction.words[i];
        for (unsigned byte = 0; byte < 4; ++byte) {
            const auto ch = static_cast<char>((word >> (byte * 8u)) & 0xffu);
            if (ch == '\0') return result;
            result.push_back(ch);
        }
    }
    return result;
}

std::vector<std::uint32_t> encodedString(std::string_view value) {
    std::vector<std::uint32_t> words((value.size() + 1 + 3) / 4, 0);
    for (std::size_t i = 0; i < value.size(); ++i) {
        words[i / 4] |= static_cast<std::uint32_t>(static_cast<unsigned char>(value[i])) <<
                        ((i % 4) * 8u);
    }
    return words;
}

Instruction makeInstruction(SpvOp opcode, std::vector<std::uint32_t> operands) {
    Instruction instruction;
    instruction.words.reserve(operands.size() + 1);
    instruction.words.push_back((static_cast<std::uint32_t>(operands.size() + 1) << SpvWordCountShift) |
                                static_cast<std::uint32_t>(opcode));
    instruction.words.insert(instruction.words.end(), operands.begin(), operands.end());
    return instruction;
}

bool symbolMatches(std::string_view emitted, std::string_view requested) {
    if (emitted == requested) return true;
    if (emitted.size() <= requested.size() || emitted.substr(0, requested.size()) != requested) return false;
    const auto next = emitted[requested.size()];
    return next == '(' || next == '@' || next == '$' || next == '.';
}

std::uint32_t findNamedId(const ParsedModule &module, std::string_view name) {
    std::uint32_t prefix_match = 0;
    for (const auto &instruction : module.instructions) {
        if (instruction.opcode() != SpvOpName || instruction.words.size() < 3) continue;
        const auto emitted = literalString(instruction, 2);
        if (emitted == name) return instruction.words[1];
        if (symbolMatches(emitted, name)) {
            if (prefix_match != 0 && prefix_match != instruction.words[1]) {
                throw std::runtime_error("SPIR-V symbol '" + std::string{name} + "' is ambiguous");
            }
            prefix_match = instruction.words[1];
        }
    }
    if (prefix_match != 0) return prefix_match;
    throw std::runtime_error("SPIR-V symbol '" + std::string{name} + "' was not found");
}

std::uint32_t findEntryPoint(const ParsedModule &module, std::string_view name) {
    for (const auto &instruction : module.instructions) {
        if (instruction.opcode() == SpvOpEntryPoint && instruction.words.size() >= 4 &&
            literalString(instruction, 3) == name) {
            return instruction.words[2];
        }
    }
    throw std::runtime_error("SPIR-V entry point '" + std::string{name} + "' was not found");
}

void addLinkageCapability(ParsedModule &module) {
    for (const auto &instruction : module.instructions) {
        if (instruction.opcode() == SpvOpCapability && instruction.words.size() == 2 &&
            instruction.words[1] == SpvCapabilityLinkage) return;
    }
    const auto position = std::find_if(module.instructions.begin(), module.instructions.end(),
                                       [](const auto &instruction) {
                                           return instruction.opcode() != SpvOpCapability;
                                       });
    module.instructions.insert(position,
                               makeInstruction(SpvOpCapability, {SpvCapabilityLinkage}));
}

void addLinkageDecoration(ParsedModule &module, std::uint32_t function_id,
                          std::string_view public_name, SpvLinkageType linkage) {
    auto operands = std::vector<std::uint32_t>{function_id, SpvDecorationLinkageAttributes};
    auto name_words = encodedString(public_name);
    operands.insert(operands.end(), name_words.begin(), name_words.end());
    operands.push_back(linkage);
    auto position = std::find_if(module.instructions.begin(), module.instructions.end(),
                                 [](const auto &instruction) {
                                     return instruction.opcode() >= SpvOpTypeVoid &&
                                            instruction.opcode() <= SpvOpTypeForwardPointer;
                                 });
    if (position == module.instructions.end()) {
        throw std::runtime_error("SPIR-V module has no type section");
    }
    module.instructions.insert(position, makeInstruction(SpvOpDecorate, std::move(operands)));
}

void stripFunctionBodyToImport(ParsedModule &module, std::uint32_t function_id) {
    auto begin = std::find_if(module.instructions.begin(), module.instructions.end(),
                              [&](const auto &instruction) {
                                  return instruction.opcode() == SpvOpFunction &&
                                         instruction.resultId() == function_id;
                              });
    if (begin == module.instructions.end()) throw std::runtime_error("SPIR-V function body was not found");
    auto end = std::find_if(begin, module.instructions.end(), [](const auto &instruction) {
        return instruction.opcode() == SpvOpFunctionEnd;
    });
    if (end == module.instructions.end()) throw std::runtime_error("SPIR-V function body is unterminated");

    std::vector<Instruction> declaration;
    declaration.push_back(*begin);
    for (auto it = std::next(begin); it != end; ++it) {
        if (it->opcode() == SpvOpFunctionParameter) declaration.push_back(*it);
    }
    declaration.push_back(*end);
    const auto index = static_cast<std::size_t>(std::distance(module.instructions.begin(), begin));
    module.instructions.erase(begin, std::next(end));
    module.instructions.insert(module.instructions.begin() + static_cast<std::ptrdiff_t>(index),
                               declaration.begin(), declaration.end());
}

void removeEntryPointAndFunction(ParsedModule &module, std::uint32_t function_id) {
    module.instructions.erase(
        std::remove_if(module.instructions.begin(), module.instructions.end(), [&](const auto &instruction) {
            const auto op = instruction.opcode();
            if ((op == SpvOpEntryPoint && instruction.words.size() > 2 && instruction.words[2] == function_id) ||
                ((op == SpvOpExecutionMode || op == SpvOpExecutionModeId || op == SpvOpName ||
                  op == SpvOpDecorate || op == SpvOpDecorateId) &&
                 instruction.words.size() > 1 && instruction.words[1] == function_id)) return true;
            return false;
        }),
        module.instructions.end());

    auto begin = std::find_if(module.instructions.begin(), module.instructions.end(),
                              [&](const auto &instruction) {
                                  return instruction.opcode() == SpvOpFunction &&
                                         instruction.resultId() == function_id;
                              });
    if (begin == module.instructions.end()) throw std::runtime_error("SPIR-V entry function body was not found");
    auto end = std::find_if(begin, module.instructions.end(), [](const auto &instruction) {
        return instruction.opcode() == SpvOpFunctionEnd;
    });
    if (end == module.instructions.end()) throw std::runtime_error("SPIR-V entry function is unterminated");
    module.instructions.erase(begin, std::next(end));
}

void stripDanglingDebugNames(ParsedModule &module) {
    std::unordered_set<std::uint32_t> defined;
    for (const auto &instruction : module.instructions) {
        if (const auto id = instruction.resultId(); id != 0) defined.insert(id);
    }
    module.instructions.erase(
        std::remove_if(module.instructions.begin(), module.instructions.end(), [&](const auto &instruction) {
            return (instruction.opcode() == SpvOpName || instruction.opcode() == SpvOpMemberName) &&
                   instruction.words.size() > 1 && !defined.contains(instruction.words[1]);
        }),
        module.instructions.end());
}

using InstructionMap = std::unordered_map<std::uint32_t, const Instruction *>;

InstructionMap typeInstructionMap(const ParsedModule &module) {
    InstructionMap map;
    for (const auto &instruction : module.instructions) {
        if (const auto id = instruction.resultId(); id != 0) map.emplace(id, &instruction);
    }
    return map;
}

std::unordered_set<std::uint32_t> blockTypes(const ParsedModule &module) {
    std::unordered_set<std::uint32_t> blocks;
    for (const auto &instruction : module.instructions) {
        if (instruction.opcode() == SpvOpDecorate && instruction.words.size() >= 3 &&
            (instruction.words[2] == SpvDecorationBlock ||
             instruction.words[2] == SpvDecorationBufferBlock)) {
            blocks.insert(instruction.words[1]);
        }
    }
    return blocks;
}

void validateAbiType(std::uint32_t type_id, std::string_view symbol, bool allow_void,
                     const InstructionMap &types, const std::unordered_set<std::uint32_t> &blocks,
                     std::unordered_set<std::uint32_t> &visited,
                     std::unordered_set<std::uint32_t> &abi_structs) {
    if (!visited.insert(type_id).second) return;
    const auto found = types.find(type_id);
    if (found == types.end()) {
        throw std::runtime_error("SPIR-V ABI '" + std::string{symbol} + "' references an unknown type");
    }
    const auto &instruction = *found->second;
    switch (instruction.opcode()) {
    case SpvOpTypeVoid:
        if (allow_void) return;
        break;
    case SpvOpTypeBool:
    case SpvOpTypeInt:
    case SpvOpTypeFloat:
        return;
    case SpvOpTypeVector:
        if (instruction.words.size() >= 4 && instruction.words[3] >= 2 && instruction.words[3] <= 4) {
            validateAbiType(instruction.words[2], symbol, false, types, blocks, visited, abi_structs);
            return;
        }
        break;
    case SpvOpTypeStruct:
        if (blocks.contains(type_id)) break;
        abi_structs.insert(type_id);
        for (std::size_t i = 2; i < instruction.words.size(); ++i) {
            validateAbiType(instruction.words[i], symbol, false, types, blocks, visited, abi_structs);
        }
        return;
    case SpvOpTypePointer:
        if (instruction.words.size() >= 4 && instruction.words[2] == SpvStorageClassFunction) {
            validateAbiType(instruction.words[3], symbol, false, types, blocks, visited, abi_structs);
            return;
        }
        break;
    default:
        break;
    }
    throw std::runtime_error("SPIR-V ABI '" + std::string{symbol} +
                             "' uses a forbidden type (only scalar, vec2-4, and simple struct are allowed)");
}

std::unordered_set<std::uint32_t> validateFunctionAbi(const ParsedModule &module,
                                                      const std::vector<std::string> &symbols) {
    const auto types = typeInstructionMap(module);
    const auto blocks = blockTypes(module);
    std::unordered_set<std::uint32_t> abi_structs;
    for (const auto &symbol : symbols) {
        const auto function_id = findNamedId(module, symbol);
        auto begin = std::find_if(module.instructions.begin(), module.instructions.end(),
                                  [&](const auto &instruction) {
                                      return instruction.opcode() == SpvOpFunction &&
                                             instruction.resultId() == function_id;
                                  });
        if (begin == module.instructions.end()) {
            throw std::runtime_error("SPIR-V ABI function '" + symbol + "' has no definition");
        }
        std::unordered_set<std::uint32_t> visited;
        validateAbiType(begin->words[1], symbol, true, types, blocks, visited, abi_structs);
        for (auto it = std::next(begin); it != module.instructions.end() &&
             it->opcode() != SpvOpFunctionEnd; ++it) {
            if (it->opcode() == SpvOpFunctionParameter) {
                validateAbiType(it->words[1], symbol, false, types, blocks, visited, abi_structs);
            }
        }
    }
    return abi_structs;
}

void normalizeAbiDecorations(ParsedModule &module,
                             const std::unordered_set<std::uint32_t> &abi_structs) {
    module.instructions.erase(
        std::remove_if(module.instructions.begin(), module.instructions.end(), [&](const auto &instruction) {
            if (instruction.opcode() == SpvOpMemberDecorate && instruction.words.size() >= 3 &&
                abi_structs.contains(instruction.words[1])) {
                return true;
            }
            return instruction.opcode() == SpvOpDecorate && instruction.words.size() >= 3 &&
                   abi_structs.contains(instruction.words[1]) &&
                   instruction.words[2] == SpvDecorationRelaxedPrecision;
        }),
        module.instructions.end());
}

std::string descriptorTypeName(SpvReflectDescriptorType type) {
    switch (type) {
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER: return "sampler";
    case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "combined_image_sampler";
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return "sampled_image";
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE: return "storage_image";
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return "uniform_texel_buffer";
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return "storage_texel_buffer";
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return "uniform_buffer";
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER: return "storage_buffer";
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return "uniform_buffer_dynamic";
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return "storage_buffer_dynamic";
    case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return "input_attachment";
    case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return "acceleration_structure";
    default: return "unknown";
    }
}

struct ReflectionModule {
    SpvReflectShaderModule module{};
    explicit ReflectionModule(std::span<const std::uint32_t> words) {
        if (spvReflectCreateShaderModule(words.size_bytes(), words.data(), &module) !=
            SPV_REFLECT_RESULT_SUCCESS) {
            throw std::runtime_error("SPIRV-Reflect could not inspect the user module");
        }
    }
    ~ReflectionModule() { spvReflectDestroyShaderModule(&module); }
};

struct ReflectedDescriptor {
    std::string name;
    SpvReflectDescriptorType type{};
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    SpvReflectDescriptorBinding *binding_ptr = nullptr;
};

std::pair<std::vector<std::uint32_t>, std::vector<SpvLinkBinding>>
remapDescriptors(std::span<const std::uint32_t> words, const SpvLinkRequest &request) {
    ReflectionModule reflected{words};
    std::uint32_t count = 0;
    if (spvReflectEnumerateDescriptorBindings(&reflected.module, &count, nullptr) !=
        SPV_REFLECT_RESULT_SUCCESS) {
        throw std::runtime_error("SPIRV-Reflect could not enumerate descriptors");
    }
    std::vector<SpvReflectDescriptorBinding *> raw(count);
    if (spvReflectEnumerateDescriptorBindings(&reflected.module, &count, raw.data()) !=
        SPV_REFLECT_RESULT_SUCCESS) {
        throw std::runtime_error("SPIRV-Reflect could not enumerate descriptors");
    }
    std::vector<ReflectedDescriptor> descriptors;
    descriptors.reserve(raw.size());
    for (auto *binding : raw) {
        descriptors.push_back({binding->name == nullptr ? "" : binding->name,
                               binding->descriptor_type, binding->set, binding->binding, binding});
    }
    std::sort(descriptors.begin(), descriptors.end(), [](const auto &lhs, const auto &rhs) {
        return std::tie(lhs.set, lhs.binding, lhs.name) < std::tie(rhs.set, rhs.binding, rhs.name);
    });

    const std::unordered_set<std::string> preserved{request.preserved_descriptor_names.begin(),
                                                    request.preserved_descriptor_names.end()};
    std::set<std::uint32_t> used;
    for (const auto &descriptor : descriptors) {
        if (descriptor.set == request.material_set &&
            (descriptor.binding >= request.first_free_material_binding ||
             preserved.contains(descriptor.name))) {
            used.insert(descriptor.binding);
        }
    }

    std::uint32_t next = request.first_free_material_binding;
    std::vector<SpvLinkBinding> table;
    table.reserve(descriptors.size());
    for (auto &descriptor : descriptors) {
        auto remapped_binding = descriptor.binding;
        if (descriptor.set == request.material_set &&
            descriptor.binding < request.first_free_material_binding &&
            !preserved.contains(descriptor.name)) {
            while (used.contains(next)) ++next;
            remapped_binding = next++;
            used.insert(remapped_binding);
            if (spvReflectChangeDescriptorBindingNumbers(&reflected.module, descriptor.binding_ptr,
                                                         remapped_binding,
                                                         SPV_REFLECT_SET_NUMBER_DONT_CHANGE) !=
                SPV_REFLECT_RESULT_SUCCESS) {
                throw std::runtime_error("SPIRV-Reflect failed to remap descriptor '" +
                                         descriptor.name + "'");
            }
        }
        table.push_back({descriptor.name, descriptorTypeName(descriptor.type), descriptor.set,
                         descriptor.binding, descriptor.set, remapped_binding});
    }
    const auto *code = spvReflectGetCode(&reflected.module);
    const auto size = spvReflectGetCodeSize(&reflected.module) / sizeof(std::uint32_t);
    return {{code, code + size}, std::move(table)};
}

void prepareTemplate(ParsedModule &module, const SpvLinkRequest &request) {
    auto symbols = request.user_exports;
    symbols.insert(symbols.end(), request.template_exports.begin(), request.template_exports.end());
    normalizeAbiDecorations(module, validateFunctionAbi(module, symbols));
    addLinkageCapability(module);
    for (const auto &symbol : request.user_exports) {
        const auto id = findNamedId(module, symbol);
        addLinkageDecoration(module, id, symbol, SpvLinkageTypeImport);
        stripFunctionBodyToImport(module, id);
    }
    for (const auto &symbol : request.template_exports) {
        addLinkageDecoration(module, findNamedId(module, symbol), symbol, SpvLinkageTypeExport);
    }
    stripDanglingDebugNames(module);
}

void prepareUser(ParsedModule &module, const SpvLinkRequest &request) {
    auto symbols = request.user_exports;
    symbols.insert(symbols.end(), request.template_exports.begin(), request.template_exports.end());
    normalizeAbiDecorations(module, validateFunctionAbi(module, symbols));
    const auto entry_id = findEntryPoint(module, "main");
    addLinkageCapability(module);
    for (const auto &symbol : request.user_exports) {
        addLinkageDecoration(module, findNamedId(module, symbol), symbol, SpvLinkageTypeExport);
    }
    for (const auto &symbol : request.template_exports) {
        const auto id = findNamedId(module, symbol);
        addLinkageDecoration(module, id, symbol, SpvLinkageTypeImport);
        stripFunctionBodyToImport(module, id);
    }
    removeEntryPointAndFunction(module, entry_id);
    stripDanglingDebugNames(module);
}

std::string digest(std::span<const std::uint32_t> words) {
    const auto *begin = reinterpret_cast<const unsigned char *>(words.data());
    return picosha2::hash256_hex_string(begin, begin + words.size_bytes());
}

std::string makeCacheKey(const SpvLinkRequest &request) {
    std::ostringstream material;
    material << spvLinkToolchainManifest()
             << ";target=vulkan1.2;template-generator=0x" << std::hex
             << (request.template_module.size() > 2 ? request.template_module[2] : 0)
             << ";user-generator=0x"
             << (request.user_module.size() > 2 ? request.user_module[2] : 0) << std::dec
             << ";template-sha256=" << digest(request.template_module)
             << ";user-sha256=" << digest(request.user_module)
             << ";material-set=" << request.material_set
             << ";first-free-binding=" << request.first_free_material_binding;
    for (const auto &symbol : request.user_exports) material << ";user-export=" << symbol;
    for (const auto &symbol : request.template_exports) material << ";template-export=" << symbol;
    for (const auto &salt : request.cache_salts) material << ";cache-salt=" << salt;
    const auto raw = material.str();
    return raw + ";key-sha256=" + picosha2::hash256_hex_string(raw);
}

} // namespace

std::string spvLinkToolchainManifest() {
    return "spirv-headers=" + std::string{headers_revision} + ";spirv-tools=" +
           std::string{tools_revision} + ";spirv-reflect=" + std::string{reflect_revision} +
           ";spirv-tools-runtime=" + spvSoftwareVersionString();
}

SpvLinkResult linkSpirvModules(const SpvLinkRequest &request) {
    if (request.user_exports.empty()) {
        throw std::runtime_error("SPIR-V link requires at least one user ABI export");
    }

    auto [remapped_user, binding_table] = remapDescriptors(request.user_module, request);
    auto template_module = parseModule(request.template_module, "template module");
    auto user_module = parseModule(remapped_user, "user module");
    prepareTemplate(template_module, request);
    prepareUser(user_module, request);

    spvtools::Context context{target_env};
    std::string messages;
    context.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &position,
                                   const char *message) {
        messages += "[" + std::to_string(position.index) + "] " + message + "\n";
    });
    spvtools::LinkerOptions link_options;
    link_options.SetVerifyIds(true);
    std::vector<std::uint32_t> linked;
    if (spvtools::Link(context, std::vector<std::vector<std::uint32_t>>{
                                   serialize(template_module), serialize(user_module)},
                       &linked, link_options) != SPV_SUCCESS) {
        throw std::runtime_error("SPIRV-Tools link failed:\n" + messages);
    }

    messages.clear();
    spvtools::Optimizer optimizer{target_env};
    optimizer.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &position,
                                     const char *message) {
        messages += "[" + std::to_string(position.index) + "] " + message + "\n";
    });
    optimizer.RegisterPass(spvtools::CreateInlineExhaustivePass());
    optimizer.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
    optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass());
    optimizer.RegisterPass(spvtools::CreateDeadVariableEliminationPass());
    optimizer.RegisterPass(spvtools::CreateCompactIdsPass());
    std::vector<std::uint32_t> optimized;
    if (!optimizer.Run(linked.data(), linked.size(), &optimized)) {
        throw std::runtime_error("SPIRV-Tools optimization failed:\n" + messages);
    }

    messages.clear();
    spvtools::SpirvTools validator{target_env};
    validator.SetMessageConsumer([&](spv_message_level_t, const char *, const spv_position_t &position,
                                     const char *message) {
        messages += "[" + std::to_string(position.index) + "] " + message + "\n";
    });
    if (!validator.Validate(optimized)) {
        throw std::runtime_error("SPIRV-Tools validation failed:\n" + messages);
    }

    auto final_request = request;
    final_request.first_free_material_binding = 0; // reflection-only; no binding can be below zero
    auto [unchanged_final, final_bindings] = remapDescriptors(optimized, final_request);
    (void)unchanged_final;
    for (auto &binding : final_bindings) {
        const auto found = std::find_if(binding_table.begin(), binding_table.end(),
                                        [&](const auto &existing) {
                                            return existing.set == binding.set &&
                                                   existing.binding == binding.binding &&
                                                   existing.descriptor_type == binding.descriptor_type;
                                        });
        if (found == binding_table.end()) binding_table.push_back(std::move(binding));
    }
    std::sort(binding_table.begin(), binding_table.end(), [](const auto &lhs, const auto &rhs) {
        return std::tie(lhs.set, lhs.binding, lhs.descriptor_type, lhs.logical_name) <
               std::tie(rhs.set, rhs.binding, rhs.descriptor_type, rhs.logical_name);
    });

    return {std::move(optimized), std::move(binding_table), makeCacheKey(request)};
}

} // namespace Pelican
