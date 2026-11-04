#include "inspectorgraphsections.hpp"

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLayout>
#include <QWidget>

namespace PelicanStudio {
namespace {

QLabel *sectionLabel(const QString &text, QWidget &parent) {
    auto *label = new QLabel(text, &parent);
    label->setObjectName(QStringLiteral("pelican.inspectorGraph.section"));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

void addRow(QWidget &parent, const QString &name, const QString &value) {
    if (value.isEmpty()) return;
    auto *box = new QGroupBox(parent.tr("Operation"), &parent);
    box->setObjectName(QStringLiteral("pelican.inspectorGraph.section"));
    auto *form = new QFormLayout(box);
    form->addRow(name, sectionLabel(value, *box));
    parent.layout()->addWidget(box);
}

void fillNothing(QWidget &, const InspectorGraphNodeView &) {}

void fillRender(QWidget &page, const InspectorGraphNodeView &node) {
    QStringList parts;
    if (!node.domain.empty()) parts << page.tr("Domain: %1").arg(QString::fromStdString(node.domain));
    if (!node.evaluation.empty())
        parts << page.tr("Evaluation: %1").arg(QString::fromStdString(node.evaluation));
    if (node.sink) parts << page.tr("Sink node");
    if (parts.isEmpty()) return;
    auto *label = sectionLabel(parts.join(QStringLiteral("\n")), page);
    page.layout()->addWidget(label);
}

void fillDefinition(QWidget &page, const InspectorGraphNodeView &node) {
    addRow(page, page.tr("Definition"), QString::fromStdString(node.definition));
}

constexpr InspectorGraphSection kSections[] = {
    {.op_prefix = "render.", .fill = &fillRender},
    {.op_prefix = "graph.instance", .fill = &fillDefinition},
    {.op_prefix = "graph.loop", .fill = &fillDefinition},
    {.op_prefix = "", .fill = &fillNothing},
};

} // namespace

std::span<const InspectorGraphSection> inspectorGraphSections() {
    return kSections;
}

const InspectorGraphSection &inspectorGraphSectionFor(std::string_view op) {
    const InspectorGraphSection *best = &kSections[std::size(kSections) - 1];
    for (const InspectorGraphSection &section : kSections) {
        if (section.op_prefix.empty()) continue;
        if (!op.starts_with(section.op_prefix)) continue;
        if (section.op_prefix.size() > best->op_prefix.size()) best = &section;
    }
    return *best;
}

} // namespace PelicanStudio
