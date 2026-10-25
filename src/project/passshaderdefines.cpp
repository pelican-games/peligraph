#include "passshaderdefines.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace Pelican {

namespace {

std::string defineErrorMessage(const PassShaderDefineErrorInfo &info) {
    std::string message = "Pass '" + info.pass + "' shader.defines entry " +
                          std::to_string(info.index) + " ('" + info.define +
                          "') ";
    if (info.reason == passShaderDefineReserved) {
        message += "defines " + info.name +
                   ", which the engine defines itself when it compiles the "
                   "pass";
    } else if (info.reason == passShaderDefineDuplicate) {
        message += "defines " + info.name + " a second time";
    } else {
        message += "is not NAME or NAME=VALUE with NAME a C identifier";
    }
    return message;
}

bool identifier(std::string_view name) {
    if (name.empty()) return false;
    const auto first = static_cast<unsigned char>(name.front());
    if (!std::isalpha(first) && name.front() != '_') return false;
    return std::all_of(name.begin(), name.end(), [](char ch) {
        const auto value = static_cast<unsigned char>(ch);
        return std::isalnum(value) || ch == '_';
    });
}

} // namespace

PassShaderDefineError::PassShaderDefineError(PassShaderDefineErrorInfo info)
    : std::runtime_error{defineErrorMessage(info)}, info_{std::move(info)} {}

bool isEngineReservedShaderDefine(std::string_view name) {
    static constexpr std::array exact{
        passTextureInputCountDefine,
        std::string_view{"PELICAN_VARIADIC_INPUT_COUNT"},
        std::string_view{"PELICAN_VARIADIC_INPUTS_LIMIT"},
        std::string_view{"PELICAN_MULTIVIEW"},
        std::string_view{"PELICAN_VIEW_COUNT"},
        std::string_view{"PELICAN_OUTPUT_UNORM_FALLBACK"},
    };
    if (std::find(exact.begin(), exact.end(), name) != exact.end()) {
        return true;
    }
    // PELICAN_INPUT_<n>_LAYERED / _LOCAL_READ come from the pass's inputs,
    // PELICAN_FEATURE_* from the enabled features.
    return name.starts_with("PELICAN_INPUT_") ||
           name.starts_with("PELICAN_FEATURE_");
}

std::vector<std::string> parsePassShaderDefines(const nlohmann::json &shader,
                                                const std::string &pass_name) {
    const auto found = shader.find("defines");
    if (found == shader.end()) return {};
    if (!found->is_array()) {
        throw std::runtime_error("Pass shader.defines must be an array: " +
                                 pass_name);
    }
    std::vector<std::string> defines;
    std::vector<std::string> names;
    for (std::size_t index = 0; index < found->size(); ++index) {
        const auto &entry = found->at(index);
        PassShaderDefineErrorInfo info{
            .pass = pass_name,
            .define = entry.is_string() ? entry.get<std::string>()
                                        : entry.dump(),
            .index = index,
            .reason = passShaderDefineMalformed,
        };
        if (!entry.is_string()) throw PassShaderDefineError{std::move(info)};
        const auto &text = entry.get_ref<const std::string &>();
        const auto separator = text.find('=');
        const auto name = text.substr(0, separator);
        if (!identifier(name) ||
            text.find_first_of("\r\n") != std::string::npos) {
            info.name = identifier(name) ? name : std::string{};
            throw PassShaderDefineError{std::move(info)};
        }
        info.name = name;
        if (isEngineReservedShaderDefine(name)) {
            info.reason = passShaderDefineReserved;
            throw PassShaderDefineError{std::move(info)};
        }
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            info.reason = passShaderDefineDuplicate;
            throw PassShaderDefineError{std::move(info)};
        }
        names.push_back(name);
        defines.push_back(text);
    }
    return defines;
}

} // namespace Pelican
