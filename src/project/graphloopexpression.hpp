#pragma once

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican {

// LP-1b: the per-element values of a graph.loop (docs/design_graph_loops.md,
// decision 5). An expression is written only in object form, never scanned
// out of a plain string:
//
//   {"$expr":   "<expression>"}         a number
//   {"$format": "TEXT={<expression>}"}   a string; `{{` and `}}` are braces
//
// The grammar (the text is at most 256 characters, parentheses -- a call's
// included -- nest at most 16 deep):
//
//   expression := or
//   or         := and { "||" and }
//   and        := comparison { "&&" comparison }
//   comparison := sum [ ( "==" | "!=" | "<" | "<=" | ">" | ">=" ) sum ]
//   sum        := product { ( "+" | "-" ) product }
//   product    := unary { ( "*" | "/" | "%" ) unary }
//   unary      := ( "-" | "+" | "!" ) unary | primary
//   primary    := number | variable | call | "(" expression ")"
//   call       := ( "min" | "max" ) "(" expression { "," expression } ")"
//               | "pow2" "(" expression ")"
//               | ( "mip_levels_of" | "layers_of" ) "(" string ")"
//   number     := digit { digit } [ "." digit { digit } ]
//   variable   := "element" | "index" | "count"
//   string     := '"' { any character but '"' } '"'
//
// A comparison, `&&`, `||` and `!` answer 1 or 0; `&&` and `||` treat any
// value but 0 as true and do not evaluate their right side when the left
// decides. Where an integer is required, every value is an integer: a literal
// with a fraction is not_integer and `/` truncates toward zero. Where a real
// is required (`extent_scale`), the value is an exact fraction of two 64-bit
// integers and becomes a double once, at the end, so no platform rounding
// enters it. `%` takes integers in both. `pow2(n)` is 2 to the n; n is an
// integer, negative only where a real is required. `mip_levels_of("<target>")`
// and `layers_of("<target>")` are the composed target's mip_levels and
// layers, known when the graph is recompiled; a target the runtime widens (a
// shadow map) or whose mip chain follows its extent is not_static.
// A `$format` placeholder is an integer expression.

// graph.loop_expression_invalid@1 reasons.
inline constexpr std::string_view graphLoopExpressionSyntax = "syntax";
inline constexpr std::string_view graphLoopExpressionUnknownVariable = "unknown_variable";
inline constexpr std::string_view graphLoopExpressionNotInteger = "not_integer";
inline constexpr std::string_view graphLoopExpressionNotStatic = "not_static";
inline constexpr std::string_view graphLoopExpressionDivisionByZero = "division_by_zero";
inline constexpr std::string_view graphLoopExpressionOverflow = "overflow";
inline constexpr std::string_view graphLoopExpressionTooLong = "too_long";
inline constexpr std::string_view graphLoopExpressionFieldNotAllowed = "field_not_allowed";

inline constexpr std::size_t graphLoopExpressionLengthLimit = 256;
inline constexpr std::size_t graphLoopExpressionNestingLimit = 16;

// The failure of one expression; the caller names the node and the pointer
// (graph.loop_expression_invalid@1 {node, pointer, reason}).
class GraphLoopExpressionError : public std::runtime_error {
    std::string reason_;

  public:
    GraphLoopExpressionError(std::string_view reason, const std::string &detail)
        : std::runtime_error(std::string{reason} + ": " + detail), reason_(reason) {}
    const std::string &reason() const noexcept { return reason_; }
};

enum class GraphLoopExpressionKind : std::uint8_t { integer, real, format };

// Which variables a place may read. Composition-time constants are allowed
// everywhere an expression is.
struct GraphLoopExpressionScope {
    bool element = false;
    bool index = false;
    bool count = false;
};

struct GraphLoopExpressionValues {
    std::optional<std::int64_t> element;
    std::optional<std::int64_t> index;
    std::optional<std::int64_t> count;
};

// Answers `mip_levels_of` / `layers_of` for a target. It throws
// GraphLoopExpressionError(not_static) for a target whose value is not known
// when the graph is recompiled; an empty resolver answers not_static for all.
using GraphLoopConstantResolver =
    std::function<std::int64_t(std::string_view function, const std::string &target)>;

// `{"$expr": ...}` or `{"$format": ...}`.
bool isGraphLoopExpression(const nlohmann::json &value);

// The kind of expression the object is: `$format` is a format, `$expr` a
// number (integer or real is the place's to say).
bool isGraphLoopFormat(const nlohmann::json &value);

// Checks the form: one member, the length, the grammar, the nesting, and the
// variables the scope allows. A literal with a fraction where an integer is
// required is not_integer already here.
void checkGraphLoopExpression(const nlohmann::json &value, GraphLoopExpressionKind kind,
                              const GraphLoopExpressionScope &scope);

// Evaluates a checked expression: a JSON integer, a JSON double, or a string.
// Members beside `$expr` / `$format` whose name starts with `$` are
// annotations the expansion adds and are ignored.
nlohmann::json evaluateGraphLoopExpression(const nlohmann::json &value, GraphLoopExpressionKind kind,
                                           const GraphLoopExpressionValues &values,
                                           const GraphLoopConstantResolver &constants);

} // namespace Pelican
