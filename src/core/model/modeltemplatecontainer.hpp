#pragma once

#include "../container.hpp"
#include "modeltemplate.hpp"
#include <unordered_map>

namespace Pelican {

DECLARE_MODULE(ModelTemplateContainer) {
  private:
    std::unordered_map<ModelTemplateId, ModelTemplate> model_db;

  public:
    ModelTemplateContainer();
    ~ModelTemplateContainer();
};

} // namespace Pelican
