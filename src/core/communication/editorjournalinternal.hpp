#pragma once

// Internal helper surface shared by the editor journal translation units.
//
// The editor journal was one 6.4k-line file whose anonymous namespace held
// every helper. `editorjournalops.cpp` (the prepare* operation family) and
// `editorjournalintegrity.cpp` (unpack journal integrity) now live in their
// own translation units, so the helpers they share with `editorjournal.cpp`
// need a name with external linkage. This header only declares them; the
// definitions stay in `editorjournal.cpp`. Nothing here is part of the public
// editor surface - do not include it outside src/core/communication.

#include "editorjournal.hpp"

#include "../loader/authoringscenedocument.hpp"
#include "../loader/editorprojectiontransaction.hpp"
#include "../loader/resolvedscene.hpp"
#include "../schemanodeadapter.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {
namespace EditorJournalInternal {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

// Carries an editor error code plus its wire payload out of a helper. Callers
// convert it to the rejected/failed ticket result at the frame boundary.
class EditFailure final : public std::runtime_error {
  public:
    EditorEditErrorCode code;
    OrderedJson payload;

    EditFailure(EditorEditErrorCode error_code, OrderedJson error_payload,
                std::string message);
};

[[noreturn]] void editFailure(EditorEditErrorCode code, OrderedJson payload,
                              std::string message);

const Json &requireObject(const Json &value, std::string_view context);
void requireOnly(const Json &value,
                 std::initializer_list<std::string_view> fields,
                 std::string_view context);
std::uint64_t exactUnsigned(const Json &value, std::string_view context,
                            bool nonzero = false);
std::uint32_t exactU32(const Json &value, std::string_view context,
                       bool nonzero = false);
std::string requireString(const Json &value, std::string_view field,
                          std::string_view context);
std::optional<std::string> optionalString(const Json &value,
                                          std::string_view field,
                                          std::string_view context);

struct ObjectLocation {
    std::string scene_id;
    std::size_t scene_index = 0;
    std::size_t object_index = 0;
    AuthoringObjectId object_id{};
    std::optional<std::string> name;
    std::optional<std::string> parent;
    Json authored;
};

std::vector<ObjectLocation> objectLocations(const AuthoringSceneDocument &document);
ObjectLocation requireObjectLocation(
    const AuthoringSceneDocument &document, AuthoringObjectId object_id,
    EditorEditErrorCode error = EditorEditErrorCode::closure_unresolvable);
std::optional<AuthoringObjectId>
objectIdByName(const AuthoringSceneDocument &document, std::string_view scene_id,
               std::string_view name);
std::pair<std::size_t, Json> requireComponent(const ObjectLocation &object,
                                              std::string_view component_name);
std::pair<std::size_t, Json> requireComponentAt(
    const ObjectLocation &object, std::size_t component_index,
    std::string_view component_name);
bool hasComponent(const ObjectLocation &object, std::string_view component_name);

Json canonicalComponent(const Json &component, AuthoringObjectId object_id,
                        std::string_view component_name,
                        const std::function<bool(std::string_view)> &
                            component_exists = {});
Json canonicalBehaviorComponent(const Json &component,
                                AuthoringObjectId object_id);

EditorProjectionCommand makeReplaceComponentCommand(
    const ObjectLocation &object, std::size_t component_index,
    std::string component_name, Json component);
EditorProjectionCommand makeInsertComponentCommand(
    const ObjectLocation &object, std::size_t component_index, Json component);
EditorProjectionCommand makeEraseComponentCommand(
    const ObjectLocation &object, std::size_t component_index,
    std::string component_name);

// One prepared operation: the projection commands that apply it, the forward
// and inverse journal wire, and the stable read/write sets used for preview
// lease and undo conflict detection.
struct PreparedUnpack {
    ResolvedScene generated_scene;
    std::string scene_id;
    AuthoringObjectId object_id{};
    std::string instance_id;
    std::vector<OriginTransitionAuthority> authority;
};

struct PreparedOperation {
    std::vector<EditorProjectionCommand> commands;
    OrderedJson forward;
    OrderedJson inverse;
    std::vector<AuthoringObjectId> affected;
    std::string stable_target;
    std::vector<std::string> read_set;
    std::vector<std::string> write_set;
    OrderedJson structural_domain = OrderedJson::object();
    std::optional<PreparedUnpack> unpack;
};

std::string stableComponentTarget(AuthoringObjectId object_id,
                                  std::string_view component,
                                  std::string_view field_path = {});
std::string stableBehaviorTarget(AuthoringObjectId object_id,
                                 std::size_t attachment_index,
                                 std::uint64_t attachment_handle,
                                 std::string_view field_path = {});
void copyBehaviorAttachmentWitness(const Json &raw, OrderedJson &forward,
                                   OrderedJson &inverse);
std::uint64_t internalIdentityValue(const Json &operation,
                                    std::string_view field,
                                    std::string_view context,
                                    bool nonzero = false);
std::vector<std::string> pointerTokens(std::string_view pointer,
                                       std::string_view context,
                                       bool allow_empty = false);
bool decimalIndex(std::string_view token) noexcept;

// A component addressed by the AUTHORING0 address form: the object, the
// attachment occurrence, the list path inside it and an optional row / leaf.
struct AddressedComponent {
    ObjectLocation object;
    std::size_t component_index = 0;
    std::string component_name;
    Json component;
    OrderedJson attachment_identity;
    std::string list_path;
    std::vector<std::string> list_tokens;
    std::optional<std::uint32_t> row_id;
    std::string leaf_path;
    std::vector<std::string> leaf_tokens;
};

AddressedComponent requireComponentAddress(const Json &raw,
                                           const AuthoringSceneDocument &document,
                                           std::string_view context,
                                           bool row_optional);
std::optional<Schema::Node> componentSchema(const AddressedComponent &address);
Schema::Node requireListSchema(const AddressedComponent &address);
const Schema::Node &requireRowLeafSchema(const AddressedComponent &address,
                                         const Schema::Node &list_schema);
Json *findListValue(Json &component, const AddressedComponent &address);
Json &createMissingListValue(Json &component, const AddressedComponent &address);
void validateListContainerShape(const Json &list,
                                const AddressedComponent &address);
std::pair<std::size_t, Json &> requireRow(Json &list, std::uint32_t row_id,
                                          const AddressedComponent &address);
void validateRawComponentTransition(const Json &previous, Json &next,
                                    AuthoringObjectId object_id,
                                    std::string_view component_name,
                                    bool preserve_on_replay = false);
void rejectLegacyListPointer(const Json &component,
                             std::string_view component_name,
                             std::span<const std::string> tokens,
                             AuthoringObjectId object_id,
                             std::string_view field_path);

} // namespace EditorJournalInternal
} // namespace Pelican
