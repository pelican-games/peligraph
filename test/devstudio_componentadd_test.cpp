// Q-6: the Add component list, on real Qt widgets. A candidate this build
// cannot take is greyed with a short word after its name, sorts after the
// rest, and selecting it turns Add and the declaration off. The dialog puts
// its combo box, Add button and declaration through the same two functions.

#include "componentaddlist.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QBrush>
#include <QComboBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_componentadd_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

Json catalog() {
    return {{"object_id", 41}, {"scene_revision", 1}, {"candidates", Json::array({
        {{"key", "physics_body"}, {"name", "physics_body"}, {"available", false},
         {"unavailable_reason", "physics.module_unavailable@1"}},
        {{"key", "collider"}, {"name", "collider"}, {"available", true}},
        {{"key", "simplemodelview"}, {"name", "simplemodelview"}, {"available", false},
         {"unavailable_reason", "A model reference is required."}},
    })}};
}

TEST_CASE("Q-6 a candidate this build cannot take is greyed and cannot be added",
          "[devstudio][inspector][component-addition][q6]") {
    application();
    QComboBox types;
    QPushButton add;
    QPlainTextEdit declaration;
    fillComponentAddTypes(types, componentAddRows(catalog()));
    REQUIRE(types.count() == 3);

    // The one that cannot be added is last, greyed, and says why in two words.
    const int physics = types.findData(QStringLiteral("physics_body"));
    REQUIRE(physics == 2);
    CHECK(types.itemText(physics) == QStringLiteral("physics_body — not in this build"));
    const auto grey = types.itemData(physics, Qt::ForegroundRole);
    REQUIRE(grey.isValid());
    CHECK(grey.value<QBrush>().color() == types.palette().color(QPalette::Disabled, QPalette::Text));
    CHECK_FALSE(types.itemData(physics, ComponentAddAddableRole).toBool());
    for (const auto *key : {"collider", "simplemodelview"}) {
        const int index = types.findData(QString::fromLatin1(key));
        CAPTURE(key);
        CHECK_FALSE(types.itemData(index, Qt::ForegroundRole).isValid());
        CHECK(types.itemData(index, ComponentAddAddableRole).toBool());
    }

    // An addable row: Add and the declaration are on.
    types.setCurrentIndex(types.findData(QStringLiteral("collider")));
    CHECK(applyComponentAddAvailability(types, true, add, declaration));
    CHECK(add.isEnabled());
    CHECK_FALSE(declaration.isReadOnly());

    // Selecting the greyed row turns both off, though the session is ready.
    types.setCurrentIndex(physics);
    CHECK_FALSE(applyComponentAddAvailability(types, true, add, declaration));
    CHECK_FALSE(add.isEnabled());
    CHECK(declaration.isReadOnly());
    const auto row = currentComponentAddRow(types);
    REQUIRE(row);
    CHECK(row->candidate == 0);
    CHECK(row->blocked == "This build has no physics module (physics.module_unavailable@1)");

    // A default that only needs input stays addable.
    types.setCurrentIndex(types.findData(QStringLiteral("simplemodelview")));
    CHECK(applyComponentAddAvailability(types, true, add, declaration));
    CHECK(add.isEnabled());
    CHECK_FALSE(declaration.isReadOnly());

    // A session that is not ready turns both off for any row.
    CHECK(applyComponentAddAvailability(types, false, add, declaration));
    CHECK_FALSE(add.isEnabled());
    CHECK(declaration.isReadOnly());
}

} // namespace
} // namespace PelicanStudio
