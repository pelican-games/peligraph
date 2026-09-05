#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace PelicanStudio {
namespace detail {
inline bool rpcJsonValuesPreserved(const nlohmann::json &original,
                                   const nlohmann::json &converted) {
    if (original.is_number_integer())
        return converted.is_number_integer() && original.dump() == converted.dump();
    if (original.is_structured()) {
        if (original.type() != converted.type() || original.size() != converted.size()) return false;
        if (original.is_array()) {
            for (std::size_t index = 0; index < original.size(); ++index)
                if (!rpcJsonValuesPreserved(original[index], converted[index])) return false;
        } else {
            for (const auto &[key, value] : original.items())
                if (!converted.contains(key) || !rpcJsonValuesPreserved(value, converted.at(key))) return false;
        }
        return true;
    }
    return original == converted;
}
} // namespace detail

// Qt is the transport boundary; never silently round an authored integer.
inline QJsonObject exactJsonRpcObject(const nlohmann::json &request) {
    if (!request.is_object()) throw std::invalid_argument("The request must be a JSON object.");
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(request.dump()));
    const auto converted = nlohmann::json::parse(
        document.toJson(QJsonDocument::Compact).toStdString(), nullptr, false);
    if (!document.isObject() || converted.is_discarded() || !detail::rpcJsonValuesPreserved(request, converted))
        throw std::invalid_argument("A value cannot be represented exactly by the Studio RPC transport. The draft is unchanged.");
    return document.object();
}
} // namespace PelicanStudio
