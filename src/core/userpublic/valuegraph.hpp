#pragma once

#include "animation/curve.hpp"
#include "export.hpp"
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::ValueGraph {

using Inputs = std::map<std::string, double, std::less<>>;
using Outputs = std::map<std::string, std::vector<double>, std::less<>>;

// Immutable scalar dataflow. Inputs supply clocks or other external values;
// evaluation does not advance a clock or perform side effects.
class PELICAN_API Graph {
  public:
    explicit Graph(std::string_view document);
    const std::string &serialize() const noexcept { return document_; }
    Outputs evaluate(const Inputs &inputs) const;

  private:
    enum class Op { input, constant, add, multiply, curve };
    struct Node {
        std::string id;
        Op op{};
        std::string input_name;
        double constant{};
        std::vector<std::size_t> dependencies;
        std::optional<Animation::ScalarCurve> curve;
    };
    std::string document_;
    std::vector<Node> nodes_;
    std::vector<std::size_t> order_;
    std::map<std::string, std::vector<std::size_t>, std::less<>> outputs_;
};

} // namespace Pelican::ValueGraph
