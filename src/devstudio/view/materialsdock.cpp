#include "materialsdock.hpp"

#include "graphcanvastagpicker.hpp"
#include "panelhelp.hpp"
#include "studiotheme.hpp"

#include <QAbstractSpinBox>
#include <QAction>
#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

constexpr int SliderSteps = 1000;
// What a texture combo's "None" carries: the slot's own default.
const QString NoTexture;

QString qtext(const std::string &text) { return QString::fromStdString(text); }

QString fieldName(const std::string &param, const char *part = nullptr) {
    QString name = QStringLiteral("pelican.materials.param.") + qtext(param);
    if (part != nullptr) name += QLatin1Char('.') + QLatin1String(part);
    return name;
}

bool sameNumber(double left, double right) { return std::abs(left - right) < 1e-9; }

QString hexOf(const std::array<double, 4> &shown) {
    return QColor::fromRgbF(static_cast<float>(shown[0]), static_cast<float>(shown[1]),
                            static_cast<float>(shown[2]))
        .name();
}

} // namespace

struct MaterialsDock::Impl {
    // One param's row of the editor.
    struct ParamRow {
        SurfaceParam param;
        MaterialFieldKind kind = MaterialFieldKind::Other;
        QLabel *label = nullptr;
        QAction *reset = nullptr;
        QLabel *error = nullptr;
        QSlider *slider = nullptr;
        QDoubleSpinBox *spin = nullptr;
        QSpinBox *integer = nullptr;
        QCheckBox *check = nullptr;
        QPushButton *swatch = nullptr;
        std::vector<QDoubleSpinBox *> components;
        QLabel *other = nullptr;
    };
    struct TextureRow {
        SurfaceTexture texture;
        QLabel *label = nullptr;
        QComboBox *choice = nullptr;
        QLabel *error = nullptr;
    };

    MaterialsDock &owner;
    Hooks hooks;
    MaterialsDockSession session;
    QToolButton *undo_button = nullptr;
    QAction *undo_action = nullptr;
    QTreeWidget *tree = nullptr;
    QPushButton *new_button = nullptr;
    QPushButton *assign_button = nullptr;
    QWidget *new_row = nullptr;
    QComboBox *new_surface = nullptr;
    QLineEdit *new_name = nullptr;
    QLabel *new_path = nullptr;
    QLabel *new_notice = nullptr;
    QPushButton *new_create = nullptr;
    QPushButton *new_cancel = nullptr;
    QLabel *notice = nullptr;
    QPushButton *restart = nullptr;
    QScrollArea *editor_scroll = nullptr;
    QWidget *editor = nullptr;
    QLabel *heading = nullptr;
    QComboBox *surface = nullptr;
    GraphCanvasTagPicker *tags = nullptr;
    QComboBox *alpha_mode = nullptr;
    QCheckBox *double_sided = nullptr;
    QLabel *variants = nullptr;
    QFormLayout *params_form = nullptr;
    QFormLayout *textures_form = nullptr;
    std::map<std::string, ParamRow> param_rows;
    std::map<std::string, TextureRow> texture_rows;
    std::string built_structure;
    QTimer *poll_timer = nullptr;
    QPointer<QColorDialog> color_dialog;
    std::string color_param;
    MaterialList shown_list;
    bool list_shown = false;
    bool filling = false;
    // The project's images, read once per selection of a material.
    std::vector<AssetEntry> images;

    Impl(MaterialsDock &dock, Hooks dock_hooks)
        : owner{dock}, hooks{std::move(dock_hooks)},
          session{MaterialsDockSession::Transport{
              .request = [this](const std::string &method, const Json &params,
                                std::string *error) -> std::int64_t {
                  if (!hooks.send) return 0;
                  const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(params.dump())).object();
                  QString message;
                  const qint64 id = hooks.send(qtext(method), object, &message);
                  if (id == 0 && error != nullptr) *error = message.toStdString();
                  return id;
              },
          }} {
        session.changed = [this] { present(); };
        session.restartRequested = [this](const std::string &reason) {
            if (hooks.restart) hooks.restart(qtext(reason));
        };
        session.listChanged = [this] {
            if (hooks.materials_changed) hooks.materials_changed();
        };
        session.bound = [this](const std::string &model) {
            if (hooks.bound) hooks.bound(model);
        };
        build();
    }

    void build() {
        owner.setFont(studioPanelFont());
        auto *layout = new QVBoxLayout(&owner);
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(4);

        undo_action = new QAction(owner.tr("Undo"), &owner);
        undo_action->setObjectName(QStringLiteral("pelican.materials.undo"));
        undo_action->setShortcut(QKeySequence::Undo);
        undo_action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        owner.addAction(undo_action);
        QObject::connect(undo_action, &QAction::triggered, &owner, [this] { session.undo(); });

        auto *top = new QHBoxLayout;
        top->setSpacing(4);
        new_button = new QPushButton(owner.tr("New material\xE2\x80\xA6"), &owner);
        new_button->setObjectName(QStringLiteral("pelican.materials.newMaterial"));
        new_button->setToolTip(owner.tr("Make a material document on a surface the engine lists "
                                         "(create_material)"));
        QObject::connect(new_button, &QPushButton::clicked, &owner, [this] { openNewRow(); });
        assign_button = new QPushButton(owner.tr("Assign to selected object"), &owner);
        assign_button->setObjectName(QStringLiteral("pelican.materials.assignSelected"));
        QObject::connect(assign_button, &QPushButton::clicked, &owner, [this] { assignSelected(); });
        undo_button = new QToolButton(&owner);
        undo_button->setObjectName(QStringLiteral("pelican.materials.undoButton"));
        undo_button->setDefaultAction(undo_action);
        undo_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        top->addWidget(new_button);
        top->addWidget(assign_button);
        top->addStretch();
        top->addWidget(undo_button);
        layout->addLayout(top);

        buildNewRow(*layout);

        tree = new QTreeWidget(&owner);
        tree->setObjectName(QStringLiteral("pelican.materials.list"));
        tree->setColumnCount(2);
        tree->setHeaderLabels({owner.tr("Material"), owner.tr("Surface")});
        tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        tree->header()->setStretchLastSection(false);
        tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        QObject::connect(tree, &QTreeWidget::currentItemChanged, &owner,
                         [this](QTreeWidgetItem *current, QTreeWidgetItem *) { picked(current); });
        layout->addWidget(tree, 1);

        auto *notice_row = new QHBoxLayout;
        notice = new QLabel(&owner);
        notice->setObjectName(QStringLiteral("pelican.materials.notice"));
        applyPanelNoticeStyle(notice);
        notice->hide();
        restart = new QPushButton(owner.tr("Restart engine"), &owner);
        restart->setObjectName(QStringLiteral("pelican.materials.restartEngine"));
        restart->hide();
        QObject::connect(restart, &QPushButton::clicked, &owner, [this] {
            if (hooks.restart) hooks.restart(qtext(session.restartReason()));
        });
        notice_row->addWidget(notice, 1);
        notice_row->addWidget(restart);
        layout->addLayout(notice_row);

        editor_scroll = new QScrollArea(&owner);
        editor_scroll->setObjectName(QStringLiteral("pelican.materials.editor"));
        editor_scroll->setWidgetResizable(true);
        editor_scroll->setFrameShape(QFrame::NoFrame);
        layout->addWidget(editor_scroll, 2);

        poll_timer = new QTimer(&owner);
        poll_timer->setInterval(MaterialsDockSession::PollMs);
        QObject::connect(poll_timer, &QTimer::timeout, &owner, [this] { session.poll(); });
        present();
    }

    // --- New material… (decision 4)

    void buildNewRow(QVBoxLayout &layout) {
        new_row = new QFrame(&owner);
        new_row->setObjectName(QStringLiteral("pelican.materials.new"));
        static_cast<QFrame *>(new_row)->setFrameShape(QFrame::StyledPanel);
        auto *form = new QFormLayout(new_row);
        form->setContentsMargins(4, 4, 4, 4);
        new_surface = new QComboBox(new_row);
        new_surface->setObjectName(QStringLiteral("pelican.materials.new.surface"));
        new_surface->setToolTip(owner.tr("The surfaces the engine lists (list_surfaces): the shader "
                                         "and the parameters the material takes"));
        new_name = new QLineEdit(new_row);
        new_name->setObjectName(QStringLiteral("pelican.materials.new.name"));
        new_name->setToolTip(owner.tr("Letters, digits and _; the file is named after it"));
        new_path = new QLabel(new_row);
        new_path->setObjectName(QStringLiteral("pelican.materials.new.path"));
        applyPanelNoticeStyle(new_path);
        new_notice = new QLabel(new_row);
        new_notice->setObjectName(QStringLiteral("pelican.materials.new.notice"));
        applyPanelNoticeStyle(new_notice);
        new_notice->hide();
        auto *buttons = new QHBoxLayout;
        new_create = new QPushButton(owner.tr("Create"), new_row);
        new_create->setObjectName(QStringLiteral("pelican.materials.new.create"));
        new_cancel = new QPushButton(owner.tr("Cancel"), new_row);
        new_cancel->setObjectName(QStringLiteral("pelican.materials.new.cancel"));
        buttons->addStretch();
        buttons->addWidget(new_create);
        buttons->addWidget(new_cancel);
        form->addRow(owner.tr("Surface"), new_surface);
        form->addRow(owner.tr("Name"), new_name);
        form->addRow(QString{}, new_path);
        form->addRow(new_notice);
        form->addRow(buttons);
        new_row->hide();
        QObject::connect(new_name, &QLineEdit::textChanged, &owner, [this] { presentNewRow(); });
        QObject::connect(new_surface, &QComboBox::currentIndexChanged, &owner, [this] { presentNewRow(); });
        QObject::connect(new_cancel, &QPushButton::clicked, &owner, [this] { new_row->hide(); });
        QObject::connect(new_create, &QPushButton::clicked, &owner, [this] {
            const int index = new_surface->currentData().isValid() ? new_surface->currentData().toInt() : -1;
            const auto &surfaces = session.surfaces();
            if (index < 0 || index >= static_cast<int>(surfaces.size())) return;
            if (session.create(surfaces[static_cast<std::size_t>(index)], new_name->text().trimmed().toStdString()))
                new_row->hide();
        });
        layout.addWidget(new_row);
    }

    void openNewRow() {
        session.readSurfaces();
        {
            const QSignalBlocker block{new_name};
            new_name->setText(qtext(suggestMaterialName(session.list(), session.projectRoot())));
        }
        fillSurfaceChoices(*new_surface, {}, true);
        new_row->show();
        presentNewRow();
        new_name->setFocus();
    }

    // The engine's surfaces, as the combo shows them; one that cannot be read
    // is listed greyed, with the reason, and cannot be chosen.
    void fillSurfaceChoices(QComboBox &choices, const std::string &current, bool pick_default) {
        const QSignalBlocker block{&choices};
        const auto previous = choices.currentData();
        choices.clear();
        const auto &surfaces = session.surfaces();
        for (std::size_t index = 0; index < surfaces.size(); ++index) {
            const auto &surface = surfaces[index];
            choices.addItem(qtext(surfaceChoiceLabel(surface)), static_cast<int>(index));
            const int row = choices.count() - 1;
            QString tip = qtext(surface.reference);
            if (!surface.error.empty()) {
                tip += QLatin1Char('\n') + qtext(surface.error);
                if (auto *model = qobject_cast<QStandardItemModel *>(choices.model()))
                    if (auto *item = model->item(row)) item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            }
            choices.setItemData(row, tip, Qt::ToolTipRole);
        }
        int wanted = -1;
        if (!current.empty()) {
            for (std::size_t index = 0; index < surfaces.size(); ++index)
                if (surfaces[index].reference == current) wanted = static_cast<int>(index);
            if (wanted < 0) {
                // A surface the engine does not list stays shown as it is.
                choices.addItem(qtext(current), -1);
                wanted = choices.count() - 1;
            }
        } else if (pick_default && previous.isValid() && previous.toInt() >= 0 &&
                   previous.toInt() < static_cast<int>(surfaces.size()) &&
                   surfaces[static_cast<std::size_t>(previous.toInt())].error.empty()) {
            wanted = previous.toInt();
        } else if (pick_default) {
            if (const auto chosen = defaultSurfaceChoice(surfaces)) wanted = static_cast<int>(*chosen);
        }
        choices.setCurrentIndex(wanted);
    }

    void presentNewRow() {
        if (new_row->isHidden()) return;
        if (new_surface->count() != static_cast<int>(session.surfaces().size()))
            fillSurfaceChoices(*new_surface, {}, true);
        const auto name = new_name->text().trimmed().toStdString();
        new_path->setText(name.empty() ? QString{} : qtext(materialDocumentPath(name)));
        QString line;
        using State = MaterialsDockSession::SurfacesState;
        if (session.surfacesState() == State::Unavailable)
            line = owner.tr("This engine does not list surfaces (list_surfaces), so no material can be "
                            "made here yet.");
        else if (session.surfacesState() == State::Reading)
            line = owner.tr("Reading the engine's surfaces\xE2\x80\xA6");
        else if (!validMaterialName(name))
            line = owner.tr("A material name is letters, digits and _.");
        else if (const auto names = materialNames(session.list());
                 std::ranges::find(names, name) != names.end())
            line = owner.tr("A material named %1 exists already.").arg(qtext(name));
        setPanelNotice(new_notice, line);
        const bool surface_chosen = new_surface->currentIndex() >= 0 && new_surface->currentData().toInt() >= 0;
        new_create->setEnabled(line.isEmpty() && surface_chosen && session.ready());
    }

    // --- the list (decision 1)

    void fillTree() {
        const auto &list = session.list();
        if (list_shown && list == shown_list) return;
        shown_list = list;
        list_shown = true;
        const auto selection = session.selection();
        const QBrush dim = owner.palette().brush(QPalette::Disabled, QPalette::Text);
        const QSignalBlocker block{tree};
        tree->clear();
        QTreeWidgetItem *current = nullptr;
        for (const auto &document : list.documents) {
            auto *row = new QTreeWidgetItem(tree);
            const auto slash = document.path.rfind('/');
            row->setText(0, qtext(slash == std::string::npos ? document.path : document.path.substr(slash + 1)));
            row->setToolTip(0, qtext(document.path));
            row->setData(0, MaterialPathRole, qtext(document.path));
            if (document.status != "loaded") {
                row->setText(1, qtext(document.status + (document.reason.empty() ? "" : ": " + document.reason)));
                row->setToolTip(1, qtext(document.reason));
                row->setForeground(0, dim);
                row->setForeground(1, dim);
                row->setFlags(Qt::ItemIsEnabled);
                continue;
            }
            for (const auto &material : document.materials) {
                auto *child = new QTreeWidgetItem(row);
                child->setText(0, qtext(material.name));
                child->setText(1, qtext(material.surface));
                child->setToolTip(0, material.registered
                                         ? qtext(material.id)
                                         : owner.tr("Not registered in this engine yet: it is read when the "
                                                    "engine starts"));
                child->setData(0, MaterialPathRole, qtext(document.path));
                child->setData(0, MaterialNameRole, qtext(material.name));
                if (!material.registered) child->setForeground(0, dim);
                if (selection && selection->path == document.path && selection->name == material.name)
                    current = child;
            }
            row->setExpanded(true);
        }
        if (current != nullptr) tree->setCurrentItem(current);
    }

    void picked(QTreeWidgetItem *item) {
        if (item == nullptr) return;
        const auto name = item->data(0, MaterialNameRole).toString().toStdString();
        const auto path = item->data(0, MaterialPathRole).toString().toStdString();
        if (name.empty()) {
            // A document's row: its first material.
            if (item->childCount() > 0) tree->setCurrentItem(item->child(0));
            return;
        }
        images.clear();
        session.select({path, name});
    }

    void selectInTree() {
        const auto selection = session.selection();
        if (!selection) return;
        for (QTreeWidgetItemIterator it{tree}; *it; ++it)
            if ((*it)->data(0, MaterialPathRole).toString().toStdString() == selection->path &&
                (*it)->data(0, MaterialNameRole).toString().toStdString() == selection->name) {
                if (tree->currentItem() != *it) {
                    const QSignalBlocker block{tree};
                    tree->setCurrentItem(*it);
                }
                return;
            }
    }

    // --- the editor (decisions 2 and 3)

    std::string structureOf() const {
        const auto selection = session.selection();
        const auto &schema = session.schema();
        if (!selection || !session.material().is_object() || !schema) return {};
        std::string structure = selection->path + "\n" + selection->name + "\n" + schema->surface;
        for (const auto &param : schema->params) structure += "\n" + param.name + ":" + param.type;
        for (const auto &texture : schema->textures) structure += "\n" + texture.name;
        return structure;
    }

    void buildEditor() {
        built_structure = structureOf();
        param_rows.clear();
        texture_rows.clear();
        heading = nullptr;
        surface = nullptr;
        tags = nullptr;
        alpha_mode = nullptr;
        double_sided = nullptr;
        variants = nullptr;
        delete editor_scroll->takeWidget();
        editor = new QWidget;
        editor->setObjectName(QStringLiteral("pelican.materials.fields"));
        auto *layout = new QVBoxLayout(editor);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
        editor_scroll->setWidget(editor);
        const auto selection = session.selection();
        if (built_structure.empty()) {
            auto *empty = new QLabel(editor);
            empty->setObjectName(QStringLiteral("pelican.materials.empty"));
            applyPanelNoticeStyle(empty);
            empty->setText(!selection ? owner.tr("Pick a material to edit it.")
                                      : owner.tr("Reading %1\xE2\x80\xA6").arg(qtext(selection->name)));
            layout->addWidget(empty);
            layout->addStretch();
            return;
        }
        heading = new QLabel(editor);
        heading->setObjectName(QStringLiteral("pelican.materials.heading"));
        QFont bold = heading->font();
        bold.setBold(true);
        heading->setFont(bold);
        heading->setText(owner.tr("%1 \xE2\x80\x94 %2").arg(qtext(selection->name), qtext(selection->path)));
        layout->addWidget(heading);

        auto *settings = new QFormLayout;
        surface = new QComboBox(editor);
        surface->setObjectName(QStringLiteral("pelican.materials.surface"));
        surface->setToolTip(owner.tr("The surface the material draws with; a change is read when the "
                                     "engine starts"));
        QObject::connect(surface, &QComboBox::currentIndexChanged, &owner, [this](int row) {
            if (filling || row < 0) return;
            const int index = surface->itemData(row).toInt();
            const auto &surfaces = session.surfaces();
            if (index < 0 || index >= static_cast<int>(surfaces.size())) return;
            const auto &reference = surfaces[static_cast<std::size_t>(index)].reference;
            if (reference != session.material().value("surface", std::string{})) session.setSurface(reference);
        });
        settings->addRow(owner.tr("Surface"), surface);
        tags = new GraphCanvasTagPicker(QStringLiteral("pelican.materials.tags"), owner.tr("(no tags)"), editor);
        tags->allowNewTags(true);
        tags->setToolTip(owner.tr("The tags a material pass's filter and a draw loop test; a change is read "
                                  "when the engine starts"));
        tags->changed = [this] {
            if (!filling) session.setTags(tags->selected());
        };
        settings->addRow(owner.tr("Tags"), tags);
        alpha_mode = new QComboBox(editor);
        alpha_mode->setObjectName(QStringLiteral("pelican.materials.alphaMode"));
        for (const char *mode : {"opaque", "mask", "blend"}) alpha_mode->addItem(QLatin1String(mode), QLatin1String(mode));
        alpha_mode->setToolTip(owner.tr("routing.alpha_mode: which material passes draw it; read when the "
                                        "engine starts"));
        double_sided = new QCheckBox(owner.tr("Double sided"), editor);
        double_sided->setObjectName(QStringLiteral("pelican.materials.doubleSided"));
        double_sided->setToolTip(owner.tr("routing.double_sided; read when the engine starts"));
        const auto routing_changed = [this] {
            if (filling) return;
            session.setRouting(alpha_mode->currentData().toString().toStdString(), double_sided->isChecked());
        };
        QObject::connect(alpha_mode, &QComboBox::currentIndexChanged, &owner, routing_changed);
        QObject::connect(double_sided, &QCheckBox::toggled, &owner, routing_changed);
        auto *routing = new QHBoxLayout;
        routing->addWidget(alpha_mode, 1);
        routing->addWidget(double_sided);
        settings->addRow(owner.tr("Alpha mode"), routing);
        variants = new QLabel(editor);
        variants->setObjectName(QStringLiteral("pelican.materials.variants"));
        applyPanelNoticeStyle(variants);
        settings->addRow(owner.tr("Variants"), variants);
        layout->addLayout(settings);

        const auto &schema = *session.schema();
        auto *params_box = new QFrame(editor);
        params_form = new QFormLayout(params_box);
        params_form->setContentsMargins(0, 0, 0, 0);
        for (const auto &param : schema.params) buildParamRow(*params_box, param);
        layout->addWidget(new QLabel(owner.tr("Parameters"), editor));
        layout->addWidget(params_box);
        if (!schema.textures.empty()) {
            if (images.empty()) {
                const auto roots = assetRoots(session.projectRoot(), Json{}, Json{});
                images = scanAssetRoots(roots);
            }
            auto *textures_box = new QFrame(editor);
            textures_form = new QFormLayout(textures_box);
            textures_form->setContentsMargins(0, 0, 0, 0);
            for (const auto &texture : schema.textures) buildTextureRow(*textures_box, texture);
            layout->addWidget(new QLabel(owner.tr("Textures"), editor));
            layout->addWidget(textures_box);
        }
        layout->addStretch();
    }

    void addLabel(ParamRow &row, QWidget &parent) {
        row.label = new QLabel(qtext(row.param.name), &parent);
        row.label->setObjectName(fieldName(row.param.name, "label"));
        QString tip = qtext(row.param.type);
        if (!row.param.hint.empty()) tip += QStringLiteral(" \xC2\xB7 ") + qtext(row.param.hint);
        if (row.param.min || row.param.max)
            tip += QStringLiteral(" \xC2\xB7 ") + owner.tr("%1 to %2")
                                                    .arg(row.param.min ? QString::number(*row.param.min) : QStringLiteral("-"),
                                                         row.param.max ? QString::number(*row.param.max) : QStringLiteral("-"));
        tip += QLatin1Char('\n') + owner.tr("Default: %1").arg(qtext(row.param.default_value.dump()));
        row.label->setToolTip(tip);
        row.reset = new QAction(owner.tr("Reset to default"), row.label);
        row.reset->setObjectName(fieldName(row.param.name, "reset"));
        row.label->addAction(row.reset);
        row.label->setContextMenuPolicy(Qt::ActionsContextMenu);
        QObject::connect(row.reset, &QAction::triggered, &owner,
                         [this, name = row.param.name] { session.setValue(name, nullptr); });
    }

    void buildParamRow(QWidget &parent, const SurfaceParam &param) {
        ParamRow row;
        row.param = param;
        row.kind = materialFieldKind(param);
        addLabel(row, parent);
        auto *cell = new QWidget(&parent);
        cell->setObjectName(fieldName(param.name));
        auto *cell_layout = new QVBoxLayout(cell);
        cell_layout->setContentsMargins(0, 0, 0, 0);
        cell_layout->setSpacing(1);
        auto *line = new QHBoxLayout;
        line->setSpacing(4);
        const std::string name = param.name;
        switch (row.kind) {
        case MaterialFieldKind::Slider: {
            row.slider = new QSlider(Qt::Horizontal, cell);
            row.slider->setObjectName(fieldName(name, "slider"));
            row.slider->setRange(0, SliderSteps);
            row.spin = new QDoubleSpinBox(cell);
            row.spin->setObjectName(fieldName(name, "spin"));
            row.spin->setRange(*param.min, *param.max);
            row.spin->setDecimals(3);
            row.spin->setSingleStep((*param.max - *param.min) / 100.0);
            row.spin->setKeyboardTracking(false);
            line->addWidget(row.slider, 1);
            line->addWidget(row.spin);
            QSlider *slider = row.slider;
            QDoubleSpinBox *spin = row.spin;
            const double low = *param.min;
            const double high = *param.max;
            // Dragged: the number follows, nothing is sent until let go.
            QObject::connect(slider, &QSlider::valueChanged, &owner, [=, this](int step) {
                if (filling) return;
                const double value = low + (high - low) * step / SliderSteps;
                {
                    const QSignalBlocker block{spin};
                    spin->setValue(value);
                }
                if (!slider->isSliderDown()) commitNumber(name, spin->value());
            });
            QObject::connect(slider, &QSlider::sliderReleased, &owner,
                             [=, this] { commitNumber(name, spin->value()); });
            QObject::connect(spin, &QDoubleSpinBox::valueChanged, &owner, [=, this](double value) {
                if (filling) return;
                {
                    const QSignalBlocker block{slider};
                    slider->setValue(static_cast<int>(std::lround((value - low) / (high - low) * SliderSteps)));
                }
                commitNumber(name, value);
            });
            break;
        }
        case MaterialFieldKind::Number: {
            row.spin = new QDoubleSpinBox(cell);
            row.spin->setObjectName(fieldName(name, "spin"));
            row.spin->setRange(param.min.value_or(-1.0e9), param.max.value_or(1.0e9));
            row.spin->setDecimals(4);
            row.spin->setSingleStep(0.05);
            row.spin->setKeyboardTracking(false);
            line->addWidget(row.spin, 1);
            QObject::connect(row.spin, &QDoubleSpinBox::valueChanged, &owner, [=, this](double value) {
                if (!filling) commitNumber(name, value);
            });
            break;
        }
        case MaterialFieldKind::Integer: {
            row.integer = new QSpinBox(cell);
            row.integer->setObjectName(fieldName(name, "spin"));
            row.integer->setRange(param.min ? static_cast<int>(*param.min) : -1000000,
                                  param.max ? static_cast<int>(*param.max) : 1000000);
            row.integer->setKeyboardTracking(false);
            line->addWidget(row.integer, 1);
            QObject::connect(row.integer, &QSpinBox::valueChanged, &owner, [=, this](int value) {
                if (!filling) commitJson(name, Json(value));
            });
            break;
        }
        case MaterialFieldKind::Toggle: {
            row.check = new QCheckBox(cell);
            row.check->setObjectName(fieldName(name, "check"));
            line->addWidget(row.check, 1);
            QObject::connect(row.check, &QCheckBox::toggled, &owner, [=, this](bool on) {
                if (!filling) commitJson(name, Json(on ? 1 : 0));
            });
            break;
        }
        case MaterialFieldKind::Color: {
            row.swatch = new QPushButton(cell);
            row.swatch->setObjectName(fieldName(name, "swatch"));
            row.swatch->setToolTip(param.encoding == "srgb"
                                       ? owner.tr("Pick a colour (stored as shown, sRGB)")
                                       : owner.tr("Pick a colour (shown in sRGB, stored linear)"));
            row.swatch->setMinimumWidth(64);
            line->addWidget(row.swatch, 1);
            QObject::connect(row.swatch, &QPushButton::clicked, &owner, [this, name] { pickColor(name); });
            break;
        }
        case MaterialFieldKind::Vector: {
            const std::size_t count = param.type == "vec2" ? 2 : (param.type == "vec3" ? 3 : 4);
            for (std::size_t index = 0; index < count; ++index) {
                auto *component = new QDoubleSpinBox(cell);
                component->setObjectName(fieldName(name) + QStringLiteral(".%1").arg(index));
                component->setRange(param.min.value_or(-1.0e9), param.max.value_or(1.0e9));
                component->setDecimals(4);
                component->setSingleStep(0.05);
                component->setKeyboardTracking(false);
                row.components.push_back(component);
                line->addWidget(component, 1);
                QObject::connect(component, &QDoubleSpinBox::valueChanged, &owner, [this, name] {
                    if (filling) return;
                    const auto found = param_rows.find(name);
                    if (found == param_rows.end()) return;
                    Json value = Json::array();
                    for (auto *editor : found->second.components) value.push_back(editor->value());
                    commitJson(name, value);
                });
            }
            break;
        }
        case MaterialFieldKind::Other: {
            row.other = new QLabel(cell);
            row.other->setObjectName(fieldName(name, "value"));
            row.other->setToolTip(owner.tr("A %1 is not edited here").arg(qtext(param.type)));
            line->addWidget(row.other, 1);
            break;
        }
        }
        cell_layout->addLayout(line);
        row.error = new QLabel(cell);
        row.error->setObjectName(fieldName(name, "error"));
        applyPanelNoticeStyle(row.error);
        row.error->setStyleSheet(QStringLiteral("color: #d94c3d;"));
        row.error->hide();
        cell_layout->addWidget(row.error);
        params_form->addRow(row.label, cell);
        param_rows.emplace(name, std::move(row));
    }

    void buildTextureRow(QWidget &parent, const SurfaceTexture &texture) {
        TextureRow row;
        row.texture = texture;
        row.label = new QLabel(qtext(texture.name), &parent);
        row.label->setObjectName(QStringLiteral("pelican.materials.texture.") + qtext(texture.name) +
                                 QStringLiteral(".label"));
        const QString tip = owner.tr("color space %1 \xC2\xB7 %2 \xC2\xB7 %3")
                                .arg(qtext(texture.color_space), qtext(texture.role), qtext(texture.dimension));
        row.label->setToolTip(tip);
        row.choice = new QComboBox(&parent);
        row.choice->setObjectName(QStringLiteral("pelican.materials.texture.") + qtext(texture.name));
        row.choice->setToolTip(tip);
        const std::string name = texture.name;
        QObject::connect(row.choice, &QComboBox::currentIndexChanged, &owner, [this, name](int index) {
            if (filling || index < 0) return;
            const auto found = texture_rows.find(name);
            if (found == texture_rows.end()) return;
            const auto reference = found->second.choice->itemData(index).toString();
            const auto current = materialTextureValue(session.material(), name);
            if (reference.toStdString() == current) return;
            session.setTexture(name, reference.isEmpty() ? Json(nullptr) : Json(reference.toStdString()));
        });
        textures_form->addRow(row.label, row.choice);
        texture_rows.emplace(name, std::move(row));
    }

    void commitJson(const std::string &name, const Json &value) {
        const auto found = param_rows.find(name);
        if (found == param_rows.end()) return;
        if (materialParamValue(session.material(), found->second.param) == value) return;
        session.setValue(name, value);
    }

    void commitNumber(const std::string &name, double value) {
        const auto found = param_rows.find(name);
        if (found == param_rows.end()) return;
        const auto current = materialParamValue(session.material(), found->second.param);
        if (current.is_number() && sameNumber(current.get<double>(), value)) return;
        // What the spin shows has its decimals: send that, not a float's tail.
        session.setValue(name, std::round(value * 1.0e6) / 1.0e6);
    }

    void pickColor(const std::string &name) {
        const auto found = param_rows.find(name);
        if (found == param_rows.end()) return;
        if (color_dialog) color_dialog->close();
        const auto shown = materialColorForDisplay(materialParamValue(session.material(), found->second.param),
                                                   found->second.param);
        auto *dialog = new QColorDialog(&owner);
        dialog->setObjectName(QStringLiteral("pelican.materials.colorDialog"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setOption(QColorDialog::DontUseNativeDialog);
        dialog->setWindowTitle(qtext(name));
        dialog->setCurrentColor(QColor::fromRgbF(static_cast<float>(shown[0]), static_cast<float>(shown[1]),
                                                 static_cast<float>(shown[2])));
        // A dialog is filled in as a panel is: its hex field and its OK are
        // named, so a scenario types the one and presses the other.
        for (auto *field : dialog->findChildren<QLineEdit *>()) {
            bool in_spin = false;
            for (QObject *up = field->parent(); up != nullptr && up != dialog; up = up->parent())
                if (qobject_cast<QAbstractSpinBox *>(up) != nullptr) in_spin = true;
            if (!in_spin) {
                field->setObjectName(QStringLiteral("pelican.materials.colorDialog.hex"));
                break;
            }
        }
        if (auto *box = dialog->findChild<QDialogButtonBox *>()) {
            if (auto *ok = box->button(QDialogButtonBox::Ok))
                ok->setObjectName(QStringLiteral("pelican.materials.colorDialog.ok"));
            if (auto *cancel = box->button(QDialogButtonBox::Cancel))
                cancel->setObjectName(QStringLiteral("pelican.materials.colorDialog.cancel"));
        }
        color_param = name;
        QObject::connect(dialog, &QColorDialog::colorSelected, &owner, [this, alpha = shown[3]](const QColor &color) {
            const auto found_row = param_rows.find(color_param);
            if (found_row == param_rows.end() || !color.isValid()) return;
            const auto value = materialColorFromDisplay({color.redF(), color.greenF(), color.blueF(), alpha},
                                                        found_row->second.param);
            commitJson(color_param, value);
        });
        color_dialog = dialog;
        dialog->open();
    }

    void fillTextureChoices(TextureRow &row) {
        const auto current = materialTextureValue(session.material(), row.texture.name);
        const QSignalBlocker block{row.choice};
        row.choice->clear();
        row.choice->addItem(owner.tr("None (%1)").arg(qtext(row.texture.default_reference)), NoTexture);
        auto choices = textureChoices(images, row.texture.dimension);
        if (!current.empty() && std::ranges::find(choices, current) == choices.end()) choices.push_back(current);
        for (const auto &choice : choices) {
            QString shown = qtext(choice);
            if (shown.startsWith(QStringLiteral("project://assets/"))) shown.remove(0, 17);
            row.choice->addItem(shown, qtext(choice));
            row.choice->setItemData(row.choice->count() - 1, qtext(choice), Qt::ToolTipRole);
        }
        row.choice->setCurrentIndex(std::max(0, row.choice->findData(qtext(current))));
    }

    void applyValues() {
        const auto &material = session.material();
        filling = true;
        if (surface != nullptr) fillSurfaceChoices(*surface, material.value("surface", std::string{}), false);
        if (tags != nullptr) {
            std::vector<std::string> chosen;
            if (const auto found = material.find("tags"); found != material.end() && found->is_array())
                for (const auto &tag : *found)
                    if (tag.is_string()) chosen.push_back(tag.get<std::string>());
            tags->setTags(materialTagVocabulary(session.list()), chosen);
        }
        if (alpha_mode != nullptr) {
            const auto routing = material.value("routing", Json::object());
            const QSignalBlocker block_mode{alpha_mode};
            const QSignalBlocker block_sides{double_sided};
            alpha_mode->setCurrentIndex(
                std::max(0, alpha_mode->findData(qtext(routing.is_object() ? routing.value("alpha_mode", std::string{"opaque"})
                                                                             : std::string{"opaque"}))));
            double_sided->setChecked(routing.is_object() && routing.value("double_sided", false));
        }
        if (variants != nullptr) {
            QStringList names;
            if (const auto found = material.find("variants"); found != material.end() && found->is_object())
                for (const auto &[key, value] : found->items()) names << qtext(key);
            variants->setText(names.isEmpty() ? owner.tr("none") : names.join(QStringLiteral(", ")));
            variants->setToolTip(owner.tr("Variants are edited in the document itself for now"));
        }
        for (auto &[name, row] : param_rows) {
            const auto value = materialParamValue(material, row.param);
            QFont font = row.label->font();
            font.setBold(materialParamOverridden(material, row.param));
            row.label->setFont(font);
            row.reset->setEnabled(materialParamOverridden(material, row.param));
            const auto number_of = [](const Json &json) { return json.is_number() ? json.get<double>() : 0.0; };
            if (row.spin != nullptr && !row.spin->hasFocus()) {
                const QSignalBlocker block{row.spin};
                row.spin->setValue(number_of(value));
            }
            if (row.slider != nullptr && !row.slider->isSliderDown()) {
                const QSignalBlocker block{row.slider};
                const double low = *row.param.min;
                const double high = *row.param.max;
                row.slider->setValue(
                    static_cast<int>(std::lround((number_of(value) - low) / (high - low) * SliderSteps)));
            }
            if (row.integer != nullptr && !row.integer->hasFocus()) {
                const QSignalBlocker block{row.integer};
                row.integer->setValue(static_cast<int>(number_of(value)));
            }
            if (row.check != nullptr) {
                const QSignalBlocker block{row.check};
                row.check->setChecked(number_of(value) != 0.0);
            }
            if (row.swatch != nullptr) {
                const auto shown = materialColorForDisplay(value, row.param);
                const QString hex = hexOf(shown);
                row.swatch->setText(hex);
                row.swatch->setStyleSheet(
                    QStringLiteral("QPushButton { background-color: %1; color: %2; }")
                        .arg(hex, (shown[0] * 0.3 + shown[1] * 0.59 + shown[2] * 0.11) > 0.5 ? QStringLiteral("#000000")
                                                                                             : QStringLiteral("#ffffff")));
                row.swatch->setProperty("pelicanColorValue", qtext(value.dump()));
            }
            for (std::size_t index = 0; index < row.components.size(); ++index) {
                auto *component = row.components[index];
                if (component->hasFocus()) continue;
                const QSignalBlocker block{component};
                component->setValue(value.is_array() && index < value.size() ? number_of(value[index]) : 0.0);
            }
            if (row.other != nullptr) row.other->setText(qtext(value.dump()));
            const bool error = session.fieldErrorParam() == name && !session.fieldError().empty();
            setPanelNotice(row.error, error ? oneLine(qtext(session.fieldError()), 120) : QString{},
                           error ? qtext(session.fieldError()) : QString{});
        }
        for (auto &[name, row] : texture_rows) {
            fillTextureChoices(row);
            QFont font = row.label->font();
            font.setBold(!materialTextureValue(material, name).empty());
            row.label->setFont(font);
        }
        filling = false;
    }

    void assignSelected() {
        const auto object = hooks.selected_object ? hooks.selected_object() : std::nullopt;
        if (!object) {
            session.say(owner.tr("Pick an object that shows a model in the Outliner first.").toStdString());
            return;
        }
        session.assign(object->model, object->name);
    }

    void presentAssign() {
        const auto object = hooks.selected_object ? hooks.selected_object() : std::nullopt;
        const auto selection = session.selection();
        assign_button->setEnabled(session.ready() && selection.has_value());
        if (object && selection)
            assign_button->setToolTip(owner.tr("Bind %1 to every primitive of %2 (%3): every object showing "
                                               "that model takes it")
                                          .arg(qtext(selection->name), qtext(object->name), qtext(object->model)));
        else
            assign_button->setToolTip(owner.tr("Pick a material, and an object with a model in the Outliner"));
    }

    void present() {
        const auto &undo = session.undoEntries();
        undo_action->setEnabled(!undo.empty() && session.ready() && !session.editPending());
        undo_action->setText(undo.empty() ? owner.tr("Undo") : owner.tr("Undo %1").arg(qtext(undo.back().description)));
        undo_action->setToolTip(undo_action->text());
        new_button->setEnabled(session.ready());
        QString line = qtext(session.notice());
        QString detail = qtext(session.noticeDetail());
        if (line.isEmpty() && !session.restartReason().empty()) {
            line = owner.tr("Restart the engine to apply: %1").arg(qtext(session.restartReason()));
            detail = qtext(session.restartReason());
        }
        if (line.isEmpty() && !session.ready() && !session.projectRoot().empty())
            line = owner.tr("The engine is not running; materials are read when it is.");
        setPanelNotice(notice, oneLine(line, 140), detail.isEmpty() ? line : detail);
        restart->setVisible(!session.restartReason().empty() && static_cast<bool>(hooks.restart));
        fillTree();
        selectInTree();
        if (structureOf() != built_structure || editor_scroll->widget() == nullptr) buildEditor();
        if (!built_structure.empty()) applyValues();
        presentNewRow();
        presentAssign();
        updatePolling();
    }

    void updatePolling() {
        const bool wanted = session.ready() && owner.isVisible();
        if (wanted && !poll_timer->isActive()) poll_timer->start();
        if (!wanted && poll_timer->isActive()) poll_timer->stop();
    }
};

MaterialsDock::MaterialsDock(Hooks hooks, QWidget *parent) : QWidget(parent) {
    impl_ = std::make_unique<Impl>(*this, std::move(hooks));
}

MaterialsDock::~MaterialsDock() {
    impl_->session.changed = nullptr;
    impl_->session.listChanged = nullptr;
    impl_->session.restartRequested = nullptr;
    impl_->session.bound = nullptr;
    for (QObject *child : findChildren<QObject *>()) QObject::disconnect(child, nullptr, this, nullptr);
}

void MaterialsDock::open(const std::filesystem::path &project_root) {
    impl_->images.clear();
    impl_->list_shown = false;
    impl_->new_row->hide();
    impl_->session.open(project_root);
}

void MaterialsDock::engineAvailable() { impl_->session.engineReady(); }

void MaterialsDock::engineUnavailable() { impl_->session.engineGone(); }

void MaterialsDock::selectedObjectChanged() { impl_->presentAssign(); }

bool MaterialsDock::receiveResult(qint64 request_id, const QByteArray &result_json) {
    if (!impl_->session.owns(request_id)) return false;
    const auto result =
        Json::parse(result_json.constData(), result_json.constData() + result_json.size(), nullptr, false);
    return impl_->session.receiveResult(request_id, result.is_discarded() ? Json{} : result);
}

bool MaterialsDock::receiveRefusal(qint64 request_id, int code, const QString &message, const QJsonValue &data) {
    if (!impl_->session.owns(request_id)) return false;
    const QJsonObject object = data.toObject();
    const auto fields = Json::parse(QJsonDocument(object.value(QStringLiteral("error_fields")).toObject())
                                        .toJson(QJsonDocument::Compact)
                                        .toStdString(),
                                    nullptr, false);
    return impl_->session.receiveRefusal(request_id, code,
                                         object.value(QStringLiteral("error_code")).toString().toStdString(),
                                         message.toStdString(), fields.is_discarded() ? Json::object() : fields);
}

bool MaterialsDock::receiveFailure(qint64 request_id, const QString &message) {
    return impl_->session.receiveFailure(request_id, message.toStdString());
}

void MaterialsDock::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    impl_->updatePolling();
    // Shown again: what changed while it was hidden is read now.
    impl_->session.poll();
}

void MaterialsDock::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    impl_->updatePolling();
}

MaterialsDockSession &MaterialsDock::session() noexcept { return impl_->session; }
QTreeWidget *MaterialsDock::list() const noexcept { return impl_->tree; }
QAction *MaterialsDock::undoAction() const noexcept { return impl_->undo_action; }
QLabel *MaterialsDock::noticeLine() const noexcept { return impl_->notice; }

} // namespace PelicanStudio
