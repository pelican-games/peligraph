#pragma once

#include "passform.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;

namespace PelicanStudio {

// UX-4a. A preset's `material_routing`: which material pass draws the
// deferred materials, which the forward opaque and which the forward
// transparent ones (policy hybrid_auto_v1). One row a route, each a choice of
// the preset's material passes. Off, the preset has no routing table.
class MaterialRoutingForm final : public PassForm {
    Q_OBJECT

  public:
    // The routes, in the order the rows show them.
    static constexpr std::array<const char *, 3> routes{
        "deferred_geometry", "forward_opaque", "forward_transparent"};

    explicit MaterialRoutingForm(const QString &object_prefix,
                                 QWidget *parent = nullptr);

    // material_routing as the preset holds it (null: none).
    void load(const nlohmann::json &routing);
    void reset();
    // The preset's material passes and their contracts. Turning routing on
    // fills each empty route with the pass whose contract is the route's.
    void setPassCandidates(
        const std::vector<std::pair<std::string, std::string>> &passes);

    // null when routing is off.
    [[nodiscard]] nlohmann::json routing() const;

  private:
    void fillEmptyRoutes();

    nlohmann::json original_ = nullptr;
    std::vector<std::pair<std::string, std::string>> passes_;
    QCheckBox *enabled_ = nullptr;
    std::array<QComboBox *, 3> rows_{};
};

} // namespace PelicanStudio
