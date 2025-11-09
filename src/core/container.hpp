#pragma once

#include "./log.hpp"
#include <memory>
#include <typeindex>
#include <unordered_map>

namespace Pelican {

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