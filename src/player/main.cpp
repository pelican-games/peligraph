#include <components/predefined.hpp>
#include <details/component/registerer.hpp>
#include <details/ecs/componentdeclare.hpp>
#include <details/ecs/coredist.hpp>
#include <gameobjects.hpp>
#include <iostream>
#include <pelican_core.hpp>

struct MyCharComponent {
    float x;

    template <class TArchive> void ref(TArchive &ar) { ar.prop("x", x); }
};

DECLARE_COMPONENT(MyCharComponent, 32);

class MyCharSystem {
  public:
    using QueryComponents = std::tuple<MyCharComponent *, Pelican::LocalTransformComponent *>;
    int timer = 0;

    void process(QueryComponents components, size_t count) {
        auto m = std::get<MyCharComponent *>(components);
        auto t = std::get<Pelican::LocalTransformComponent *>(components);

        for (int i = 0; i < count; i++) {
            t[i].pos = Pelican::vec3{m[i].x, 0, 0};
            t[i].scale = Pelican::vec3{0.1, 0.1, 0.1};
            t[i].rotation = Pelican::quat{0, 0, 0, 1};

            m[i].x += 0.05;
        }

        // timer++;
        // if (timer == 10)
        //     Pelican::GameObjects::add()
        //         .addComponent<Pelican::TransformComponent>()
        //         .addComponent<Pelican::LocalTransformComponent>(t[0])
        //         .addComponent<Pelican::SimpleModelViewComponent>()
        //         .finish();
    }
};

int main() {
    Pelican::PelicanCore pl;

    auto &cr = Pelican::internal::getComponentRegisterer();
    auto &ecs = Pelican::internal::getEcsCore();

    cr.registerComponent<MyCharComponent>("mychar");

    MyCharSystem sys;
    ecs.registerSystem<MyCharSystem, MyCharComponent, Pelican::LocalTransformComponent>(sys, {}, true);

    pl.run();
    return 0;
}
