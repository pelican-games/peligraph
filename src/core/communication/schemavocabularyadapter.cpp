#include "schemavocabularyadapter.hpp"

#include <utility>

namespace Pelican::internal {
namespace {

[[noreturn]] void invalidCore(StructFieldType type) {
    throw SchemaTypeMappingError{
        SchemaTypeMappingErrorCode::InvalidCoreOrdinal,
        "core StructFieldType ordinal " +
            std::to_string(static_cast<unsigned>(type)) + " is invalid"};
}

[[noreturn]] void invalidLeaf(Schema::FieldType type) {
    throw SchemaTypeMappingError{
        SchemaTypeMappingErrorCode::InvalidLeafOrdinal,
        "leaf FieldType ordinal " +
            std::to_string(static_cast<unsigned>(type)) + " is invalid"};
}

} // namespace

std::string_view schemaTypeMappingErrorCodeName(
    SchemaTypeMappingErrorCode code) noexcept {
    switch (code) {
    case SchemaTypeMappingErrorCode::InvalidCoreOrdinal:
        return "invalid_core_schema_type_ordinal";
    case SchemaTypeMappingErrorCode::InvalidLeafOrdinal:
        return "invalid_leaf_schema_type_ordinal";
    case SchemaTypeMappingErrorCode::ResolverRejected:
        return "schema_type_resolver_rejected";
    }
    return "invalid_schema_mapping_error_code";
}

SchemaTypeMappingError::SchemaTypeMappingError(
    SchemaTypeMappingErrorCode code, std::string detail)
    : std::runtime_error{
          "schema_mapping_error[" +
          std::string{schemaTypeMappingErrorCodeName(code)} + "]: " + detail},
      code_{code} {}

Schema::FieldType toLeafSchemaType(StructFieldType type) {
    switch (type) {
    case StructFieldType::I8: return Schema::FieldType::I8;
    case StructFieldType::I16: return Schema::FieldType::I16;
    case StructFieldType::I32: return Schema::FieldType::I32;
    case StructFieldType::I64: return Schema::FieldType::I64;
    case StructFieldType::U8: return Schema::FieldType::U8;
    case StructFieldType::U16: return Schema::FieldType::U16;
    case StructFieldType::U32: return Schema::FieldType::U32;
    case StructFieldType::U64: return Schema::FieldType::U64;
    case StructFieldType::F32: return Schema::FieldType::F32;
    case StructFieldType::F64: return Schema::FieldType::F64;
    case StructFieldType::Vec2: return Schema::FieldType::Vec2;
    case StructFieldType::Vec3: return Schema::FieldType::Vec3;
    case StructFieldType::Vec4: return Schema::FieldType::Vec4;
    case StructFieldType::Quat: return Schema::FieldType::Quat;
    case StructFieldType::String: return Schema::FieldType::String;
    case StructFieldType::Bool: return Schema::FieldType::Bool;
    case StructFieldType::Enum: return Schema::FieldType::Enum;
    }
    invalidCore(type);
}

StructFieldType toCoreSchemaType(Schema::FieldType type) {
    switch (type) {
    case Schema::FieldType::I8: return StructFieldType::I8;
    case Schema::FieldType::I16: return StructFieldType::I16;
    case Schema::FieldType::I32: return StructFieldType::I32;
    case Schema::FieldType::I64: return StructFieldType::I64;
    case Schema::FieldType::U8: return StructFieldType::U8;
    case Schema::FieldType::U16: return StructFieldType::U16;
    case Schema::FieldType::U32: return StructFieldType::U32;
    case Schema::FieldType::U64: return StructFieldType::U64;
    case Schema::FieldType::F32: return StructFieldType::F32;
    case Schema::FieldType::F64: return StructFieldType::F64;
    case Schema::FieldType::Vec2: return StructFieldType::Vec2;
    case Schema::FieldType::Vec3: return StructFieldType::Vec3;
    case Schema::FieldType::Vec4: return StructFieldType::Vec4;
    case Schema::FieldType::Quat: return StructFieldType::Quat;
    case Schema::FieldType::String: return StructFieldType::String;
    case Schema::FieldType::Bool: return StructFieldType::Bool;
    case Schema::FieldType::Enum: return StructFieldType::Enum;
    }
    invalidLeaf(type);
}

std::string schemaTypeName(StructFieldType type) {
    return std::string{Schema::fieldTypeName(toLeafSchemaType(type))};
}

} // namespace Pelican::internal

