#pragma once

#include "../container.hpp"
#include "modeltemplate.hpp"
#include <unordered_map>

namespace Pelican {

class ModelTemplateContainer {
  private:
    std::unordered_map<ModelTemplateId, ModelTemplate> model_db;

  public:
    ModelTemplateContainer(DependencyContainer &con);
    ~ModelTemplateContainer();
};

} // namespace Pelican
