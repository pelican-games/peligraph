#include "graphloopexpression.hpp"

#include <bit>
#include <cctype>
#include <limits>
#include <numeric>
#include <vector>

namespace Pelican {
namespace {
using Json = nlohmann::json;
using Int = std::int64_t;
constexpr Int kMax = (std::numeric_limits<Int>::max)();

[[noreturn]] void fail(std::string_view reason, const std::string &detail) {
    throw GraphLoopExpressionError(reason, detail);
}

// Every intermediate stays within (-2^63, 2^63): INT64_MIN is left out so a
// negation can never overflow.
Int checkedAdd(Int a, Int b) {
    if ((b > 0 && a > kMax - b) || (b < 0 && a < -kMax - b)) fail(graphLoopExpressionOverflow, "addition");
    return a + b;
}

Int checkedMul(Int a, Int b) {
    if (a == 0 || b == 0) return 0;
    const auto magnitude_a = a < 0 ? -a : a;
    const auto magnitude_b = b < 0 ? -b : b;
    if (magnitude_a > kMax / magnitude_b) fail(graphLoopExpressionOverflow, "multiplication");
    return a * b;
}

// An exact fraction n/d with d > 0 in lowest terms. Integers have d == 1.
struct Rational {
    Int n = 0;
    Int d = 1;
    bool integer() const { return d == 1; }
    bool truthy() const { return n != 0; }
};

Rational normalized(Int n, Int d) {
    if (d == 0) fail(graphLoopExpressionDivisionByZero, "division by zero");
    if (d < 0) {
        n = -n;
        d = -d;
    }
    const auto divisor = std::gcd(n < 0 ? -n : n, d);
    if (divisor > 1) {
        n /= divisor;
        d /= divisor;
    }
    return {n, d};
}

Rational integerValue(Int n) { return {n, 1}; }

class Parser {
    std::string_view text_;
    std::size_t at_ = 0;
    std::size_t depth_ = 0;
    GraphLoopExpressionKind kind_;
    const GraphLoopExpressionScope &scope_;
    const GraphLoopExpressionValues *values_;
    const GraphLoopConstantResolver *constants_;

    bool integerOnly() const { return kind_ != GraphLoopExpressionKind::real; }

    void skipSpace() {
        while (at_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[at_]))) ++at_;
    }
    bool take(std::string_view token) {
        skipSpace();
        if (text_.substr(at_, token.size()) != token) return false;
        at_ += token.size();
        return true;
    }
    void expect(std::string_view token) {
        if (!take(token)) fail(graphLoopExpressionSyntax, "expected '" + std::string{token} + "' at " + std::to_string(at_));
    }
    void open() {
        expect("(");
        if (++depth_ > graphLoopExpressionNestingLimit)
            fail(graphLoopExpressionTooLong, "parentheses nest deeper than " +
                                                 std::to_string(graphLoopExpressionNestingLimit));
    }
    void close() {
        expect(")");
        --depth_;
    }

    Rational add(const Rational &a, const Rational &b, bool live) {
        if (!live) return {};
        if (a.integer() && b.integer()) return integerValue(checkedAdd(a.n, b.n));
        return normalized(checkedAdd(checkedMul(a.n, b.d), checkedMul(b.n, a.d)), checkedMul(a.d, b.d));
    }
    Rational negate(const Rational &a) { return {-a.n, a.d}; }
    Rational multiply(const Rational &a, const Rational &b, bool live) {
        if (!live) return {};
        return normalized(checkedMul(a.n, b.n), checkedMul(a.d, b.d));
    }
    Rational divide(const Rational &a, const Rational &b, bool live) {
        if (!live) return {};
        if (b.n == 0) fail(graphLoopExpressionDivisionByZero, "division by zero");
        if (integerOnly()) return integerValue(a.n / b.n);
        return normalized(checkedMul(a.n, b.d), checkedMul(a.d, b.n));
    }
    Rational modulo(const Rational &a, const Rational &b, bool live) {
        if (!live) return {};
        if (!a.integer() || !b.integer()) fail(graphLoopExpressionNotInteger, "% takes integers");
        if (b.n == 0) fail(graphLoopExpressionDivisionByZero, "modulo by zero");
        return integerValue(a.n % b.n);
    }
    int compare(const Rational &a, const Rational &b) {
        const auto lhs = checkedMul(a.n, b.d);
        const auto rhs = checkedMul(b.n, a.d);
        return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
    }

    Rational expression(bool live) { return orExpression(live); }

    Rational orExpression(bool live) {
        auto value = andExpression(live);
        while (take("||")) {
            const bool decided = live && value.truthy();
            const auto rhs = andExpression(live && !decided);
            if (live) value = integerValue(decided || rhs.truthy() ? 1 : 0);
        }
        return value;
    }
    Rational andExpression(bool live) {
        auto value = comparison(live);
        while (take("&&")) {
            const bool decided = live && !value.truthy();
            const auto rhs = comparison(live && !decided);
            if (live) value = integerValue(!decided && rhs.truthy() ? 1 : 0);
        }
        return value;
    }
    Rational comparison(bool live) {
        auto value = sum(live);
        for (const auto *op : {"==", "!=", "<=", ">=", "<", ">"}) {
            if (!take(op)) continue;
            const auto rhs = sum(live);
            if (!live) return {};
            const auto order = compare(value, rhs);
            const std::string_view name{op};
            const bool result = name == "==" ? order == 0 : name == "!=" ? order != 0
                : name == "<=" ? order <= 0 : name == ">=" ? order >= 0 : name == "<" ? order < 0 : order > 0;
            return integerValue(result ? 1 : 0);
        }
        return value;
    }
    Rational sum(bool live) {
        auto value = product(live);
        for (;;) {
            if (take("+")) value = add(value, product(live), live);
            else if (take("-")) value = add(value, negate(product(live)), live);
            else return value;
        }
    }
    Rational product(bool live) {
        auto value = unary(live);
        for (;;) {
            skipSpace();
            // `&&` / `||` are never a product operator, and neither is a
            // second character of a comparison.
            if (take("*")) value = multiply(value, unary(live), live);
            else if (take("/")) value = divide(value, unary(live), live);
            else if (take("%")) value = modulo(value, unary(live), live);
            else return value;
        }
    }
    Rational unary(bool live) {
        skipSpace();
        if (text_.substr(at_, 2) != "!=" && take("!")) {
            const auto value = unary(live);
            return live ? integerValue(value.truthy() ? 0 : 1) : Rational{};
        }
        if (take("-")) return negate(unary(live));
        if (take("+")) return unary(live);
        return primary(live);
    }

    Rational number() {
        const auto start = at_;
        Int n = 0;
        Int d = 1;
        bool fraction = false;
        while (at_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[at_])) || text_[at_] == '.')) {
            if (text_[at_] == '.') {
                if (fraction) fail(graphLoopExpressionSyntax, "a number has one point");
                fraction = true;
                ++at_;
                if (at_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[at_])))
                    fail(graphLoopExpressionSyntax, "a digit must follow the point");
                continue;
            }
            n = checkedAdd(checkedMul(n, 10), text_[at_] - '0');
            if (fraction) d = checkedMul(d, 10);
            ++at_;
        }
        if (fraction && integerOnly())
            fail(graphLoopExpressionNotInteger, "an integer is required: " + std::string{text_.substr(start, at_ - start)});
        return normalized(n, d);
    }

    std::string identifier() {
        const auto start = at_;
        while (at_ < text_.size() &&
               (std::isalnum(static_cast<unsigned char>(text_[at_])) || text_[at_] == '_'))
            ++at_;
        return std::string{text_.substr(start, at_ - start)};
    }

    Rational variable(const std::string &name, bool live) {
        const auto read = [&](bool allowed, const std::optional<Int> &value) {
            if (!allowed) fail(graphLoopExpressionUnknownVariable, name + " is not available here");
            if (!live) return Rational{};
            if (!value) fail(graphLoopExpressionUnknownVariable, name + " has no value here");
            return integerValue(*value);
        };
        if (name == "element") return read(scope_.element, values_ ? values_->element : std::nullopt);
        if (name == "index") return read(scope_.index, values_ ? values_->index : std::nullopt);
        if (name == "count") return read(scope_.count, values_ ? values_->count : std::nullopt);
        fail(graphLoopExpressionUnknownVariable, "unknown variable " + name);
    }

    Rational call(const std::string &name, bool live) {
        if (name == "min" || name == "max") {
            open();
            auto value = expression(live);
            while (take(",")) {
                const auto other = expression(live);
                if (live && (name == "min" ? compare(other, value) < 0 : compare(other, value) > 0)) value = other;
            }
            close();
            return value;
        }
        if (name == "pow2") {
            open();
            const auto exponent = expression(live);
            close();
            if (!live) return {};
            if (!exponent.integer()) fail(graphLoopExpressionNotInteger, "pow2 takes an integer");
            if (exponent.n < 0 && integerOnly()) fail(graphLoopExpressionNotInteger, "pow2 of a negative number");
            const auto magnitude = exponent.n < 0 ? -exponent.n : exponent.n;
            if (magnitude > 62) fail(graphLoopExpressionOverflow, "pow2 past 2^62");
            const Int power = Int{1} << magnitude;
            return exponent.n < 0 ? Rational{1, power} : integerValue(power);
        }
        if (name == "log2") {
            // RS-15: the integer base-2 logarithm, rounded down (log2(255) is
            // 7), as floor(log2(n)) + 1 counts a mip chain. n is a positive
            // integer.
            open();
            const auto argument = expression(live);
            close();
            if (!live) return {};
            if (!argument.integer()) fail(graphLoopExpressionNotInteger, "log2 takes an integer");
            if (argument.n < 1) fail(graphLoopExpressionNotInteger, "log2 of a number below 1");
            return integerValue(static_cast<Int>(std::bit_width(static_cast<std::uint64_t>(argument.n))) - 1);
        }
        if (name == "mip_levels_of" || name == "layers_of") {
            open();
            skipSpace();
            if (at_ >= text_.size() || text_[at_] != '"') fail(graphLoopExpressionSyntax, name + " takes a target name");
            const auto end = text_.find('"', at_ + 1);
            if (end == std::string_view::npos) fail(graphLoopExpressionSyntax, "unterminated string");
            const std::string target{text_.substr(at_ + 1, end - at_ - 1)};
            if (target.empty()) fail(graphLoopExpressionSyntax, name + " takes a target name");
            at_ = end + 1;
            close();
            if (!live) return {};
            if (constants_ == nullptr || !*constants_)
                fail(graphLoopExpressionNotStatic, name + "(\"" + target + "\") is known only when the graph is recompiled");
            return integerValue((*constants_)(name, target));
        }
        fail(graphLoopExpressionSyntax, "unknown function " + name);
    }

    Rational primary(bool live) {
        skipSpace();
        if (at_ >= text_.size()) fail(graphLoopExpressionSyntax, "the expression ends early");
        const auto c = text_[at_];
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') return number();
        if (c == '(') {
            open();
            const auto value = expression(live);
            close();
            return value;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const auto name = identifier();
            skipSpace();
            if (at_ < text_.size() && text_[at_] == '(') return call(name, live);
            return variable(name, live);
        }
        fail(graphLoopExpressionSyntax, std::string{"unexpected '"} + c + "' at " + std::to_string(at_));
    }

  public:
    Parser(std::string_view text, GraphLoopExpressionKind kind, const GraphLoopExpressionScope &scope,
           const GraphLoopExpressionValues *values, const GraphLoopConstantResolver *constants)
        : text_(text), kind_(kind), scope_(scope), values_(values), constants_(constants) {}

    Rational whole(bool live) {
        const auto value = expression(live);
        skipSpace();
        if (at_ != text_.size()) fail(graphLoopExpressionSyntax, "unexpected text at " + std::to_string(at_));
        return value;
    }
};

const std::string &expressionText(const Json &value, bool strict) {
    if (!isGraphLoopExpression(value)) fail(graphLoopExpressionSyntax, "an expression is {\"$expr\"} or {\"$format\"}");
    const bool format = value.contains("$format");
    for (const auto &[key, member] : value.items()) {
        (void)member;
        if (key == (format ? "$format" : "$expr")) continue;
        if (!strict && key.starts_with("$") && key != "$expr" && key != "$format") continue;
        fail(graphLoopExpressionSyntax, "an expression object holds only $expr or $format");
    }
    const auto &text = value.at(format ? "$format" : "$expr");
    if (!text.is_string()) fail(graphLoopExpressionSyntax, "an expression is a string");
    const auto &result = text.get_ref<const std::string &>();
    if (result.size() > graphLoopExpressionLengthLimit)
        fail(graphLoopExpressionTooLong, "longer than " + std::to_string(graphLoopExpressionLengthLimit) + " characters");
    return result;
}

// The pieces of a `$format`: literal text, and the placeholders between.
struct FormatPiece {
    bool placeholder = false;
    std::string text;
};

std::vector<FormatPiece> formatPieces(const std::string &format) {
    std::vector<FormatPiece> pieces{{}};
    for (std::size_t at = 0; at < format.size(); ++at) {
        const auto c = format[at];
        if (c == '{' && at + 1 < format.size() && format[at + 1] == '{') {
            pieces.back().text += '{';
            ++at;
        } else if (c == '}' && at + 1 < format.size() && format[at + 1] == '}') {
            pieces.back().text += '}';
            ++at;
        } else if (c == '{') {
            const auto end = format.find('}', at + 1);
            if (end == std::string::npos) fail(graphLoopExpressionSyntax, "unterminated {");
            pieces.push_back({true, format.substr(at + 1, end - at - 1)});
            pieces.push_back({});
            at = end;
        } else if (c == '}') {
            fail(graphLoopExpressionSyntax, "a lone } is written }}");
        } else {
            pieces.back().text += c;
        }
    }
    return pieces;
}
} // namespace

bool isGraphLoopExpression(const Json &value) {
    return value.is_object() && (value.contains("$expr") || value.contains("$format"));
}

bool isGraphLoopFormat(const Json &value) { return value.is_object() && value.contains("$format"); }

void checkGraphLoopExpression(const Json &value, GraphLoopExpressionKind kind, const GraphLoopExpressionScope &scope) {
    const auto &text = expressionText(value, true);
    if (isGraphLoopFormat(value) != (kind == GraphLoopExpressionKind::format))
        fail(graphLoopExpressionFieldNotAllowed, kind == GraphLoopExpressionKind::format
            ? "a string is written {\"$format\": ...}" : "a number is written {\"$expr\": ...}");
    if (kind == GraphLoopExpressionKind::format) {
        for (const auto &piece : formatPieces(text))
            if (piece.placeholder)
                Parser{piece.text, GraphLoopExpressionKind::integer, scope, nullptr, nullptr}.whole(false);
        return;
    }
    Parser{text, kind, scope, nullptr, nullptr}.whole(false);
}

Json evaluateGraphLoopExpression(const Json &value, GraphLoopExpressionKind kind, const GraphLoopExpressionValues &values,
                                 const GraphLoopConstantResolver &constants) {
    const auto &text = expressionText(value, false);
    if (isGraphLoopFormat(value) != (kind == GraphLoopExpressionKind::format))
        fail(graphLoopExpressionFieldNotAllowed, "the expression is of the wrong kind for its place");
    const GraphLoopExpressionScope all{true, true, true};
    if (kind == GraphLoopExpressionKind::format) {
        std::string result;
        for (const auto &piece : formatPieces(text)) {
            if (!piece.placeholder) {
                result += piece.text;
                continue;
            }
            const auto number = Parser{piece.text, GraphLoopExpressionKind::integer, all, &values, &constants}.whole(true);
            result += std::to_string(number.n);
        }
        return result;
    }
    const auto number = Parser{text, kind, all, &values, &constants}.whole(true);
    if (kind == GraphLoopExpressionKind::integer) return number.n;
    // The one conversion to a double.
    return static_cast<double>(number.n) / static_cast<double>(number.d);
}

} // namespace Pelican
