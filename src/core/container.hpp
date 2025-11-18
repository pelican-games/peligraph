#pragma once

#include "./log.hpp"
#include <optional>
#include <typeindex>
#include <utility>

#define DECLARE_MODULE(name) class name : public ModuleBase<name>
#define GET_MODULE(name) FastModuleContainer::get<name>()

namespace Pelican {

template <class T> struct ModuleBase {
    static std::optional<T> &__get() {
        static std::optional<T> obj;
        return obj;
    }
};

class FastModuleContainer {
    using CleanerFunctionType = void();
    static inline std::vector<std::pair<std::type_index, CleanerFunctionType *>> cleaners;

  public:
    template <class T> static T &get() {
        std::optional<T> &obj_ref = T::__get();
        if (!obj_ref.has_value()) {
            const auto name = typeid(T).name();
            LOG_INFO(logger, "Module [{}] initializing...", name);
            obj_ref.emplace();
            cleaners.push_back({std::type_index{typeid(T)}, []() { T::__get().reset(); }});
            LOG_INFO(logger, "Module [{}] initialized", name);
        }
        return obj_ref.value();
    }
    ~FastModuleContainer() {
        while (!cleaners.empty()) {
            const auto [id, cleaner] = cleaners.back();
            LOG_INFO(logger, "Module [{}] destroying...", id.name());
            cleaner();
            cleaners.pop_back();
            LOG_INFO(logger, "Module [{}] destroyed", id.name());
        }
    }
};

} // namespace Pelican