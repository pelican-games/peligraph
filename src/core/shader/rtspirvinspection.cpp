#include "rtspirvinspection.hpp"

#include <spirv_reflect.h>

#include <algorithm>
#include <array>
#include <bit>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Pelican {

namespace {

constexpr std::uint32_t spirv_magic = 0x07230203u;

[[noreturn]] void failUnsupported(
    std::string_view uri, std::string detail) {
    throw std::runtime_error(
        "pelican.ray_tracing.interface_type_unsupported@1: shader '" +
        std::string{uri} + "' " + std::move(detail));
}

[[noreturn]] void failInterfaceCount(
    std::string_view uri, const SpirvEntryPointKey &entry,
    std::string detail) {
    throw std::runtime_error(
        "pelican.ray_tracing.interface_count@1: shader '" +
        std::string{uri} + "' entry '" + entry.name +
        "' execution_model=" +
        std::to_string(entry.execution_model) + " " +
        std::move(detail));
}

std::uint64_t checkedAdd(std::uint64_t left, std::uint64_t right,
                         std::string_view context) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw std::overflow_error(
            "pelican.ray_tracing.interface_type_unsupported@1: " +
            std::string{context} + " byte-size overflow");
    }
    return left + right;
}

std::uint64_t checkedMultiply(std::uint64_t left,
                              std::uint64_t right,
                              std::string_view context) {
    if (left != 0 &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        throw std::overflow_error(
            "pelican.ray_tracing.interface_type_unsupported@1: " +
            std::string{context} + " byte-size overflow");
    }
    return left * right;
}

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment,
                      std::string_view context) {
    if (alignment == 0) {
        throw std::runtime_error(
            "pelican.ray_tracing.interface_type_unsupported@1: " +
            std::string{context} + " has zero scalar alignment");
    }
    const auto remainder = value % alignment;
    return remainder == 0
               ? value
               : checkedAdd(value, alignment - remainder, context);
}

struct ScalarLayout {
    std::uint64_t size = 0;
    std::uint64_t alignment = 0;
};

ScalarLayout scalarLayout(const RayTracingCanonicalType &type) {
    const auto scalar = [&]() -> ScalarLayout {
        if (type.kind == RayTracingCanonicalTypeKind::boolean) {
            return {4, 4};
        }
        if (type.bit_width == 0 || type.bit_width % 8 != 0) {
            throw std::runtime_error(
                "pelican.ray_tracing.interface_type_unsupported@1: "
                "RT scalar bit width is not byte-addressable");
        }
        const auto bytes = static_cast<std::uint64_t>(type.bit_width / 8);
        return {bytes, bytes};
    };

    switch (type.kind) {
    case RayTracingCanonicalTypeKind::boolean:
    case RayTracingCanonicalTypeKind::signed_integer:
    case RayTracingCanonicalTypeKind::unsigned_integer:
    case RayTracingCanonicalTypeKind::floating:
        return scalar();
    case RayTracingCanonicalTypeKind::vector: {
        if (type.elements.size() != 1 || type.element_count == 0) {
            throw std::runtime_error(
                "pelican.ray_tracing.interface_type_unsupported@1: "
                "malformed RT vector type");
        }
        const auto element = scalarLayout(type.elements.front());
        return {checkedMultiply(element.size, type.element_count,
                                "RT vector"),
                element.alignment};
    }
    case RayTracingCanonicalTypeKind::matrix:
    case RayTracingCanonicalTypeKind::fixed_array: {
        if (type.elements.size() != 1 || type.element_count == 0) {
            throw std::runtime_error(
                "pelican.ray_tracing.interface_type_unsupported@1: "
                "malformed RT aggregate type");
        }
        const auto element = scalarLayout(type.elements.front());
        const auto stride =
            alignUp(element.size, element.alignment, "RT aggregate");
        return {checkedMultiply(stride, type.element_count,
                                "RT aggregate"),
                element.alignment};
    }
    case RayTracingCanonicalTypeKind::structure: {
        std::uint64_t offset = 0;
        std::uint64_t alignment = 1;
        for (const auto &member_type : type.elements) {
            const auto member = scalarLayout(member_type);
            alignment = std::max(alignment, member.alignment);
            offset = alignUp(offset, member.alignment, "RT structure");
            offset = checkedAdd(offset, member.size, "RT structure");
        }
        return {alignUp(offset, alignment, "RT structure"), alignment};
    }
    }
    throw std::runtime_error(
        "pelican.ray_tracing.interface_type_unsupported@1: "
        "unknown RT canonical type");
}

std::string canonicalText(const RayTracingCanonicalType &type) {
    std::ostringstream stream;
    switch (type.kind) {
    case RayTracingCanonicalTypeKind::boolean:
        stream << "bool";
        break;
    case RayTracingCanonicalTypeKind::signed_integer:
        stream << 'i' << type.bit_width;
        break;
    case RayTracingCanonicalTypeKind::unsigned_integer:
        stream << 'u' << type.bit_width;
        break;
    case RayTracingCanonicalTypeKind::floating:
        stream << 'f' << type.bit_width;
        break;
    case RayTracingCanonicalTypeKind::vector:
        stream << "vec" << type.element_count << '<'
               << canonicalText(type.elements.at(0)) << '>';
        break;
    case RayTracingCanonicalTypeKind::matrix:
        stream << "mat" << type.element_count << '<'
               << canonicalText(type.elements.at(0)) << '>';
        break;
    case RayTracingCanonicalTypeKind::fixed_array:
        stream << "array" << type.element_count << '<'
               << canonicalText(type.elements.at(0)) << '>';
        break;
    case RayTracingCanonicalTypeKind::structure:
        stream << "struct{";
        for (std::size_t index = 0; index < type.elements.size(); ++index) {
            if (index != 0) stream << ',';
            stream << canonicalText(type.elements[index]);
        }
        stream << '}';
        break;
    }
    return stream.str();
}

struct TypeInstruction {
    SpvOp opcode = SpvOpNop;
    std::vector<std::uint32_t> operands;
};

struct Producer {
    SpvOp opcode = SpvOpNop;
    std::uint32_t type_id = 0;
    std::optional<std::uint32_t> literal;
};

struct Variable {
    std::uint32_t pointer_type_id = 0;
    SpvStorageClass storage = SpvStorageClassMax;
};

struct RawEntry {
    SpirvEntryPointKey key;
    std::uint32_t function_id = 0;
    std::vector<std::uint32_t> interfaces;
};

struct RawTraceCall {
    std::uint32_t function_id = 0;
    std::uint32_t instruction_number = 0;
    std::uint32_t sbt_record_offset_id = 0;
    std::uint32_t sbt_record_stride_id = 0;
    std::uint32_t miss_index_id = 0;
    std::uint32_t payload_id = 0;
    std::uint32_t ray_flags_id = 0;
};

std::string decodeLiteralString(
    std::span<const std::uint32_t> words, std::size_t first_word,
    std::size_t &consumed_words, std::string_view uri) {
    std::string result;
    for (std::size_t word_index = first_word;
         word_index < words.size(); ++word_index) {
        const auto word = words[word_index];
        for (std::uint32_t byte_index = 0; byte_index < 4; ++byte_index) {
            const auto value = static_cast<char>(
                (word >> (byte_index * 8u)) & 0xffu);
            if (value == '\0') {
                consumed_words = word_index - first_word + 1;
                return result;
            }
            result.push_back(value);
        }
    }
    failUnsupported(uri, "contains an unterminated OpEntryPoint name");
}

std::string producerName(SpvOp opcode) {
    switch (opcode) {
    case SpvOpConstant:
        return "OpConstant";
    case SpvOpConstantNull:
        return "OpConstantNull";
    case SpvOpSpecConstantTrue:
    case SpvOpSpecConstantFalse:
    case SpvOpSpecConstant:
    case SpvOpSpecConstantComposite:
    case SpvOpSpecConstantOp:
        return "OpSpecConstant*";
    case SpvOpCopyObject:
        return "OpCopyObject";
    default:
        return "dynamic instruction";
    }
}

class Inspector {
    std::span<const std::uint32_t> words_;
    std::string uri_;
    std::unordered_map<std::uint32_t, TypeInstruction> types_;
    std::unordered_map<std::uint32_t, Producer> producers_;
    std::unordered_map<std::uint32_t, Variable> variables_;
    std::unordered_map<std::uint32_t, std::uint32_t> locations_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t>
        member_offsets_;
    std::vector<RawEntry> entries_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> calls_;
    std::vector<RawTraceCall> trace_calls_;
    std::unordered_map<std::uint32_t, RayTracingCanonicalType>
        canonical_cache_;
    std::unordered_set<std::uint32_t> canonical_stack_;

    bool isInt32(std::uint32_t type_id) const {
        const auto found = types_.find(type_id);
        return found != types_.end() &&
               found->second.opcode == SpvOpTypeInt &&
               found->second.operands.size() >= 2 &&
               found->second.operands[0] == 32;
    }

    std::uint32_t directUint32Constant(
        std::uint32_t id, std::uint32_t instruction_number,
        std::string_view operand_name, bool trace_operand) const {
        const auto found = producers_.find(id);
        const auto fail = [&](std::string producer) -> std::uint32_t {
            const auto prefix = trace_operand
                                    ? "pelican.ray_tracing.trace_operand_dynamic@1: shader '"
                                    : "pelican.ray_tracing.interface_type_unsupported@1: shader '";
            throw std::runtime_error(
                std::string{prefix} + uri_ + "' instruction " +
                std::to_string(instruction_number) + " operand '" +
                std::string{operand_name} + "' is " + producer +
                "; only direct OpConstant 32-bit integer and "
                "OpConstantNull are accepted");
        };
        if (found == producers_.end()) {
            return fail("dynamic or unknown id " + std::to_string(id));
        }
        const auto &producer = found->second;
        if ((producer.opcode != SpvOpConstant &&
             producer.opcode != SpvOpConstantNull) ||
            !isInt32(producer.type_id)) {
            return fail(producerName(producer.opcode));
        }
        return producer.opcode == SpvOpConstantNull
                   ? 0u
                   : producer.literal.value_or(0u);
    }

    std::optional<std::uint32_t> tryDirectUint32Constant(
        std::uint32_t id) const noexcept {
        const auto found = producers_.find(id);
        if (found == producers_.end()) return std::nullopt;
        const auto &producer = found->second;
        if ((producer.opcode != SpvOpConstant &&
             producer.opcode != SpvOpConstantNull) ||
            !isInt32(producer.type_id)) {
            return std::nullopt;
        }
        return producer.opcode == SpvOpConstantNull
                   ? std::optional<std::uint32_t>{0u}
                   : producer.literal;
    }

    RayTracingCanonicalType canonicalType(std::uint32_t type_id,
                                          std::uint32_t instruction_number) {
        if (const auto found = canonical_cache_.find(type_id);
            found != canonical_cache_.end()) {
            return found->second;
        }
        if (!canonical_stack_.insert(type_id).second) {
            failUnsupported(uri_, "has recursive RT interface type id " +
                                      std::to_string(type_id));
        }
        const auto clear_stack = [&] { canonical_stack_.erase(type_id); };
        const auto found = types_.find(type_id);
        if (found == types_.end()) {
            clear_stack();
            failUnsupported(uri_, "references unknown RT interface type id " +
                                      std::to_string(type_id));
        }
        const auto &type = found->second;
        RayTracingCanonicalType result;
        switch (type.opcode) {
        case SpvOpTypeBool:
            result.kind = RayTracingCanonicalTypeKind::boolean;
            result.bit_width = 32;
            break;
        case SpvOpTypeInt:
            if (type.operands.size() < 2) {
                clear_stack();
                failUnsupported(uri_, "has malformed OpTypeInt");
            }
            result.kind = type.operands[1] == 0
                              ? RayTracingCanonicalTypeKind::unsigned_integer
                              : RayTracingCanonicalTypeKind::signed_integer;
            result.bit_width = type.operands[0];
            break;
        case SpvOpTypeFloat:
            if (type.operands.empty()) {
                clear_stack();
                failUnsupported(uri_, "has malformed OpTypeFloat");
            }
            result.kind = RayTracingCanonicalTypeKind::floating;
            result.bit_width = type.operands[0];
            break;
        case SpvOpTypeVector:
        case SpvOpTypeMatrix:
            if (type.operands.size() < 2 || type.operands[1] == 0) {
                clear_stack();
                failUnsupported(uri_, "has malformed RT vector/matrix type");
            }
            result.kind = type.opcode == SpvOpTypeVector
                              ? RayTracingCanonicalTypeKind::vector
                              : RayTracingCanonicalTypeKind::matrix;
            result.element_count = type.operands[1];
            result.elements.push_back(
                canonicalType(type.operands[0], instruction_number));
            break;
        case SpvOpTypeArray:
            if (type.operands.size() < 2) {
                clear_stack();
                failUnsupported(uri_, "has malformed OpTypeArray");
            }
            result.kind = RayTracingCanonicalTypeKind::fixed_array;
            result.element_count = directUint32Constant(
                type.operands[1], instruction_number,
                "array length", false);
            if (result.element_count == 0) {
                clear_stack();
                failUnsupported(uri_, "uses zero-sized RT interface array");
            }
            result.elements.push_back(
                canonicalType(type.operands[0], instruction_number));
            break;
        case SpvOpTypeRuntimeArray:
            clear_stack();
            failUnsupported(
                uri_, "uses runtime-sized RT interface array at instruction " +
                          std::to_string(instruction_number));
        case SpvOpTypeStruct:
            result.kind = RayTracingCanonicalTypeKind::structure;
            for (const auto member : type.operands) {
                result.elements.push_back(
                    canonicalType(member, instruction_number));
            }
            break;
        case SpvOpTypePointer:
            if (type.operands.size() < 2) {
                clear_stack();
                failUnsupported(uri_, "has malformed OpTypePointer");
            }
            result = canonicalType(type.operands[1], instruction_number);
            break;
        default:
            clear_stack();
            failUnsupported(
                uri_, "uses unsupported RT interface type opcode " +
                          std::to_string(static_cast<std::uint32_t>(type.opcode)) +
                          " at instruction " +
                          std::to_string(instruction_number));
        }
        clear_stack();
        canonical_cache_.emplace(type_id, result);
        return result;
    }

    static std::optional<RayTracingInterfaceStorage>
    interfaceStorage(SpvStorageClass storage) {
        switch (storage) {
        case SpvStorageClassRayPayloadKHR:
            return RayTracingInterfaceStorage::ray_payload;
        case SpvStorageClassIncomingRayPayloadKHR:
            return RayTracingInterfaceStorage::incoming_ray_payload;
        case SpvStorageClassHitAttributeKHR:
            return RayTracingInterfaceStorage::hit_attribute;
        default:
            // CallableDataKHR and IncomingCallableDataKHR are deliberately
            // carved out until RT4, along with all non-RT storage classes.
            return std::nullopt;
        }
    }

    RayTracingInterfaceVariable reflectVariable(
        std::uint32_t variable_id, std::uint32_t instruction_number) {
        const auto found = variables_.find(variable_id);
        if (found == variables_.end()) {
            failUnsupported(uri_, "entry interface references unknown variable id " +
                                      std::to_string(variable_id));
        }
        const auto storage = interfaceStorage(found->second.storage);
        if (!storage) {
            failUnsupported(uri_, "internal RT variable selection mismatch");
        }
        const auto location = locations_.find(variable_id);
        auto type = canonicalType(found->second.pointer_type_id,
                                  instruction_number);
        const auto fingerprint =
            rayTracingCanonicalTypeFingerprint(type);
        const auto byte_size = rayTracingCanonicalTypeByteSize(type);
        return {
            .storage = *storage,
            .location = location == locations_.end()
                            ? std::nullopt
                            : std::optional<std::uint32_t>{
                                  location->second},
            .type = std::move(type),
            .fingerprint = fingerprint,
            .byte_size = byte_size,
        };
    }

    RayTracingShaderRecordReflection reflectShaderRecord(
        std::uint32_t variable_id, std::uint32_t instruction_number) {
        const auto variable = variables_.find(variable_id);
        if (variable == variables_.end() ||
            variable->second.storage !=
                SpvStorageClassShaderRecordBufferKHR) {
            failUnsupported(uri_,
                            "internal shader-record variable selection mismatch");
        }
        const auto pointer = types_.find(variable->second.pointer_type_id);
        if (pointer == types_.end() ||
            pointer->second.opcode != SpvOpTypePointer ||
            pointer->second.operands.size() < 2) {
            failUnsupported(uri_,
                            "shader-record variable does not have a pointer type");
        }
        const auto structure_id = pointer->second.operands[1];
        const auto structure = types_.find(structure_id);
        if (structure == types_.end() ||
            structure->second.opcode != SpvOpTypeStruct) {
            failUnsupported(uri_,
                            "shader-record storage must point to a structure");
        }
        RayTracingShaderRecordReflection result;
        result.members.reserve(structure->second.operands.size());
        for (std::size_t member = 0;
             member < structure->second.operands.size(); ++member) {
            const auto offset = member_offsets_.find(
                {structure_id, static_cast<std::uint32_t>(member)});
            if (offset == member_offsets_.end()) {
                failUnsupported(
                    uri_, "shader-record member " + std::to_string(member) +
                              " has no SPIR-V Offset decoration");
            }
            result.members.push_back({
                .type = canonicalType(
                    structure->second.operands[member], instruction_number),
                .offset = offset->second,
            });
        }
        return result;
    }

    void parse() {
        if (words_.size() < 5 || words_[0] != spirv_magic) {
            failUnsupported(uri_, "is not a SPIR-V module");
        }
        std::optional<std::uint32_t> current_function;
        std::uint32_t instruction_number = 0;
        for (std::size_t offset = 5; offset < words_.size();) {
            ++instruction_number;
            const auto header = words_[offset];
            const auto word_count = header >> 16u;
            const auto opcode = static_cast<SpvOp>(header & 0xffffu);
            if (word_count == 0 || offset + word_count > words_.size()) {
                failUnsupported(uri_, "has malformed instruction " +
                                          std::to_string(instruction_number));
            }
            const auto instruction = words_.subspan(offset, word_count);
            const auto remember_type = [&](std::size_t first_operand = 2) {
                if (instruction.size() < first_operand) {
                    failUnsupported(uri_, "has malformed type instruction " +
                                              std::to_string(instruction_number));
                }
                const auto result_id = instruction[1];
                TypeInstruction value{.opcode = opcode};
                value.operands.assign(
                    instruction.begin() +
                        static_cast<std::ptrdiff_t>(first_operand),
                    instruction.end());
                if (!types_.emplace(result_id, std::move(value)).second) {
                    failUnsupported(uri_, "redefines SPIR-V id " +
                                              std::to_string(result_id));
                }
            };

            switch (opcode) {
            case SpvOpEntryPoint: {
                if (instruction.size() < 4) {
                    failUnsupported(uri_, "has malformed OpEntryPoint");
                }
                std::size_t name_words = 0;
                auto name = decodeLiteralString(
                    instruction, 3, name_words, uri_);
                RawEntry entry{
                    .key = {instruction[1], std::move(name)},
                    .function_id = instruction[2],
                };
                const auto first_interface = 3 + name_words;
                if (first_interface > instruction.size()) {
                    failUnsupported(uri_, "has malformed OpEntryPoint interface");
                }
                entry.interfaces.assign(
                    instruction.begin() +
                        static_cast<std::ptrdiff_t>(first_interface),
                    instruction.end());
                entries_.push_back(std::move(entry));
                break;
            }
            case SpvOpDecorate:
                if (instruction.size() >= 4 &&
                    instruction[2] == SpvDecorationLocation) {
                    locations_.insert_or_assign(instruction[1],
                                                instruction[3]);
                }
                break;
            case SpvOpMemberDecorate:
                if (instruction.size() >= 5 &&
                    instruction[3] == SpvDecorationOffset) {
                    member_offsets_.insert_or_assign(
                        {instruction[1], instruction[2]}, instruction[4]);
                }
                break;
            case SpvOpTypeBool:
            case SpvOpTypeInt:
            case SpvOpTypeFloat:
            case SpvOpTypeVector:
            case SpvOpTypeMatrix:
            case SpvOpTypeArray:
            case SpvOpTypeRuntimeArray:
            case SpvOpTypeStruct:
            case SpvOpTypePointer:
                remember_type();
                break;
            case SpvOpConstant:
                if (instruction.size() >= 4) {
                    producers_.insert_or_assign(
                        instruction[2],
                        Producer{opcode, instruction[1], instruction[3]});
                }
                break;
            case SpvOpConstantNull:
            case SpvOpSpecConstantTrue:
            case SpvOpSpecConstantFalse:
            case SpvOpSpecConstant:
            case SpvOpSpecConstantComposite:
            case SpvOpSpecConstantOp:
            case SpvOpCopyObject:
                if (instruction.size() >= 3) {
                    producers_.insert_or_assign(
                        instruction[2],
                        Producer{opcode, instruction[1], std::nullopt});
                }
                break;
            case SpvOpVariable:
                if (instruction.size() >= 4) {
                    variables_.insert_or_assign(
                        instruction[2],
                        Variable{instruction[1],
                                 static_cast<SpvStorageClass>(instruction[3])});
                }
                break;
            case SpvOpFunction:
                if (instruction.size() >= 3) {
                    current_function = instruction[2];
                }
                break;
            case SpvOpFunctionEnd:
                current_function.reset();
                break;
            case SpvOpFunctionCall:
                if (current_function && instruction.size() >= 4) {
                    calls_[*current_function].push_back(instruction[3]);
                }
                break;
            case SpvOpTraceRayKHR:
                if (!current_function || instruction.size() < 12) {
                    failUnsupported(uri_, "has malformed or global OpTraceRayKHR at "
                                              "instruction " +
                                              std::to_string(instruction_number));
                }
                trace_calls_.push_back({
                    .function_id = *current_function,
                    .instruction_number = instruction_number,
                    .sbt_record_offset_id = instruction[4],
                    .sbt_record_stride_id = instruction[5],
                    .miss_index_id = instruction[6],
                    .payload_id = instruction[11],
                    .ray_flags_id = instruction[2],
                });
                break;
            default:
                break;
            }
            offset += word_count;
        }
    }

  public:
    Inspector(std::span<const std::uint32_t> words,
              std::string_view uri)
        : words_{words}, uri_{uri.empty() ? "memory://unnamed"
                                         : std::string{uri}} {}

    RayTracingModuleReflection run() {
        parse();

        struct ResolvedTrace {
            RawTraceCall raw;
            std::uint32_t offset = 0;
            std::uint32_t stride = 0;
            std::uint32_t miss = 0;
            RayTracingCanonicalType payload_type;
            std::optional<std::uint32_t> ray_flags;
        };
        std::vector<ResolvedTrace> resolved_traces;
        resolved_traces.reserve(trace_calls_.size());
        for (const auto &trace : trace_calls_) {
            const auto variable = variables_.find(trace.payload_id);
            if (variable == variables_.end() ||
                variable->second.storage != SpvStorageClassRayPayloadKHR) {
                failUnsupported(
                    uri_, "OpTraceRayKHR instruction " +
                              std::to_string(trace.instruction_number) +
                              " payload operand is not a direct RayPayloadKHR variable");
            }
            // Build the payload tree now as well, so unsupported nested arrays
            // are rejected even if the producer omitted it from an entry list.
            auto payload_type = canonicalType(
                variable->second.pointer_type_id,
                trace.instruction_number);
            resolved_traces.push_back({
                .raw = trace,
                .offset = directUint32Constant(
                    trace.sbt_record_offset_id, trace.instruction_number,
                    "sbtRecordOffset", true),
                .stride = directUint32Constant(
                    trace.sbt_record_stride_id, trace.instruction_number,
                    "sbtRecordStride", true),
                .miss = directUint32Constant(
                    trace.miss_index_id, trace.instruction_number,
                    "missIndex", true),
                .payload_type = std::move(payload_type),
                .ray_flags = tryDirectUint32Constant(
                    trace.ray_flags_id),
            });
        }

        RayTracingModuleReflection result;
        for (const auto &raw_entry : entries_) {
            RayTracingEntryPointReflection entry{.key = raw_entry.key};
            std::set<std::pair<RayTracingInterfaceStorage, std::uint32_t>>
                seen_interfaces;
            std::array<std::uint32_t, 3> storage_counts{};
            for (const auto variable_id : raw_entry.interfaces) {
                const auto variable = variables_.find(variable_id);
                if (variable == variables_.end()) continue;
                if (variable->second.storage ==
                    SpvStorageClassShaderRecordBufferKHR) {
                    if (entry.shader_record) {
                        failInterfaceCount(
                            uri_, raw_entry.key,
                            "declares more than one shader-record variable");
                    }
                    entry.shader_record =
                        reflectShaderRecord(variable_id, 0);
                    continue;
                }
                const auto storage = interfaceStorage(variable->second.storage);
                if (!storage) continue;
                auto reflected = reflectVariable(variable_id, 0);
                const auto storage_index =
                    static_cast<std::size_t>(*storage);
                ++storage_counts.at(storage_index);
                if (reflected.location &&
                    !seen_interfaces
                         .emplace(*storage, *reflected.location)
                         .second) {
                    failInterfaceCount(
                        uri_, raw_entry.key,
                        "duplicates RT interface kind/location " +
                            std::to_string(storage_index) + "/" +
                            std::to_string(*reflected.location));
                }
                entry.interfaces.push_back(std::move(reflected));
            }
            if (storage_counts[static_cast<std::size_t>(
                    RayTracingInterfaceStorage::incoming_ray_payload)] > 1) {
                failInterfaceCount(uri_, raw_entry.key,
                                   "declares more than one incoming ray payload");
            }
            if (storage_counts[static_cast<std::size_t>(
                    RayTracingInterfaceStorage::hit_attribute)] > 1) {
                failInterfaceCount(uri_, raw_entry.key,
                                   "declares more than one hit attribute");
            }
            std::ranges::sort(
                entry.interfaces, {},
                [](const auto &value) {
                    return std::pair{
                        static_cast<std::uint32_t>(value.storage),
                        value.location};
                });

            std::unordered_set<std::uint32_t> reachable;
            std::vector<std::uint32_t> pending{raw_entry.function_id};
            while (!pending.empty()) {
                const auto function = pending.back();
                pending.pop_back();
                if (!reachable.insert(function).second) continue;
                if (const auto found = calls_.find(function);
                    found != calls_.end()) {
                    pending.insert(pending.end(), found->second.begin(),
                                   found->second.end());
                }
            }
            for (std::size_t index = 0; index < resolved_traces.size();
                 ++index) {
                const auto &trace = resolved_traces[index];
                if (!reachable.contains(trace.raw.function_id)) continue;
                entry.trace_calls.push_back({
                    .caller_entry = raw_entry.key,
                    .instruction_number = trace.raw.instruction_number,
                    .payload_type = trace.payload_type,
                    .miss_index = trace.miss,
                    .sbt_record_offset = trace.offset,
                    .sbt_record_stride = trace.stride,
                    .ray_flags = trace.ray_flags,
                });
            }
            if (!result.entries.emplace(raw_entry.key, std::move(entry)).second) {
                failUnsupported(
                    uri_, "duplicates OpEntryPoint key (execution_model=" +
                              std::to_string(raw_entry.key.execution_model) +
                              ", name='" + raw_entry.key.name + "')");
            }
        }
        return result;
    }
};

} // namespace

std::string describeRayTracingCanonicalType(
    const RayTracingCanonicalType &type) {
    return canonicalText(type);
}

std::string rayTracingCanonicalTypeFingerprint(
    const RayTracingCanonicalType &type) {
    const auto text = canonicalText(type);
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : text) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ull;
    }
    std::ostringstream stream;
    stream << "fnv1a64:" << std::hex << std::setw(16)
           << std::setfill('0') << hash;
    return stream.str();
}

std::uint64_t rayTracingCanonicalTypeByteSize(
    const RayTracingCanonicalType &type) {
    return scalarLayout(type).size;
}

RayTracingModuleReflection inspectRayTracingSpirv(
    std::span<const std::uint32_t> spirv,
    std::string_view diagnostic_uri) {
    return Inspector{spirv, diagnostic_uri}.run();
}

std::uint32_t spirvExecutionModelForShaderStage(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::vertex:
        return SpvExecutionModelVertex;
    case ShaderStage::fragment:
        return SpvExecutionModelFragment;
    case ShaderStage::compute:
        return SpvExecutionModelGLCompute;
    case ShaderStage::raygen:
        return SpvExecutionModelRayGenerationKHR;
    case ShaderStage::miss:
        return SpvExecutionModelMissKHR;
    case ShaderStage::closesthit:
        return SpvExecutionModelClosestHitKHR;
    case ShaderStage::anyhit:
        return SpvExecutionModelAnyHitKHR;
    case ShaderStage::intersection:
        return SpvExecutionModelIntersectionKHR;
    }
    throw std::runtime_error("unknown shader stage");
}

const RayTracingEntryPointReflection &selectRayTracingEntryPoint(
    const RayTracingModuleReflection &reflection,
    ShaderStage expected_stage, std::string_view entry_name,
    std::string_view diagnostic_uri) {
    const SpirvEntryPointKey key{
        spirvExecutionModelForShaderStage(expected_stage),
        std::string{entry_name},
    };
    const auto found = reflection.entries.find(key);
    if (found != reflection.entries.end()) return found->second;

    std::ostringstream available;
    bool first = true;
    for (const auto &[candidate, entry] : reflection.entries) {
        (void)entry;
        if (!first) available << ", ";
        first = false;
        available << '(' << candidate.execution_model << ", '"
                  << candidate.name << "')";
    }
    failUnsupported(
        diagnostic_uri.empty() ? "memory://unnamed" : diagnostic_uri,
        "does not contain expected entry (execution_model=" +
            std::to_string(key.execution_model) + ", name='" + key.name +
            "'); available=[" + available.str() + "]");
}

} // namespace Pelican
