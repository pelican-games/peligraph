#pragma once

#include "./log.hpp"
#include <memory>
#include <optional>
#include <typeindex>
#include <unordered_map>

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

class DependencyContainer {
    class IStore {
      public:
        virtual ~IStore() = default;
    };
    template <class T> class Store : public IStore {
        T p;

      public:
        Store(DependencyContainer &con) : p{con} {}
        T &get() { return p; }
    };
    std::unordered_map<std::type_index, std::unique_ptr<IStore>> objs;
    std::vector<std::type_index> order;

  public:
    template <class T> T &get() {
        const auto id = std::type_index{typeid(T)};
        const auto it = objs.find(id);
        if (it == objs.end()) {
            LOG_INFO(logger, "Module [{}] initializing...", id.name());
            auto obj_ptr = std::make_unique<Store<T>>(*this);
            auto &obj = *obj_ptr.get();
            objs.insert({id, std::move(obj_ptr)});
            LOG_INFO(logger, "Module [{}] initialized", id.name());
            order.push_back(id);
            return obj.get();
        } else {
            return static_cast<Store<T> *>(it->second.get())->get();
        }
    }
    ~DependencyContainer() {
        while (!order.empty()) {
            const auto id = order.back();
            LOG_INFO(logger, "Module [{}] destroying...", id.name());
            objs.erase(id);
            LOG_INFO(logger, "Module [{}] destroyed", id.name());
            order.pop_back();
        }
    }
};

} // namespace Pelican