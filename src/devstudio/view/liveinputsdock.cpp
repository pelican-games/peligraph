#include "liveinputsdock.hpp"

#include <nlohmann/json.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <utility>

namespace PelicanStudio {
namespace {

constexpr int kReadIntervalMs = 500;
constexpr int kReadTimeoutMs = 2000;

QString jsonCell(const nlohmann::json &value)
{
    if (value.is_string()) return QString::fromStdString(value.get<std::string>());
    if (value.is_boolean()) return value.get<bool>() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.is_null()) return QStringLiteral("null");
    if (value.is_number_integer()) return QString::number(value.get<qint64>());
    if (value.is_number_unsigned()) return QString::number(value.get<quint64>());
    if (value.is_number_float()) return QString::number(value.get<double>(), 'g', 8);
    return QString::fromStdString(value.dump());
}

QString jsonString(const nlohmann::json &object, const char *key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) return {};
    return QString::fromStdString(found->get<std::string>());
}

} // namespace

class LiveInputsDock::Impl {
  public:
    Impl(LiveInputsDock &owner, Hooks hooks, QSettings &settings)
        : owner_(owner),
          hooks_(std::move(hooks)),
          store_(settings),
          shown_(owner,
                 [this] { caughtUpOnShow(); },
                 [this] { wentOutOfView(); }) {
        clock_.start();
        saved_ = store_.load();
        buildUi();
        environment_supplies_options_ =
            hooks_.configured_arguments &&
            studioOscOptionsFromEnvironment(hooks_.configured_arguments());
        listen_->setChecked(saved_.enabled);
        port_->setValue(saved_.port);
        interface_->setCurrentText(saved_.interface_address);
        allow_->setText(saved_.allow);
        settings_group_->setEnabled(studioBuildHasOsc() && !environment_supplies_options_);
        timer_.setInterval(kReadIntervalMs);
        QObject::connect(&timer_, &QTimer::timeout, &owner_,
                         [this] { poll(); });
        QObject::connect(listen_, &QCheckBox::toggled, &owner_,
                         [this] { edited(); });
        QObject::connect(port_, &QSpinBox::valueChanged, &owner_,
                         [this] { edited(); });
        QObject::connect(interface_, &QComboBox::currentTextChanged, &owner_,
                         [this] { edited(); });
        QObject::connect(interface_, &QComboBox::editTextChanged, &owner_,
                         [this] { edited(); });
        QObject::connect(allow_, &QLineEdit::textChanged, &owner_,
                         [this] { edited(); });
        QObject::connect(apply_, &QPushButton::clicked, &owner_,
                         [this] { apply(); });
        resetRate();
        updateApply();
        refreshNote();
    }

    ~Impl() { timer_.stop(); }

    bool receiveResult(qint64 request_id, const QByteArray &result)
    {
        if (request_id == 0 || request_id != inflight_) return false;
        const qint64 engine = inflight_engine_;
        inflight_ = 0;
        if (hooks_.engine_identity && hooks_.engine_identity() != engine) {
            // A reply from the engine this Studio stopped.  It is consumed but
            // says nothing about the engine that is running now.
            return true;
        }
        ingest(result);
        return true;
    }

    bool receiveFailure(qint64 request_id, const QString &)
    {
        if (request_id == 0 || request_id != inflight_) return false;
        inflight_ = 0;
        listener_.reset();
        listener_label_->setText(QStringLiteral("No answer from the engine"));
        resetRate();
        return true;
    }

    void engineAvailable()
    {
        if (!engine_) {
            engine_ = true;
            inputs_->clear();
            items_.clear();
            recent_->clear();
        }
        listener_.reset();
        listener_label_->setText(QStringLiteral("No answer from the engine"));
        resetRate();
        // The sentence about an earlier declined restart belongs to that
        // engine; this one started, so it is no longer true.
        declined_note_.clear();
        refreshNote();
        if (shown_.isShown()) poll();
        updateTimer();
    }

    void engineUnavailable()
    {
        engine_ = false;
        inflight_ = 0;
        timer_.stop();
        declined_note_.clear();
        refreshNote();
        listener_.reset();
        listener_label_->setText(QStringLiteral("No answer from the engine"));
        inputs_->clear();
        items_.clear();
        recent_->clear();
        resetRate();
        updateApply();
    }

    void engineRestartDeclined(const QString &message)
    {
        declined_note_ = message;
        refreshNote();
    }

    void readNow()
    {
        if (!engine_ || !shown_.isShown()) return;
        if (inflight_ != 0 && timedOut()) return;
        if (inflight_ != 0) return;
        sendRead();
    }

    void caughtUpOnShow()
    {
        resetRate();
        updateTimer();
        poll();
    }

    void wentOutOfView()
    {
        inflight_ = 0;
        resetRate();
        updateTimer();
    }

  private:
    void buildUi()
    {
        owner_.setObjectName(QStringLiteral("pelican.liveInputsDock"));
        auto *layout = new QVBoxLayout(&owner_);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);

        auto *headline = new QHBoxLayout;
        auto *listener_caption = new QLabel(QStringLiteral("Listener:"), &owner_);
        listener_label_ = new QLabel(QStringLiteral("No answer from the engine"), &owner_);
        listener_label_->setObjectName(QStringLiteral("pelican.liveInputs.listener"));
        listener_label_->setWordWrap(true);
        listener_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *rate_caption = new QLabel(QStringLiteral("Rate:"), &owner_);
        rate_label_ = new QLabel(QStringLiteral("—"), &owner_);
        rate_label_->setObjectName(QStringLiteral("pelican.liveInputs.rate"));
        rate_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        headline->addWidget(listener_caption);
        headline->addWidget(listener_label_, 1);
        headline->addSpacing(12);
        headline->addWidget(rate_caption);
        headline->addWidget(rate_label_);
        layout->addLayout(headline);

        auto *inputs_caption = new QLabel(QStringLiteral("Declared inputs"), &owner_);
        layout->addWidget(inputs_caption);
        inputs_ = new QTreeWidget(&owner_);
        inputs_->setObjectName(QStringLiteral("pelican.liveInputs.table"));
        inputs_->setColumnCount(4);
        inputs_->setHeaderLabels({QStringLiteral("Input"), QStringLiteral("Type"),
                                  QStringLiteral("Value"), QStringLiteral("Source")});
        inputs_->header()->setStretchLastSection(true);
        inputs_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        inputs_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        inputs_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        inputs_->setRootIsDecorated(false);
        inputs_->setUniformRowHeights(true);
        inputs_->setSelectionBehavior(QAbstractItemView::SelectRows);
        layout->addWidget(inputs_, 1);

        auto *recent_caption = new QLabel(QStringLiteral("Recent OSC traffic"), &owner_);
        layout->addWidget(recent_caption);
        recent_ = new QTreeWidget(&owner_);
        recent_->setObjectName(QStringLiteral("pelican.liveInputs.recent"));
        recent_->setColumnCount(4);
        recent_->setHeaderLabels({QStringLiteral("Address"), QStringLiteral("Input"),
                                  QStringLiteral("Sender"), QStringLiteral("Result")});
        recent_->header()->setStretchLastSection(true);
        recent_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        recent_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        recent_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        recent_->setRootIsDecorated(false);
        recent_->setUniformRowHeights(true);
        recent_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        layout->addWidget(recent_, 1);

        settings_group_ = new QGroupBox(QStringLiteral("OSC listener"), &owner_);
        settings_group_->setObjectName(QStringLiteral("pelican.liveInputs.settings"));
        auto *settings_layout = new QVBoxLayout(settings_group_);
        auto *scope = new QLabel(QStringLiteral("For this user, in every project."),
                                 settings_group_);
        scope->setObjectName(QStringLiteral("pelican.liveInputs.scope"));
        settings_layout->addWidget(scope);
        auto *form = new QFormLayout;
        listen_ = new QCheckBox(QStringLiteral("Listen for OSC"), settings_group_);
        listen_->setObjectName(QStringLiteral("pelican.liveInputs.enabled"));
        form->addRow(QString(), listen_);
        port_ = new QSpinBox(settings_group_);
        port_->setObjectName(QStringLiteral("pelican.liveInputs.port"));
        port_->setRange(0, 65535);
        port_->setToolTip(
            QStringLiteral("0 lets the operating system choose a free port."));
        form->addRow(QStringLiteral("Port"), port_);
        interface_ = new QComboBox(settings_group_);
        interface_->setObjectName(QStringLiteral("pelican.liveInputs.interface"));
        interface_->setEditable(true);
        interface_->addItems({QStringLiteral("127.0.0.1"), QStringLiteral("0.0.0.0")});
        form->addRow(QStringLiteral("Interface"), interface_);
        allow_ = new QLineEdit(settings_group_);
        allow_->setObjectName(QStringLiteral("pelican.liveInputs.allow"));
        allow_->setPlaceholderText(QStringLiteral("Any sender"));
        form->addRow(QStringLiteral("Allow senders"), allow_);
        settings_layout->addLayout(form);

        note_ = new QLabel(&owner_);
        note_->setObjectName(QStringLiteral("pelican.liveInputs.note"));
        note_->setWordWrap(true);
        note_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        settings_layout->addWidget(note_);

        apply_ = new QPushButton(QStringLiteral("Apply"), settings_group_);
        apply_->setObjectName(QStringLiteral("pelican.liveInputs.apply"));
        settings_layout->addWidget(apply_, 0, Qt::AlignRight);
        layout->addWidget(settings_group_);
    }

    StudioOscSettings formSettings() const
    {
        StudioOscSettings result;
        result.enabled = listen_->isChecked();
        result.port = port_->value();
        result.interface_address = interface_->currentText().trimmed();
        result.allow = allow_->text().trimmed();
        return result;
    }

    void edited()
    {
        updateApply();
        refreshNote();
    }

    void updateApply()
    {
        validation_error_ = validateStudioOscSettings(formSettings());
        const bool usable = studioBuildHasOsc() && !environment_supplies_options_;
        const bool changed = !(formSettings() == saved_);
        apply_->setEnabled(usable && validation_error_.isEmpty() && changed);
    }

    void refreshNote()
    {
        QStringList lines;
        if (!studioBuildHasOsc()) {
            lines << QStringLiteral(
                "This Studio was built without OSC, so no listener is started.");
        }
        if (environment_supplies_options_) {
            lines << QStringLiteral(
                "The engine's OSC options come from PELICAN_STUDIO_PLAYER_ARGUMENTS, "
                "so these settings are not used.");
        }
        if (!declined_note_.isEmpty()) lines << declined_note_;
        if (!save_error_.isEmpty()) lines << save_error_;
        if (!validation_error_.isEmpty()) {
            lines << validation_error_;
        } else if (!studioOscInterfaceIsLoopback(formSettings().interface_address)) {
            lines << QStringLiteral(
                "Anyone on this network can send values. The allow list filters by "
                "sender address; it is not authentication.");
        }
        if (settingsDifferFromListener()) {
            lines << QStringLiteral("Saved; applies the next time the engine starts.");
        }
        note_->setText(lines.join(QLatin1Char('\n')));
        note_->setVisible(!lines.isEmpty());
    }

    // The listener has only the state the engine last reported.  A port of 0
    // asks the operating system for one, so any bound port is the saved one.
    bool settingsDifferFromListener() const
    {
        if (!studioBuildHasOsc() || environment_supplies_options_ || !listener_) {
            return false;
        }
        if (saved_.enabled) {
            if (listener_->state != QStringLiteral("listening")) return true;
            if (listener_->interface_address != saved_.interface_address) return true;
            if (saved_.port != 0 && listener_->port != saved_.port) return true;
            return false;
        }
        return listener_->state != QStringLiteral("off");
    }

    void apply()
    {
        if (environment_supplies_options_ || !studioBuildHasOsc() ||
            !apply_->isEnabled()) {
            return;
        }
        save_error_.clear();
        declined_note_.clear();
        const StudioOscSettings next = formSettings();
        QString error;
        if (!store_.save(next, &error)) {
            save_error_ = error;
            refreshNote();
            return;
        }
        saved_ = next;
        if (hooks_.applied) hooks_.applied(saved_);
        updateApply();
        refreshNote();
        if (hooks_.restart) hooks_.restart(QStringLiteral("OSC settings changed"));
    }

    void updateTimer()
    {
        const bool should_read = engine_ && shown_.isShown();
        if (should_read && !timer_.isActive()) {
            timer_.start();
        } else if (!should_read && timer_.isActive()) {
            timer_.stop();
        }
    }

    bool timedOut() const
    {
        return inflight_ != 0 && nowMs() - inflight_started_ >= kReadTimeoutMs;
    }

    void poll()
    {
        if (!engine_ || !shown_.isShown()) {
            updateTimer();
            return;
        }
        if (inflight_ != 0) {
            if (!timedOut()) return;
            inflight_ = 0;
            listener_.reset();
            listener_label_->setText(QStringLiteral("No answer from the engine"));
            resetRate();
            return;
        }
        sendRead();
    }

    void sendRead()
    {
        if (!hooks_.send) {
            listener_label_->setText(QStringLiteral("No answer from the engine"));
            resetRate();
            return;
        }
        QString error;
        const qint64 id = hooks_.send(QStringLiteral("get_live_inputs"), QJsonObject{}, &error);
        if (id == 0) {
            listener_label_->setText(QStringLiteral("No answer from the engine"));
            resetRate();
            return;
        }
        inflight_ = id;
        inflight_started_ = nowMs();
        inflight_engine_ = hooks_.engine_identity ? hooks_.engine_identity() : 0;
    }

    void ingest(const QByteArray &bytes)
    {
        const auto parsed = nlohmann::json::parse(
            bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            listener_.reset();
            listener_label_->setText(QStringLiteral("No answer from the engine"));
            resetRate();
            return;
        }

        const auto inputs = parsed.find("inputs");
        if (inputs != parsed.end() && inputs->is_array()) {
            updateInputs(*inputs);
        } else {
            inputs_->clear();
            items_.clear();
        }

        const auto osc = parsed.find("osc");
        if (osc == parsed.end() || !osc->is_object()) {
            listener_.reset();
            listener_label_->setText(QStringLiteral("off"));
            recent_->clear();
            resetRate();
            return;
        }

        updateListener(*osc);
        updateRecent(*osc);

        const auto received = osc->find("received");
        if (received == osc->end() || !received->is_number_integer()) {
            resetRate();
            return;
        }
        observeRate(received->get<qint64>());
    }

    void updateInputs(const nlohmann::json &inputs)
    {
        QStringList order;
        QSet<QString> seen;
        for (const auto &input : inputs) {
            if (!input.is_object()) continue;
            const QString name = jsonString(input, "name");
            if (name.isEmpty() || seen.contains(name)) continue;
            seen.insert(name);
            order << name;
            QTreeWidgetItem *item = items_.value(name, nullptr);
            if (item == nullptr) {
                item = new QTreeWidgetItem(inputs_);
                items_.insert(name, item);
            }
            item->setText(0, name);
            item->setText(1, jsonString(input, "type"));
            const auto value = input.find("value");
            item->setText(2, value == input.end() ? QString() : jsonCell(*value));
            item->setText(3, jsonString(input, "source"));
        }

        for (auto it = items_.begin(); it != items_.end();) {
            if (seen.contains(it.key())) {
                ++it;
                continue;
            }
            delete it.value();
            it = items_.erase(it);
        }

        // Keep the engine's declaration order without rebuilding the rows:
        // selection and scroll position stay on the same items.
        for (int target = 0; target < order.size(); ++target) {
            QTreeWidgetItem *item = items_.value(order.at(target), nullptr);
            const int current = inputs_->indexOfTopLevelItem(item);
            if (current >= 0 && current != target) {
                inputs_->insertTopLevelItem(target, inputs_->takeTopLevelItem(current));
            }
        }
    }

    void updateListener(const nlohmann::json &osc)
    {
        const auto found = osc.find("listener");
        if (found == osc.end() || !found->is_object()) {
            listener_.reset();
            listener_label_->setText(QStringLiteral("off"));
            return;
        }
        ReportedListener reported;
        reported.state = jsonString(*found, "state");
        reported.interface_address = jsonString(*found, "interface");
        reported.error = jsonString(*found, "error");
        const auto port = found->find("port");
        if (port != found->end() && port->is_number_integer()) {
            reported.port = port->get<int>();
        }
        listener_ = reported;

        if (reported.state == QStringLiteral("listening")) {
            listener_label_->setText(
                QStringLiteral("listening on %1:%2")
                    .arg(reported.interface_address.isEmpty()
                             ? QStringLiteral("127.0.0.1")
                             : reported.interface_address)
                    .arg(reported.port));
        } else if (reported.state == QStringLiteral("failed")) {
            listener_label_->setText(
                QStringLiteral("failed: %1")
                    .arg(reported.error.isEmpty() ? QStringLiteral("unknown error")
                                                  : reported.error));
        } else {
            listener_label_->setText(QStringLiteral("off"));
        }
    }

    void updateRecent(const nlohmann::json &osc)
    {
        recent_->clear();
        const auto found = osc.find("recent");
        if (found == osc.end() || !found->is_array()) return;
        int shown = 0;
        for (auto it = found->rbegin(); it != found->rend() && shown < 50; ++it, ++shown) {
            if (!it->is_object()) continue;
            auto *row = new QTreeWidgetItem(recent_);
            row->setText(0, jsonString(*it, "address"));
            const QString input = jsonString(*it, "input");
            row->setText(1, input);
            row->setText(2, jsonString(*it, "sender"));
            const auto bound = it->find("bound");
            if (bound != it->end() && bound->is_boolean() && bound->get<bool>()) {
                row->setText(3, QStringLiteral("applied"));
            } else {
                QString reason = jsonString(*it, "reason");
                if (reason.isEmpty()) reason = QStringLiteral("not applied");
                row->setText(3, reason);
            }
        }
    }

    qint64 nowMs() const
    {
        return hooks_.clock_ms ? hooks_.clock_ms() : clock_.elapsed();
    }

    void observeRate(qint64 received)
    {
        if (!rate_baseline_ || received < *rate_baseline_) {
            rate_baseline_ = received;
            rate_baseline_ms_ = nowMs();
            rate_label_->setText(QStringLiteral("—"));
            return;
        }
        const qint64 elapsed = nowMs() - rate_baseline_ms_;
        if (elapsed <= 0) return;
        const double rate =
            static_cast<double>(received - *rate_baseline_) * 1000.0 /
            static_cast<double>(elapsed);
        rate_label_->setText(QString::number(rate, 'f', 1) + QStringLiteral("/s"));
        rate_baseline_ = received;
        rate_baseline_ms_ = nowMs();
    }

    void resetRate()
    {
        rate_baseline_.reset();
        rate_baseline_ms_ = 0;
        if (rate_label_ != nullptr) rate_label_->setText(QStringLiteral("—"));
    }

    struct ReportedListener {
        QString state;
        QString interface_address;
        QString error;
        int port = -1;
    };

    LiveInputsDock &owner_;
    Hooks hooks_;
    StudioOscSettingsStore store_;
    StudioOscSettings saved_;
    bool environment_supplies_options_ = false;
    QTimer timer_;
    QElapsedTimer clock_;
    QGroupBox *settings_group_ = nullptr;
    QLabel *listener_label_ = nullptr;
    QLabel *rate_label_ = nullptr;
    QLabel *note_ = nullptr;
    QTreeWidget *inputs_ = nullptr;
    QTreeWidget *recent_ = nullptr;
    QHash<QString, QTreeWidgetItem *> items_;
    QCheckBox *listen_ = nullptr;
    QSpinBox *port_ = nullptr;
    QComboBox *interface_ = nullptr;
    QLineEdit *allow_ = nullptr;
    QPushButton *apply_ = nullptr;
    std::optional<ReportedListener> listener_;
    std::optional<qint64> rate_baseline_;
    qint64 rate_baseline_ms_ = 0;
    qint64 inflight_ = 0;
    qint64 inflight_started_ = 0;
    qint64 inflight_engine_ = 0;
    bool engine_ = false;
    QString declined_note_;
    QString save_error_;
    QString validation_error_;
    PanelShown shown_;
};

LiveInputsDock::LiveInputsDock(Hooks hooks, QSettings &settings, QWidget *parent)
    : QWidget(parent),
      impl_(std::make_unique<Impl>(*this, std::move(hooks), settings)) {}

LiveInputsDock::~LiveInputsDock() = default;

bool LiveInputsDock::receiveResult(qint64 request_id, const QByteArray &result)
{
    return impl_->receiveResult(request_id, result);
}

bool LiveInputsDock::receiveFailure(qint64 request_id, const QString &message)
{
    return impl_->receiveFailure(request_id, message);
}

void LiveInputsDock::engineAvailable() { impl_->engineAvailable(); }
void LiveInputsDock::engineUnavailable() { impl_->engineUnavailable(); }
void LiveInputsDock::engineRestartDeclined(const QString &message)
{
    impl_->engineRestartDeclined(message);
}
void LiveInputsDock::readNow() { impl_->readNow(); }

void LiveInputsDock::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    impl_->caughtUpOnShow();
}

void LiveInputsDock::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    impl_->wentOutOfView();
}

} // namespace PelicanStudio
