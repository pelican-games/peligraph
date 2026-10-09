#include "jsondocument.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <vector>

namespace Pelican {
namespace {
using Json = nlohmann::ordered_json;

struct Token {
    std::size_t begin{}, end{};
    Json value;
    std::vector<Token> children;
    std::vector<std::string> keys;
};

// The JSON parser validates the input first; this cursor only records spans.
Token scan(std::string_view bytes, std::size_t &cursor, const Json &value) {
    while (cursor < bytes.size() && std::string_view{" \t\r\n"}.find(bytes[cursor]) != std::string_view::npos) ++cursor;
    Token token{cursor, 0, value};
    if (bytes[cursor] == '{' || bytes[cursor] == '[') {
        const bool object = bytes[cursor++] == '{';
        while (true) {
            while (std::string_view{" \t\r\n,"}.find(bytes[cursor]) != std::string_view::npos) ++cursor;
            if (bytes[cursor] == '}' || bytes[cursor] == ']') { ++cursor; break; }
            if (object) {
                const auto key_begin = cursor++;
                while (bytes[cursor] != '"') { if (bytes[cursor] == '\\') ++cursor; ++cursor; }
                ++cursor;
                auto key = Json::parse(bytes.substr(key_begin, cursor - key_begin)).get<std::string>();
                while (bytes[cursor++] != ':') {}
                token.children.push_back(scan(bytes, cursor, value.at(key)));
                token.keys.push_back(std::move(key));
            } else {
                token.children.push_back(scan(bytes, cursor, value.at(token.children.size())));
            }
        }
    } else if (bytes[cursor] == '"') {
        ++cursor;
        while (bytes[cursor] != '"') { if (bytes[cursor] == '\\') ++cursor; ++cursor; }
        ++cursor;
    } else {
        while (cursor < bytes.size() && std::string_view{" \t\r\n,]}"}.find(bytes[cursor]) == std::string_view::npos) ++cursor;
    }
    token.end = cursor;
    return token;
}

bool equal(const Json &a, const Json &b) {
    // Object order is presentation, not a semantic edit.
    return nlohmann::json(a) == nlohmann::json(b);
}

std::string scalar(const Json &value) {
    if (!value.is_number_float()) return value.dump();
    const auto number = value.get<double>();
    if (!std::isfinite(number)) return "null";
    if (number == 0 && std::signbit(number)) return "-0.0";
    char buffer[128];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), number);
    return {buffer, result.ptr};
}

std::string render(const Json &value, const Token *old, std::string_view bytes,
                   std::string_view newline, std::size_t depth) {
    if (old && equal(value, old->value))
        return std::string{bytes.substr(old->begin, old->end - old->begin)};
    if (!value.is_structured()) return scalar(value);
    const bool object = value.is_object();
    if (old && object != old->value.is_object()) old = nullptr;
    if (old && !old->value.is_structured()) old = nullptr;

    std::vector<std::string> keys;
    if (object) {
        if (old) for (const auto &key : old->keys)
            if (value.contains(key) && std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
        for (const auto &[key, item] : value.items())
            if (!old || !old->value.contains(key)) keys.push_back(key);
    }
    std::vector<const Token *> sources(value.size(), nullptr);
    for (std::size_t i = 0; old && i < value.size(); ++i) {
        if (object) {
            for (std::size_t j = old->keys.size(); j > 0; --j)
                if (old->keys[j - 1] == keys[i]) { sources[i] = &old->children[j - 1]; break; }
        } else {
            const auto &item = value.at(i);
            // Scene objects carry persistent identity. Components and named
            // asset rows carry a name; matching keeps their tokens on moves.
            const char *identity = item.is_object() && item.contains("authoring_object_id")
                ? "authoring_object_id" : item.is_object() && item.contains("name") ? "name" : nullptr;
            if (identity) {
                const auto available = [&](const Token &child) {
                    return std::find(sources.begin(), sources.begin() + i, &child) == sources.begin() + i;
                };
                // Repeated behavior attachments can share a name. Match an
                // unchanged occurrence first, then the edited occurrence at
                // this position, and never reuse an already matched token.
                for (const auto &child : old->children)
                    if (available(child) && equal(child.value, item)) { sources[i] = &child; break; }
                if (!sources[i] && i < old->children.size()) {
                    const auto &child = old->children[i];
                    if (available(child) && child.value.is_object() && child.value.contains(identity) &&
                        child.value.at(identity) == item.at(identity)) sources[i] = &child;
                }
                if (!sources[i]) for (const auto &child : old->children)
                    if (available(child) && child.value.is_object() && child.value.contains(identity) &&
                        child.value.at(identity) == item.at(identity)) { sources[i] = &child; break; }
                if (!sources[i] && item.contains("authoring_object_id")) {
                    // Legacy documents acquire IDs on their first save.
                    for (std::size_t j = 0; j < old->children.size(); ++j) {
                        const auto &child = old->children[j];
                        if (!child.value.is_object() || child.value.contains("authoring_object_id")) continue;
                        if ((item.contains("name") && child.value.contains("name") &&
                             item.at("name") == child.value.at("name")) ||
                            (!item.contains("name") && !child.value.contains("name") && i == j)) {
                            sources[i] = &child;
                            break;
                        }
                    }
                }
            } else if (i < old->children.size()) sources[i] = &old->children[i];
        }
    }
    bool same_shape = old && old->children.size() == value.size();
    for (std::size_t i = 0; same_shape && i < value.size(); ++i)
        same_shape = sources[i] == &old->children[i];
    if (same_shape) {
        std::string result;
        auto cursor = old->begin;
        for (std::size_t i = 0; i < value.size(); ++i) {
            result += bytes.substr(cursor, sources[i]->begin - cursor);
            result += render(object ? value.at(keys[i]) : value.at(i), sources[i], bytes, newline, depth + 1);
            cursor = sources[i]->end;
        }
        result += bytes.substr(cursor, old->end - cursor);
        return result;
    }
    std::string result = object ? "{" : "[";
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i) result += ',';
        result += newline;
        result += std::string((depth + 1) * 2, ' ');
        if (object) result += Json(keys[i]).dump() + ": ";
        result += render(object ? value.at(keys[i]) : value.at(i), sources[i], bytes, newline, depth + 1);
    }
    if (!value.empty()) { result += newline; result += std::string(depth * 2, ' '); }
    result += object ? '}' : ']';
    return result;
}
} // namespace

std::string renderJsonDocumentValue(const Json &value, std::string_view original_value,
                                    std::string_view enclosing_document, std::size_t depth) {
    const auto newline = enclosing_document.find("\r\n") == std::string_view::npos ? "\n" : "\r\n";
    if (original_value.empty()) return render(value, nullptr, {}, newline, depth);
    const auto original = Json::parse(original_value);
    std::size_t cursor = 0;
    const auto token = scan(original_value, cursor, original);
    return render(value, &token, original_value, newline, depth);
}

std::string renderJsonDocument(const Json &document, std::string_view original_bytes) {
    const auto newline = original_bytes.find("\r\n") == std::string_view::npos ? "\n" : "\r\n";
    if (original_bytes.empty()) return render(document, nullptr, {}, newline, 0) + newline;
    const auto original = Json::parse(original_bytes);
    std::size_t cursor = original_bytes.starts_with("\xef\xbb\xbf") ? 3 : 0;
    const auto token = scan(original_bytes, cursor, original);
    return std::string{original_bytes.substr(0, token.begin)} +
           render(document, &token, original_bytes, newline, 0) +
           std::string{original_bytes.substr(token.end)};
}
} // namespace Pelican
