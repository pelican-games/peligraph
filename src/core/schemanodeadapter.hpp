#pragma once

#include "userpublic/details/schema/structfieldschema.hpp"

#include <schema.hpp>

namespace Pelican::internal {

// The only descriptor -> owning-model boundary.  V3 registration entrances
// call this while the declaring DLL and its string literals are still live.
Schema::Node hostSchemaNode(const SchemaNode &descriptor,
                            std::size_t node_index = 0);

} // namespace Pelican::internal
