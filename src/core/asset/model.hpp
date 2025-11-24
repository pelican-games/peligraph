#pragma once

#include "../container.hpp"
#include "../model/modeltemplate.hpp"
#include <unordered_map>

namespace Pelican {

DECLARE_MODULE(ModelAssetContainer) {
    std::unordered_map<std::string, ModelTemplate> model_templates;

  public:
    ModelAssetContainer();

    ModelTemplate &getModelTemplateByName(const std::string &name);
};

} // namespace Pelican
