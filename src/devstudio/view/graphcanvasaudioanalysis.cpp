// AB-1s: Bake analysis..., what a committed bake offers (Use in Graph, Use in
// a shader), and the stale mark with its Re-bake (engine AB-1).

#include "graphcanvasaudioanalysis.hpp"

#include "../model/graphcanvasmodel.hpp"
#include "../model/inspectormodel.hpp"
#include "studiotheme.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

QString qtext(const std::string &value) { return QString::fromStdString(value); }

constexpr int PollMilliseconds = 250;

// The sound's file name, as the running line names it.
std::string soundName(const std::string &sound) {
    const auto relative = projectRelativeReference(sound);
    const auto slash = relative.find_last_of('/');
    return slash == std::string::npos ? relative : relative.substr(slash + 1);
}

} // namespace

// --- Decision 1: the dialog.

GraphCanvasBakeDialog::GraphCanvasBakeDialog(const QString &sound, const QString &out, QWidget *parent)
    : QDialog{parent} {
    setObjectName(QStringLiteral("pelican.audioBakeDialog"));
    setWindowTitle(tr("Bake analysis of %1").arg(sound));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("The sound's loudness and bands, baked into a table the Graph and "
                                "shaders read at any time. Bake again after the sound changes."),
                             this);
    intro->setWordWrap(true);
    intro->setFont(studioSmallFont());
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->addLayout(form);
    const auto name = [](const char *field) {
        return QStringLiteral("pelican.audioBakeDialog.") + QString::fromLatin1(field);
    };
    auto *features = new QHBoxLayout;
    rms_ = new QCheckBox(tr("RMS"), this);
    rms_->setObjectName(name("rms"));
    rms_->setChecked(true);
    rms_->setToolTip(tr("The loudness: the root mean square of each window."));
    peak_ = new QCheckBox(tr("Peak"), this);
    peak_->setObjectName(name("peak"));
    peak_->setChecked(true);
    peak_->setToolTip(tr("The largest sample of each window."));
    bands_ = new QCheckBox(tr("Bands"), this);
    bands_->setObjectName(name("bands"));
    bands_->setChecked(true);
    bands_->setToolTip(tr("The level of each frequency band, from 20 Hz up, spaced evenly in pitch."));
    features->addWidget(rms_);
    features->addWidget(peak_);
    features->addWidget(bands_);
    features->addStretch(1);
    form->addRow(tr("Features"), features);
    band_count_ = new QSpinBox(this);
    band_count_->setObjectName(name("bandCount"));
    band_count_->setRange(1, 128);
    band_count_->setValue(8);
    band_count_->setToolTip(tr("How many bands (1 to 128): rows band:0 .. band:N-1, low to high."));
    form->addRow(tr("Bands"), band_count_);
    const auto milliseconds = [this](const char *field, double low, double high, double value,
                                     const QString &tip) {
        auto *spin = new QDoubleSpinBox(this);
        spin->setObjectName(QStringLiteral("pelican.audioBakeDialog.") + QString::fromLatin1(field));
        spin->setRange(low, high);
        spin->setDecimals(1);
        spin->setValue(value);
        spin->setSuffix(QStringLiteral(" ms"));
        spin->setToolTip(tip);
        return spin;
    };
    hop_ = milliseconds("hop", 1.0, 1000.0, 10.0,
                        tr("The time between two samples of the table; the window is twice as long."));
    form->addRow(tr("Hop"), hop_);
    attack_ = milliseconds("attack", 0.0, 60000.0, 0.0,
                           tr("How fast the values may rise (0: at once)."));
    release_ = milliseconds("release", 0.0, 60000.0, 0.0,
                            tr("How fast the values may fall (0: at once)."));
    auto *smoothing = new QHBoxLayout;
    smoothing->addWidget(new QLabel(tr("attack"), this));
    smoothing->addWidget(attack_, 1);
    smoothing->addWidget(new QLabel(tr("release"), this));
    smoothing->addWidget(release_, 1);
    form->addRow(tr("Smoothing"), smoothing);
    channels_ = new QComboBox(this);
    channels_->setObjectName(name("channels"));
    channels_->addItem(tr("Mix"), QStringLiteral("mix"));
    channels_->addItem(tr("Left"), QStringLiteral("left"));
    channels_->addItem(tr("Right"), QStringLiteral("right"));
    channels_->setToolTip(tr("Which channels are read: all of them mixed, or one."));
    form->addRow(tr("Channels"), channels_);
    out_ = new QLineEdit(out, this);
    out_->setObjectName(name("out"));
    out_->setToolTip(tr("Where the table is written, in the project (a .ktx2; its manifest goes beside "
                        "it). A table already there is replaced."));
    form->addRow(tr("Table"), out_);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bake_ = buttons->button(QDialogButtonBox::Ok);
    bake_->setText(tr("Bake"));
    bake_->setObjectName(name("bake"));
    buttons->button(QDialogButtonBox::Cancel)->setObjectName(name("cancel"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    for (QCheckBox *check : {rms_, peak_, bands_}) connect(check, &QCheckBox::toggled, this, [this] { follow(); });
    connect(out_, &QLineEdit::textChanged, this, [this] { follow(); });
    follow();
}

void GraphCanvasBakeDialog::follow() {
    band_count_->setEnabled(bands_->isChecked());
    const bool any = rms_->isChecked() || peak_->isChecked() || bands_->isChecked();
    const auto out = out_->text().trimmed();
    const bool table = out.endsWith(QStringLiteral(".ktx2"), Qt::CaseInsensitive);
    bake_->setEnabled(any && table);
    bake_->setToolTip(!any     ? tr("Pick at least one feature")
                      : !table ? tr("The table is a .ktx2 file")
                               : QString{});
}

AudioBakeChoice GraphCanvasBakeDialog::choice() const {
    AudioBakeChoice choice;
    choice.rms = rms_->isChecked();
    choice.peak = peak_->isChecked();
    choice.bands = bands_->isChecked();
    choice.band_count = band_count_->value();
    choice.hop_ms = hop_->value();
    choice.attack_ms = attack_->value();
    choice.release_ms = release_->value();
    choice.channels = channels_->currentData().toString().toStdString();
    return choice;
}

QString GraphCanvasBakeDialog::out() const { return out_->text().trimmed(); }

// --- The controller.

GraphCanvasAudioAnalysis::GraphCanvasAudioAnalysis(GraphCanvasReadCapability &graph, GraphCanvasModel &model,
                                                   QWidget &owner, Hooks hooks)
    : graph_{graph}, model_{model}, owner_{owner}, hooks_{std::move(hooks)} {
    bake_row_ = new QWidget(&owner);
    bake_row_->setObjectName(QStringLiteral("pelican.graphCanvasBake"));
    auto *column = new QVBoxLayout(bake_row_);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);
    auto *line_row = new QHBoxLayout;
    line_row->setContentsMargins(0, 0, 0, 0);
    bake_line_ = new QLabel(bake_row_);
    bake_line_->setObjectName(QStringLiteral("pelican.graphCanvasBake.line"));
    bake_line_->setFont(studioSmallFont());
    bake_line_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    line_row->addWidget(bake_line_, 1);
    use_in_graph_ = new QPushButton(QObject::tr("Use in Graph"), bake_row_);
    use_in_graph_->setObjectName(QStringLiteral("pelican.graphCanvasBake.useInGraph"));
    use_in_graph_->setToolTip(QObject::tr("Place a value.sample_table that reads the table's first row "
                                          "at the scene time."));
    line_row->addWidget(use_in_graph_);
    use_in_shader_ = new QPushButton(QObject::tr("Use in a shader"), bake_row_);
    use_in_shader_->setObjectName(QStringLiteral("pelican.graphCanvasBake.useInShader"));
    use_in_shader_->setToolTip(QObject::tr(
        "Place the table as a persistent image and its layout as a vec4 buffer, connected to the "
        "picked pass when its shader declares them."));
    line_row->addWidget(use_in_shader_);
    column->addLayout(line_row);
    auto *shader_row = new QHBoxLayout;
    shader_row->setContentsMargins(0, 0, 0, 0);
    shader_line_ = new QLineEdit(bake_row_);
    shader_line_->setObjectName(QStringLiteral("pelican.graphCanvasBake.shaderLine"));
    shader_line_->setReadOnly(true);
    shader_line_->setFont(studioSmallFont());
    shader_row->addWidget(shader_line_, 1);
    copy_ = new QPushButton(QObject::tr("Copy"), bake_row_);
    copy_->setObjectName(QStringLiteral("pelican.graphCanvasBake.copy"));
    copy_->setToolTip(QObject::tr("Copy the line for the shader."));
    shader_row->addWidget(copy_);
    column->addLayout(shader_row);
    shader_line_->hide();
    copy_->hide();
    use_in_graph_->hide();
    use_in_shader_->hide();
    bake_row_->hide();
    QObject::connect(use_in_graph_, &QPushButton::clicked, &owner_, [this] {
        if (baked_ && hooks_.useInGraph) hooks_.useInGraph(*baked_);
    });
    QObject::connect(use_in_shader_, &QPushButton::clicked, &owner_, [this] {
        if (baked_ && hooks_.useInShader) hooks_.useInShader(*baked_);
    });
    QObject::connect(copy_, &QPushButton::clicked, &owner_, [this] {
        if (auto *clipboard = QApplication::clipboard()) clipboard->setText(shader_line_->text());
    });

    stale_row_ = new QWidget(&owner);
    stale_row_->setObjectName(QStringLiteral("pelican.graphCanvasStale"));
    auto *stale_layout = new QHBoxLayout(stale_row_);
    stale_layout->setContentsMargins(0, 0, 0, 0);
    stale_line_ = new QLabel(stale_row_);
    stale_line_->setObjectName(QStringLiteral("pelican.graphCanvasStale.line"));
    stale_line_->setFont(studioSmallFont());
    stale_line_->setStyleSheet(QStringLiteral("color: #e67e22;"));
    stale_layout->addWidget(stale_line_, 1);
    rebake_ = new QPushButton(QObject::tr("Re-bake"), stale_row_);
    rebake_->setObjectName(QStringLiteral("pelican.graphCanvasStale.rebake"));
    rebake_->setToolTip(QObject::tr("Bake the table again from its sound, with the settings its "
                                    "manifest records."));
    stale_layout->addWidget(rebake_);
    stale_row_->hide();
    QObject::connect(rebake_, &QPushButton::clicked, &owner_, [this] {
        if (!shown_asset_.empty()) rebake(shown_asset_);
    });

    clock_ = new QTimer(&owner);
    clock_->setInterval(PollMilliseconds);
    QObject::connect(clock_, &QTimer::timeout, &owner_, [this] { tick(); });
}

GraphCanvasAudioAnalysis::~GraphCanvasAudioAnalysis() = default;

void GraphCanvasAudioAnalysis::say(bool error, const std::string &line) {
    bake_line_->setText(qtext(line));
    bake_line_->setToolTip(qtext(line));
    bake_line_->setStyleSheet(error ? QStringLiteral("color: #c0392b;") : QString{});
    bake_row_->show();
    model_.setEditNotice({error ? InspectorNoticeKind::Error : InspectorNoticeKind::Information, line});
    if (hooks_.notice) hooks_.notice();
}

void GraphCanvasAudioAnalysis::chooseBake(const std::string &sound) {
    if (!available()) {
        say(true, "This engine connection cannot bake a sound's analysis.");
        return;
    }
    if (sound.empty()) {
        say(true, "Pick a sound to bake first.");
        return;
    }
    // Opened window-modal and answered as it closes, as the palette is: no
    // event loop of its own under the click that opened it.
    auto *dialog = new GraphCanvasBakeDialog{qtext(soundName(sound)), qtext(defaultAnalysisOut(sound)), &owner_};
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    QObject::connect(dialog, &QDialog::finished, &owner_, [this, dialog, sound](int result) {
        if (result != QDialog::Accepted) return;
        const auto out = dialog->out().toStdString();
        const auto settings = audioBakeSettings(dialog->choice());
        QMetaObject::invokeMethod(
            &owner_, [this, sound, out, settings] { bake(Json{{"asset", sound}, {"out", out}, {"settings", settings}}, false); },
            Qt::QueuedConnection);
    });
    dialog->open();
}

bool GraphCanvasAudioAnalysis::bake(const Json &params, bool rebake) {
    if (!available()) {
        say(true, "This engine connection cannot bake a sound's analysis.");
        return false;
    }
    if (running()) {
        say(true, out_ + " is being baked; wait for it to finish.");
        return false;
    }
    const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(params.dump()));
    QString error;
    bake_request_ = graph_.requestBakeAudioAnalysis(document.object(), &error);
    if (bake_request_ == 0) {
        say(true, error.isEmpty() ? std::string{"bake_audio_analysis could not be sent."} : error.toStdString());
        return false;
    }
    sound_ = params.value("asset", std::string{});
    out_ = projectRelativeReference(params.value("out", std::string{}));
    rebaking_ = rebake;
    started_ = std::chrono::steady_clock::now();
    use_in_graph_->hide();
    use_in_shader_->hide();
    shader_line_->hide();
    copy_->hide();
    rebake_->setEnabled(false);
    tick();
    clock_->start();
    return true;
}

bool GraphCanvasAudioAnalysis::rebake(const std::string &asset) {
    const auto params = rebakeParams(project_root_, asset);
    if (params.is_null()) {
        say(true, "The manifest of " + projectRelativeReference(asset) +
                      " cannot be read, so it cannot be baked again; bake it from its sound.");
        return false;
    }
    return bake(params, true);
}

void GraphCanvasAudioAnalysis::tick() {
    if (!started_) {
        clock_->stop();
        return;
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                          *started_)
                             .count();
    // Only the row: the main notice says it once, as the bake starts.
    const auto line = "Baking " + soundName(sound_) + "\xE2\x80\xA6 (" + std::to_string(seconds) + " s)";
    if (bake_line_->text() != qtext(line)) {
        bake_line_->setText(qtext(line));
        bake_line_->setStyleSheet(QString{});
        bake_row_->show();
        if (seconds == 0) {
            model_.setEditNotice({InspectorNoticeKind::Information, line});
            if (hooks_.notice) hooks_.notice();
        }
    }
    if (!ticket_.isEmpty() && ticket_request_ == 0) requestTicket();
}

void GraphCanvasAudioAnalysis::requestTicket() {
    if (ticket_.isEmpty() || ticket_request_ != 0 || !graph_.requestEditResult) return;
    QString error;
    ticket_request_ = graph_.requestEditResult(ticket_, &error);
    if (ticket_request_ != 0) return;
    ticket_.clear();
    started_.reset();
    clock_->stop();
    rebake_->setEnabled(true);
    say(true, error.toStdString());
}

void GraphCanvasAudioAnalysis::finished(const Json &response) {
    ticket_.clear();
    started_.reset();
    clock_->stop();
    rebake_->setEnabled(true);
    const auto status = response.is_object() ? response.value("status", std::string{}) : std::string{};
    if (status != "committed") {
        const auto error = response.is_object() ? response.value("error", Json::object()) : Json::object();
        auto line = audioAnalysisRefusalDescription(error);
        if (line.empty())
            line = error.is_object() && error.contains("message")
                       ? error.value("code", std::string{}) + ": " + error.value("message", std::string{})
                       : "The bake of " + soundName(sound_) + " was refused.";
        say(true, line);
        return;
    }
    baked_ = readAnalysisTable(project_root_, out_);
    if (!baked_) {
        // The engine wrote it where Studio cannot read it back: what the
        // answer says is enough to use it.
        GraphCanvasAnalysisTable table;
        table.asset = out_;
        table.source = projectRelativeReference(sound_);
        try {
            table.layout = Pelican::parseSampleTableLayout(response.value("layout", Json{}));
            baked_ = std::move(table);
        } catch (const std::exception &) {
        }
    }
    say(false, bakedLine(response));
    use_in_graph_->setVisible(baked_.has_value());
    use_in_shader_->setVisible(baked_.has_value());
    readDerived();
    if (rebaking_ && hooks_.rebaked) hooks_.rebaked(out_);
}

void GraphCanvasAudioAnalysis::showShaderLine(const std::string &line, const std::string &notice) {
    shader_line_->setText(qtext(line));
    shader_line_->setCursorPosition(0);
    shader_line_->show();
    copy_->show();
    bake_row_->show();
    if (!notice.empty()) {
        model_.setEditNotice({InspectorNoticeKind::Information, notice});
        if (hooks_.notice) hooks_.notice();
    }
}

void GraphCanvasAudioAnalysis::readDerived() {
    if (!graph_.requestDerivedAssets) return;
    if (derived_request_ != 0) {
        derived_again_ = true;
        return;
    }
    QString error;
    derived_request_ = graph_.requestDerivedAssets(&error);
}

const GraphCanvasDerivedAsset *GraphCanvasAudioAnalysis::derived(std::string_view asset) const {
    const auto relative = projectRelativeReference(asset);
    for (const auto &entry : derived_)
        if (entry.asset == relative) return &entry;
    return nullptr;
}

void GraphCanvasAudioAnalysis::showNode(const std::string &table_asset, bool editable) {
    shown_asset_ = projectRelativeReference(table_asset);
    const auto *entry = shown_asset_.empty() ? nullptr : derived(shown_asset_);
    if (entry == nullptr || !entry->stale) {
        stale_row_->hide();
        return;
    }
    stale_line_->setText(qtext(staleBandText(*entry)));
    stale_line_->setToolTip(QObject::tr("%1 was baked from %2, which is not what it was baked from any more")
                                .arg(qtext(entry->asset), qtext(entry->source)));
    rebake_->setEnabled(editable && available() && !running());
    stale_row_->show();
}

bool GraphCanvasAudioAnalysis::deliverResult(qint64 id, const QByteArray &bytes) {
    if (id == 0) return false;
    const auto parse = [&] {
        return Json::parse(bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
    };
    if (id == derived_request_) {
        derived_request_ = 0;
        const auto list = parse();
        if (!list.is_discarded()) derived_ = parseDerivedAssets(list);
        if (std::exchange(derived_again_, false)) readDerived();
        if (hooks_.staleChanged) hooks_.staleChanged();
        return true;
    }
    if (id != bake_request_ && id != ticket_request_) return false;
    const bool submission = id == bake_request_;
    (submission ? bake_request_ : ticket_request_) = 0;
    const auto response = parse();
    const auto status = response.is_object() ? response.value("status", std::string{}) : std::string{};
    if (submission && status == "accepted" && response.contains("ticket") && response.at("ticket").is_string()) {
        ticket_ = qtext(response.at("ticket").get<std::string>());
        requestTicket();
        return true;
    }
    if (!submission && (status == "running" || status == "pending")) return true; // the clock asks again
    finished(response.is_discarded() ? Json::object() : response);
    return true;
}

bool GraphCanvasAudioAnalysis::deliverFailure(qint64 id, const QString &message) {
    if (id == 0) return false;
    if (id == derived_request_) {
        derived_request_ = 0;
        derived_again_ = false;
        return true;
    }
    if (id != bake_request_ && id != ticket_request_) return false;
    bake_request_ = 0;
    ticket_request_ = 0;
    ticket_.clear();
    started_.reset();
    clock_->stop();
    rebake_->setEnabled(true);
    say(true, message.toStdString());
    return true;
}

} // namespace PelicanStudio
