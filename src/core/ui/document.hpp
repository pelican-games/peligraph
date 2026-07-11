#pragma once

#include "types.hpp"

#include "../userpublic/details/event/payloadschema.hpp"

#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Pelican::ui {

enum class SizeMode { Fixed, Content, Fill };
enum class StackDirection { None, Horizontal, Vertical };
enum class Align { Start, Center, End, Stretch };
enum class Justify { Start, Center, End, SpaceBetween };
enum class Visibility { Visible, Hidden, Collapsed };

struct AxisSpec {
    SizeMode mode = SizeMode::Fixed;
    std::int32_t value = 0;
    std::int32_t min = 0;
    std::int32_t max = INT32_MAX;
    std::uint32_t weight = 1;
};

struct LayoutSpec {
    AxisSpec x;
    AxisSpec y;
    double anchor_min_x = 0;
    double anchor_min_y = 0;
    double anchor_max_x = 0;
    double anchor_max_y = 0;
    RectI offsets{};
    StackDirection stack = StackDirection::None;
    std::int32_t gap = 0;
    RectI padding{};
    Align align = Align::Start;
    Justify justify = Justify::Start;
};

enum class EmitSourceKind { Static, StableId, DragDeltaUi, WidgetValue };

struct EmitField {
    std::string name;
    EmitSourceKind source = EmitSourceKind::Static;
    std::string widget_path;
    std::string static_json;
};

struct EmitBinding {
    std::string trigger;
    std::string event;
    std::vector<EmitField> fields;
};

struct DocumentNode {
    std::string stable_id;
    std::string path;
    std::string type;
    LayoutSpec layout;
    PointI intrinsic_size{};
    Visibility visibility = Visibility::Visible;
    bool overflow_clip = false;
    bool enabled = true;
    bool hit_testable = true;
    std::int32_t layer = 0;
    std::int32_t decl_seq = 0;
    std::vector<EmitBinding> emits;
    std::vector<DocumentNode> children;
};

struct UiDocument {
    std::string key;
    std::string revision;
    std::string direction = "ltr";
    DocumentNode root;
};

using EventLookup = std::function<EventSchemaLookup(std::string_view)>;

struct DocumentParseResult {
    std::optional<UiDocument> document;
    std::vector<UiError> errors;
    explicit operator bool() const noexcept { return document.has_value() && errors.empty(); }
};

DocumentParseResult parseUiDocument(const nlohmann::json &json, EventLookup lookup = {});

} // namespace Pelican::ui
