#include "schemanodeadapter.hpp"

#include "communication/schemavocabularyadapter.hpp"
#include "userpublic/details/schema/structfieldjson.hpp"

#include <type_traits>

namespace Pelican::internal {

Schema::Node hostSchemaNode(const SchemaNode &descriptor,
                            std::size_t node_index) {
    const auto &source = descriptor.at(node_index);
    Schema::Node result;
    result.tag = static_cast<Schema::NodeTag>(source.tag);
    result.name = source.name;
    result.presence = static_cast<Schema::FieldPresence>(source.presence);
    if (source.tag == SchemaNodeTag::Leaf) {
        result.leaf = Schema::FieldDeclaration{
            .name = std::string{source.name},
            .type = toLeafSchemaType(source.leaf_type),
            .range = [&]() -> Schema::FieldRange {
                switch (source.range.kind) {
                case StructFieldRangeKind::None: return {};
                case StructFieldRangeKind::Signed:
                    return Schema::SignedRange{source.range.signed_min,
                                               source.range.signed_max};
                case StructFieldRangeKind::Unsigned:
                    return Schema::UnsignedRange{source.range.unsigned_min,
                                                 source.range.unsigned_max};
                case StructFieldRangeKind::Floating:
                    return Schema::FloatingRange{
                        source.range.floating_min == 0.0
                            ? 0.0 : source.range.floating_min,
                        source.range.floating_max == 0.0
                            ? 0.0 : source.range.floating_max};
                }
                return {};
            }(),
            .unit = std::string{source.unit},
        };
    }
    if (source.default_value.kind != SchemaDefaultKind::None)
        result.default_value = schemaDefaultJson(source);
    for (std::size_t index = 0; index < source.vocabulary_count; ++index) {
        const auto &value = source.vocabulary[index];
        result.vocabulary.push_back(
            {std::string{value.name}, value.value, value.signed_value});
    }
    for (std::size_t ordinal = 0;
         ordinal < descriptor.childCount(node_index); ++ordinal) {
        result.children.push_back(hostSchemaNode(
            descriptor, descriptor.childIndex(node_index, ordinal)));
    }
    return result;
}

} // namespace Pelican::internal
