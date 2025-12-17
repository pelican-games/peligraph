#include "jsonarchive.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

void JsonArchiveLoader::prop(const char *name, int8_t &val) {
    val = static_cast<const nlohmann::json *>(ptr)->at(name).get<int8_t>();
}
void JsonArchiveLoader::prop(const char *name, int16_t &val) {
    val = static_cast<const nlohmann::json *>(ptr)->at(name).get<int16_t>();
}
void JsonArchiveLoader::prop(const char *name, int32_t &val) {
    val = static_cast<const nlohmann::json *>(ptr)->at(name).get<int32_t>();
}
void JsonArchiveLoader::prop(const char *name, int64_t &val) {
    val = static_cast<const nlohmann::json *>(ptr)->at(name).get<int64_t>();
}
void JsonArchiveLoader::prop(const char *name, uint8_t &val) {
    val = static_cast<const nlohmann::json *>(ptr)->at(name).get<uint8_t>();
}
void JsonArchiveLoader::prop(const char *name, uint16_t &val) {
    val = static_cast<const nlohmann::json *>(ptr)->at(name).get<uint16_t>();
}
void JsonArchiveLoader::prop(const char *name, uint32_t &val) {
    val = static_cast<const nlohmann::json *>(ptr)->at(name).get<uint32_t>();
}
void JsonArchiveLoader::prop(const char *name, uint64_t &val) {
    val = static_cast<const nlohmann::json *>(ptr)->at(name).get<uint64_t>();
}
void JsonArchiveLoader::prop(const char *name, float &val) {
    val = static_cast<const nlohmann::json *>(ptr)->at(name).get<float>();
}
void JsonArchiveLoader::prop(const char *name, double &val) {
    val = static_cast<const nlohmann::json *>(ptr)->at(name).get<double>();
}
void JsonArchiveLoader::prop(const char *name, std::string &val) {
    val = static_cast<const nlohmann::json *>(ptr)->at(name).get<std::string>();
}
void JsonArchiveLoader::prop(const char *name, vec2 &val) {
    auto &dat = static_cast<const nlohmann::json *>(ptr)->at(name);
    val = {.x = dat[0], .y = dat[1]};
}
void JsonArchiveLoader::prop(const char *name, vec3 &val) {
    auto &dat = static_cast<const nlohmann::json *>(ptr)->at(name);
    val = {.x = dat[0], .y = dat[1], .z = dat[2]};
}
void JsonArchiveLoader::prop(const char *name, vec4 &val) {
    auto &dat = static_cast<const nlohmann::json *>(ptr)->at(name);
    val = {.x = dat[0], .y = dat[1], .z = dat[2], .w = dat[3]};
}
void JsonArchiveLoader::prop(const char *name, quat &val) {
    auto &dat = static_cast<const nlohmann::json *>(ptr)->at(name);
    val = {.x = dat[0], .y = dat[1], .z = dat[2], .w = dat[3]};
}

} // namespace Pelican
