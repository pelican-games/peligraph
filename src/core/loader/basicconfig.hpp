#include "../container.hpp"

#include <string>

namespace Pelican {

class ProjectBasicConfig {

  public:
    ProjectBasicConfig(DependencyContainer &container);

    std::string windowTitle() const;
    struct window_size {
        int width, height;
    };
    window_size initialWindowSize() const;
    bool initialFullScreenState() const;
    float framerateTarget() const;
};

} // namespace Pelican
