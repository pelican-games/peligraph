#pragma once

#include "../userpublic/details/schema/structfieldschema.hpp"

#include <schema.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican::internal {

enum class SchemaTypeMappingErrorCode : std::uint8_t {
    InvalidCoreOrdinal,
    InvalidLeafOrdinal,
    ResolverRejected,
};

std::string_view schemaTypeMappingErrorCodeName(
    SchemaTypeMappingErrorCode code) noexcept;

class SchemaTypeMappingError : public std::runtime_error {
    SchemaTypeMappingErrorCode code_;

  public:
    SchemaTypeMappingError(SchemaTypeMappingErrorCode code,
                           std::string detail);
    SchemaTypeMappingErrorCode code() const noexcept { return code_; }
};

Schema::FieldType toLeafSchemaType(StructFieldType type);
StructFieldType toCoreSchemaType(Schema::FieldType type);
std::string schemaTypeName(StructFieldType type);

} // namespace Pelican::internal

