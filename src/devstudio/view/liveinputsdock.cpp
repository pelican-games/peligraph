#include "liveinputsdock.hpp"
#include "liveinputslearn.hpp"

#include <nlohmann/json.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
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
constexpr int kLearningReadIntervalMs = 100;
constexpr int kReadTimeoutMs = 2000;

nlohmann::json parseJson(const QByteArray &bytes)
{
    return nlohmann::json::parse(bytes.constData(), bytes.constData() + bytes.size(),
                                 nullptr, false);
}

QJsonObject qtObject(const nlohmann::json &value)
{
    const auto bytes = QByteArray::fromStdString(value.dump());
    return QJsonDocument::fromJson(bytes).object();
}

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

class LearnEscapeFilter final : public QObject {
  public:
    explicit LearnEscapeFilter(std::function<void()> cancel, QObject *parent)
        : QObject(parent), cancel_(std::move(cancel)) {}

  protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            cancel_();
            return true;
        }
        return false;
    }

  private:
    std::function<void()> cancel_;
};

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
        if (request_id == 0) return false;
        if (request_id == bindings_read_) {
            const qint64 engine = bindings_engine_;
            const QString profile = bindings_profile_;
            bindings_read_ = 0;
            if (hooks_.engine_identity && hooks_.engine_identity() != engine) return true;
            if (bindings_known_ && profile != profileKey(profile_)) return true;
            ingestBindings(parseJson(result));
            return true;
        }
        if (request_id == edit_inflight_) {
            const qint64 engine = edit_engine_;
            const QString profile = edit_profile_;
            edit_inflight_ = 0;
            if (hooks_.engine_identity && hooks_.engine_identity() != engine) return true;
            if (profile != profileKey(profile_)) {
                pending_edit_.reset();
                return true;
            }
            ingestEditResult(parseJson(result));
            return true;
        }
        if (request_id != inflight_) return false;
        const qint64 engine = inflight_engine_;
        const bool arming = inflight_arm_;
        inflight_ = 0;
        inflight_arm_ = false;
        if (hooks_.engine_identity && hooks_.engine_identity() != engine) return true;
        ingest(result, arming);
        return true;
    }

    bool receiveFailure(qint64 request_id, const QString &message)
    {
        if (request_id == 0) return false;
        if (request_id == bindings_read_) {
            const qint64 engine = bindings_engine_;
            const QString profile = bindings_profile_;
            bindings_read_ = 0;
            if (hooks_.engine_identity && hooks_.engine_identity() != engine) return true;
            if (bindings_known_ && profile != profileKey(profile_)) return true;
            bindings_status_ = message;
            refreshNote();
            return true;
        }
        if (request_id == edit_inflight_) {
            const qint64 engine = edit_engine_;
            const QString profile = edit_profile_;
            edit_inflight_ = 0;
            if (hooks_.engine_identity && hooks_.engine_identity() != engine) return true;
            pending_edit_.reset();
            if (profile != profileKey(profile_)) return true;
            bindings_status_ = message;
            refreshNote();
            updateBindingControls();
            return true;
        }
        if (request_id != inflight_) return false;
        const qint64 engine = inflight_engine_;
        const bool was_armed = inflight_arm_;
        inflight_ = 0;
        inflight_arm_ = false;
        if (hooks_.engine_identity && hooks_.engine_identity() != engine) return true;
        if (was_armed) cancelLearning();
        listener_.reset();
        listener_label_->setText(QStringLiteral("No answer from the engine"));
        resetRate();
        return true;
    }

    bool receiveRefusal(qint64 request_id, const QString &error_code,
                        const QString &message)
    {
        if (request_id == 0 || request_id != edit_inflight_) return false;
        const qint64 engine = edit_engine_;
        const QString profile = edit_profile_;
        edit_inflight_ = 0;
        pending_edit_.reset();
        if (hooks_.engine_identity && hooks_.engine_identity() != engine) return true;
        if (profile != profileKey(profile_)) return true;
        bindings_status_ = message;
        if (error_code == QStringLiteral("external_modification")) {
            undo_.clear();
            bindings_known_ = false;
            QTimer::singleShot(0, &owner_, [this] { sendBindingsRead(); });
        }
        refreshNote();
        updateBindingControls();
        return true;
    }

    void engineAvailable()
    {
        if (!engine_) {
            engine_ = true;
            inputs_->clear();
            items_.clear();
            learn_buttons_.clear();
            recent_->clear();
            undo_.clear();
            bindings_known_ = false;
            bindings_read_ = 0;
            edit_inflight_ = 0;
            pending_edit_.reset();
            last_sequence_ = -1;
            cancelLearning();
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
        bindings_read_ = 0;
        edit_inflight_ = 0;
        pending_edit_.reset();
        bindings_known_ = false;
        undo_.clear();
        cancelLearning();
        timer_.stop();
        declined_note_.clear();
        refreshNote();
        listener_.reset();
        listener_label_->setText(QStringLiteral("No answer from the engine"));
        inputs_->clear();
        items_.clear();
        learn_buttons_.clear();
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
        inflight_arm_ = false;
        arm_next_read_ = false;
        cancelLearning();
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
        inputs_->setColumnCount(6);
        inputs_->setHeaderLabels({QStringLiteral("Input"), QStringLiteral("Type"),
                                  QStringLiteral("Value"), QStringLiteral("Source"),
                                  QStringLiteral("Bound to"), QStringLiteral("Learn")});
        inputs_->header()->setStretchLastSection(true);
        inputs_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        inputs_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        inputs_->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        inputs_->setRootIsDecorated(false);
        inputs_->setUniformRowHeights(true);
        inputs_->setSelectionBehavior(QAbstractItemView::SelectRows);
        layout->addWidget(inputs_, 1);

        learn_row_ = new QWidget(&owner_);
        learn_row_->setObjectName(QStringLiteral("pelican.liveInputs.learnRow"));
        auto *learn_layout = new QHBoxLayout(learn_row_);
        learn_layout->setContentsMargins(0, 0, 0, 0);
        learn_state_ = new QLabel(learn_row_);
        learn_state_->setWordWrap(true);
        learn_layout->addWidget(learn_state_, 1);
        learn_candidates_ = new QComboBox(learn_row_);
        learn_candidates_->setObjectName(QStringLiteral("pelican.liveInputs.learnCandidates"));
        learn_layout->addWidget(learn_candidates_, 1);
        learn_bind_ = new QPushButton(QStringLiteral("Bind"), learn_row_);
        learn_bind_->setObjectName(QStringLiteral("pelican.liveInputs.learnBind"));
        learn_layout->addWidget(learn_bind_);
        learn_cancel_ = new QPushButton(QStringLiteral("Cancel"), learn_row_);
        learn_cancel_->setObjectName(QStringLiteral("pelican.liveInputs.learnCancel"));
        learn_layout->addWidget(learn_cancel_);
        layout->addWidget(learn_row_);
        learn_row_->hide();
        QObject::connect(learn_cancel_, &QPushButton::clicked, &owner_,
                         [this] { cancelLearning(); });
        QObject::connect(learn_bind_, &QPushButton::clicked, &owner_, [this] {
            QTimer::singleShot(0, &owner_, [this] { bindCandidate(); });
        });
        QObject::connect(learn_candidates_,
                         qOverload<int>(&QComboBox::currentIndexChanged), &owner_,
                         [this](int) {
                             if (learn_candidates_->currentData().toInt() == -2) {
                                 show_all_candidates_ = true;
                                 updateLearningUi();
                                 learn_candidates_->setCurrentIndex(learn_candidates_->count() - 1);
                             }
                             updateBindingControls();
                         });
        auto *escape = new LearnEscapeFilter([this] { cancelLearning(); }, &owner_);
        learn_row_->installEventFilter(escape);
        for (QWidget *child : learn_row_->findChildren<QWidget *>())
            child->installEventFilter(escape);
        undo_bindings_ = new QPushButton(QStringLiteral("Undo"), &owner_);
        undo_bindings_->setObjectName(QStringLiteral("pelican.liveInputs.undoBindings"));
        QObject::connect(undo_bindings_, &QPushButton::clicked, &owner_, [this] {
            QTimer::singleShot(0, &owner_, [this] { undoBindings(); });
        });
        layout->addWidget(undo_bindings_, 0, Qt::AlignRight);

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
        if (!bindings_status_.isEmpty()) lines << bindings_status_;
        if (bindings_known_ && profile_.is_object() && live_ != applied_live_)
            lines << QStringLiteral("Not applied: the file changed on disk");
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
        const bool arming = learning_.state() == LiveInputsLearn::State::arming &&
                            !arm_next_read_;
        const qint64 id = hooks_.send(QStringLiteral("get_live_inputs"), QJsonObject{}, &error);
        if (id == 0) {
            listener_label_->setText(QStringLiteral("No answer from the engine"));
            resetRate();
            return;
        }
        inflight_ = id;
        inflight_arm_ = arming;
        inflight_started_ = nowMs();
        inflight_engine_ = hooks_.engine_identity ? hooks_.engine_identity() : 0;
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
            learn_buttons_.remove(it.key());
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
            QString reason = jsonString(*it, "reason");
            if (!reason.isEmpty()) {
                row->setText(3, reason);
            } else if (bound != it->end() && bound->is_boolean() && bound->get<bool>()) {
                row->setText(3, QStringLiteral("applied"));
            } else {
                row->setText(3, QStringLiteral("not applied"));
            }
        }
    }

    static QString profileKey(const nlohmann::json &profile)
    {
        if (!profile.is_object()) return {};
        return jsonString(profile, "name") + QLatin1Char('\n') +
               jsonString(profile, "reference");
    }

    void cancelLearning()
    {
        learning_.cancel();
        arm_next_read_ = false;
        inflight_arm_ = false;
        show_all_candidates_ = false;
        learn_row_->hide();
        timer_.setInterval(kReadIntervalMs);
        updateBindingControls();
    }

    void startLearning(const QString &input, const QString &type)
    {
        if (learning_.state() != LiveInputsLearn::State::idle) return;
        if (!engine_ || !bindings_known_ || !profile_.is_object() ||
            !listener_ || listener_->state != QStringLiteral("listening")) return;
        learning_.start(input.toStdString(), type.toStdString(), nowMs());
        arm_next_read_ = inflight_ != 0;
        show_all_candidates_ = false;
        learn_row_->show();
        updateLearningUi();
        timer_.setInterval(kLearningReadIntervalMs);
        if (inflight_ == 0) sendRead();
    }

    void ingest(const QByteArray &bytes, bool arming)
    {
        const auto parsed = parseJson(bytes);
        if (parsed.is_discarded() || !parsed.is_object()) {
            if (arming) cancelLearning();
            listener_.reset();
            listener_label_->setText(QStringLiteral("No answer from the engine"));
            resetRate();
            return;
        }
        const auto inputs = parsed.find("inputs");
        if (inputs != parsed.end() && inputs->is_array()) updateInputs(*inputs);
        const auto osc = parsed.find("osc");
        if (osc == parsed.end() || !osc->is_object()) {
            if (learning_.state() != LiveInputsLearn::State::idle) cancelLearning();
            listener_.reset();
            listener_label_->setText(QStringLiteral("off"));
            recent_->clear();
            resetRate();
            return;
        }
        const auto sequence = osc->value("last_sequence", std::int64_t{-1});
        if (last_sequence_ >= 0 && sequence >= 0 && sequence < last_sequence_ &&
            learning_.state() != LiveInputsLearn::State::idle) cancelLearning();
        last_sequence_ = sequence;
        if (arming && learning_.state() == LiveInputsLearn::State::arming) {
            learning_.arm(sequence < 0 ? 0 : static_cast<std::uint64_t>(sequence), nowMs());
        } else if (learning_.state() == LiveInputsLearn::State::listening ||
                   learning_.state() == LiveInputsLearn::State::heard) {
            const auto recent = osc->find("recent");
            if (recent != osc->end()) learning_.receive(*recent, nowMs());
        }
        if (arm_next_read_) {
            arm_next_read_ = false;
            QTimer::singleShot(0, &owner_, [this] {
                if (learning_.state() == LiveInputsLearn::State::arming && inflight_ == 0)
                    sendRead();
            });
        }
        updateListener(*osc);
        if (learning_.state() != LiveInputsLearn::State::idle &&
            (!listener_ || listener_->state != QStringLiteral("listening"))) {
            cancelLearning();
        }
        updateRecent(*osc);
        updateLearningUi();
        updateBindingControls();
        if (inputs != parsed.end() && inputs->is_array() && !inputs->empty() &&
            bindings_read_ == 0 && edit_inflight_ == 0) sendBindingsRead();
        const auto received = osc->find("received");
        if (received != osc->end() && received->is_number_integer())
            observeRate(received->get<qint64>());
    }

    void sendBindingsRead()
    {
        if (!engine_ || bindings_read_ != 0 || !hooks_.send) return;
        QString error;
        bindings_engine_ = hooks_.engine_identity ? hooks_.engine_identity() : 0;
        bindings_profile_ = profileKey(profile_);
        bindings_read_ = hooks_.send(QStringLiteral("get_osc_bindings"), QJsonObject{}, &error);
        if (bindings_read_ == 0) bindings_engine_ = 0;
    }

    void ingestBindings(const nlohmann::json &result)
    {
        if (!result.is_object()) return;
        const auto profile = result.value("profile", nlohmann::json{nullptr});
        if (bindings_known_ && profileKey(profile) != profileKey(profile_)) {
            undo_.clear();
            if (learning_.state() != LiveInputsLearn::State::idle) cancelLearning();
        }
        bindings_known_ = true;
        profile_ = profile;
        live_ = result.value("live", nlohmann::json::array());
        applied_live_ = result.value("applied_live", live_);
        const auto digest = result.find("digest");
        digest_ = digest != result.end() && digest->is_string()
                      ? QString::fromStdString(digest->get<std::string>()) : QString{};
        bindings_status_.clear();
        refreshNote();
        updateBindingControls();
    }

    void updateLearningUi()
    {
        using State = LiveInputsLearn::State;
        const auto state = learning_.state();
        learn_row_->setVisible(state != State::idle);
        if (state == State::idle) return;
        const QString input = QString::fromStdString(learning_.input());
        if (state == State::arming) {
            learn_state_->setText(QStringLiteral("Arming..."));
        } else if (state == State::timed_out) {
            learn_state_->setText(QStringLiteral("Timed out listening for \"%1\"").arg(input));
        } else {
            QString text = state == State::heard
                               ? QStringLiteral("Heard controls for \"%1\"").arg(input)
                               : QStringLiteral("Listening for \"%1\"... move a control").arg(input);
            if (learning_.gapWarning())
                text += QStringLiteral(" Some messages were missed; this list may be incomplete");
            learn_state_->setText(text);
        }
        const int previous_selection = learn_candidates_->currentIndex() >= 0
                                           ? learn_candidates_->currentData().toInt() : -1;
        learn_candidates_->blockSignals(true);
        learn_candidates_->clear();
        const auto &candidates = learning_.candidates();
        const std::size_t count = show_all_candidates_ ? candidates.size()
                                                       : std::min<std::size_t>(5, candidates.size());
        if (candidates.size() > 1)
            learn_candidates_->addItem(QStringLiteral("Choose a control..."), -1);
        for (std::size_t index = 0; index < count; ++index) {
            const auto &candidate = candidates[index];
            const QString suffix = candidate.confident ? QString{} : QStringLiteral(", one value seen");
            learn_candidates_->addItem(
                QStringLiteral("%1 | %2 | %3 %4 | %5 messages%6")
                    .arg(QString::fromStdString(candidate.address),
                         QString::fromStdString(candidate.sender),
                         QString::fromStdString(candidate.arg_type))
                    .arg(candidate.value, 0, 'g', 8)
                    .arg(static_cast<qulonglong>(candidate.count)).arg(suffix),
                static_cast<int>(index));
        }
        if (!show_all_candidates_ && candidates.size() > 5)
            learn_candidates_->addItem(QStringLiteral("Show all"), -2);
        if (candidates.size() > 1) {
            learn_candidates_->setCurrentIndex(
                previous_selection == -2 && show_all_candidates_
                    ? learn_candidates_->findData(static_cast<int>(count - 1))
                    : previous_selection >= 0 &&
                              static_cast<std::size_t>(previous_selection) < count
                          ? learn_candidates_->findData(previous_selection) : 0);
        }
        learn_candidates_->blockSignals(false);
        timer_.setInterval(state == State::listening || state == State::heard ||
                                   state == State::arming
                               ? kLearningReadIntervalMs : kReadIntervalMs);
        updateBindingControls();
    }

    void updateBindingControls()
    {
        const bool listening = listener_ && listener_->state == QStringLiteral("listening");
        for (auto it = items_.begin(); it != items_.end(); ++it) {
            QTreeWidgetItem *item = it.value();
            QPushButton *button = learn_buttons_.value(it.key(), nullptr);
            if (!button) {
                button = new QPushButton(QStringLiteral("Learn"), inputs_);
                button->setObjectName(QStringLiteral("pelican.liveInputs.learn.") + it.key());
                const QString name = it.key();
                const QString type = item->text(1);
                QObject::connect(button, &QPushButton::clicked, &owner_, [this, name, type] {
                    QTimer::singleShot(0, &owner_, [this, name, type] { startLearning(name, type); });
                });
                learn_buttons_.insert(it.key(), button);
                inputs_->setItemWidget(item, 5, button);
            }
            const bool enabled = engine_ && listening && bindings_known_ && profile_.is_object() &&
                                 learning_.state() == LiveInputsLearn::State::idle;
            button->setEnabled(enabled);
            button->setToolTip(!bindings_known_ || !profile_.is_object()
                                   ? QStringLiteral("Set an input profile for this project first")
                                   : !listening ? QStringLiteral("Start the OSC listener first")
                                                : QString{});
            QStringList addresses;
            auto *bound = new QWidget(inputs_);
            auto *row = new QHBoxLayout(bound);
            row->setContentsMargins(0, 0, 0, 0);
            for (std::size_t index = 0; index < live_.size(); ++index) {
                const auto &binding = live_[index];
                if (jsonString(binding, "input") != it.key()) continue;
                const QString address = jsonString(binding, "address");
                addresses << address;
                auto *label = new QLabel(address, bound);
                row->addWidget(label);
                auto *remove = new QPushButton(QStringLiteral("×"), bound);
                remove->setObjectName(QStringLiteral("pelican.liveInputs.unbind.%1")
                                          .arg(static_cast<qulonglong>(index)));
                remove->setEnabled(engine_ && bindings_known_ && profile_.is_object() &&
                                   edit_inflight_ == 0);
                const QString input = it.key();
                QObject::connect(remove, &QPushButton::clicked, &owner_, [this, address, input] {
                    QTimer::singleShot(0, &owner_, [this, address, input] { unbind(address, input); });
                });
                row->addWidget(remove);
            }
            item->setText(4, addresses.join(QStringLiteral(", ")));
            if (QWidget *old = inputs_->itemWidget(item, 4)) old->deleteLater();
            inputs_->setItemWidget(item, 4, bound);
        }
        bool already = false;
        QString other;
        const int selected = learn_candidates_->currentData().toInt();
        const auto &candidates = learning_.candidates();
        if (selected >= 0 && static_cast<std::size_t>(selected) < candidates.size()) {
            const auto address = QString::fromStdString(candidates[selected].address);
            const QString input = QString::fromStdString(learning_.input());
            for (const auto &binding : live_) {
                if (jsonString(binding, "address") != address) continue;
                if (jsonString(binding, "input") == input) already = true;
                else other = jsonString(binding, "input");
            }
            learn_bind_->setText(other.isEmpty() ? QStringLiteral("Bind")
                                                 : QStringLiteral("Replace \"%1\"").arg(other));
        }
        learn_bind_->setEnabled(selected >= 0 && !already && engine_ && profile_.is_object() &&
                                edit_inflight_ == 0);
        learn_bind_->setToolTip(already ? QStringLiteral("Already bound") : QString{});
        learn_candidates_->setEnabled(edit_inflight_ == 0);
        undo_bindings_->setEnabled(engine_ && edit_inflight_ == 0 && !undo_.empty() &&
                                   bindings_known_ && profile_.is_object());
        undo_bindings_->setToolTip(undo_.empty() ? QString{} : undo_.back().description);
    }

    void sendEdit(nlohmann::json next, QString description, bool undo = false)
    {
        if (!engine_ || edit_inflight_ != 0 || !bindings_known_ || !profile_.is_object() ||
            digest_.isEmpty() || !hooks_.send) return;
        nlohmann::json params{{"profile", profile_}, {"base_digest", digest_.toStdString()},
                              {"live", next}};
        pending_edit_ = PendingEdit{description, live_, undo};
        edit_engine_ = hooks_.engine_identity ? hooks_.engine_identity() : 0;
        edit_profile_ = profileKey(profile_);
        QString error;
        edit_inflight_ = hooks_.send(QStringLiteral("edit_osc_bindings"), qtObject(params), &error);
        if (edit_inflight_ == 0) {
            pending_edit_.reset();
            return;
        }
        updateBindingControls();
    }

    void bindCandidate()
    {
        const int selected = learn_candidates_->currentData().toInt();
        const auto &candidates = learning_.candidates();
        if (selected < 0 || static_cast<std::size_t>(selected) >= candidates.size()) return;
        const QString address = QString::fromStdString(candidates[selected].address);
        const QString input = QString::fromStdString(learning_.input());
        auto next = live_;
        bool inserted = false;
        for (auto &binding : next) {
            if (jsonString(binding, "address") != address) continue;
            if (jsonString(binding, "input") == input) return;
            binding = nlohmann::json{{"address", address.toStdString()},
                                     {"input", input.toStdString()}};
            inserted = true;
            break;
        }
        if (!inserted)
            next.push_back({{"address", address.toStdString()}, {"input", input.toStdString()}});
        cancelLearning();
        sendEdit(std::move(next), QStringLiteral("Bind %1 to %2").arg(address, input));
    }

    void unbind(const QString &address, const QString &input)
    {
        auto next = nlohmann::json::array();
        bool removed = false;
        for (const auto &binding : live_) {
            if (!removed && jsonString(binding, "address") == address &&
                jsonString(binding, "input") == input) {
                removed = true;
                continue;
            }
            next.push_back(binding);
        }
        if (removed) sendEdit(std::move(next), QStringLiteral("Unbind %1 from %2").arg(address, input));
    }

    void undoBindings()
    {
        if (undo_.empty() || edit_inflight_ != 0) return;
        sendEdit(undo_.back().previous, QStringLiteral("Undo ") + undo_.back().description, true);
    }

    void ingestEditResult(const nlohmann::json &result)
    {
        if (!pending_edit_ || !result.is_object()) return;
        const PendingEdit edit = *pending_edit_;
        pending_edit_.reset();
        if (!result.value("committed", false)) {
            bindings_status_ = QStringLiteral("The binding edit was not committed");
            refreshNote();
            updateBindingControls();
            return;
        }
        const bool changed = result.value("changed", false);
        if (!changed) {
            updateBindingControls();
            return;
        }
        if (edit.undo) {
            if (!undo_.empty()) undo_.pop_back();
        } else {
            undo_.push_back(UndoEntry{edit.description, edit.previous,
                                      QString::fromStdString(result.value("digest", std::string{}))});
        }
        if (!undo_.empty())
            undo_.back().digest = QString::fromStdString(result.value("digest", std::string{}));
        live_ = result.value("applied", live_);
        applied_live_ = live_;
        digest_ = QString::fromStdString(result.value("digest", std::string{}));
        bindings_status_ = result.contains("apply_error")
                               ? QString::fromStdString(result.value("apply_error", std::string{}))
                               : QString{};
        refreshNote();
        cancelLearning();
        updateBindingControls();
        sendBindingsRead();
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
    QWidget *learn_row_ = nullptr;
    QLabel *learn_state_ = nullptr;
    QComboBox *learn_candidates_ = nullptr;
    QPushButton *learn_bind_ = nullptr;
    QPushButton *learn_cancel_ = nullptr;
    QPushButton *undo_bindings_ = nullptr;
    QHash<QString, QPushButton *> learn_buttons_;
    LiveInputsLearn learning_;
    bool arm_next_read_ = false;
    bool inflight_arm_ = false;
    bool show_all_candidates_ = false;
    qint64 last_sequence_ = -1;
    qint64 bindings_read_ = 0;
    qint64 bindings_engine_ = 0;
    QString bindings_profile_;
    qint64 edit_inflight_ = 0;
    qint64 edit_engine_ = 0;
    QString edit_profile_;
    bool bindings_known_ = false;
    nlohmann::json profile_ = nullptr;
    nlohmann::json live_ = nlohmann::json::array();
    nlohmann::json applied_live_ = nlohmann::json::array();
    QString digest_;
    QString bindings_status_;
    struct UndoEntry {
        QString description;
        nlohmann::json previous;
        QString digest;
    };
    std::vector<UndoEntry> undo_;
    struct PendingEdit {
        QString description;
        nlohmann::json previous;
        bool undo = false;
    };
    std::optional<PendingEdit> pending_edit_;
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

bool LiveInputsDock::receiveRefusal(qint64 request_id, const QString &error_code,
                                    const QString &message)
{
    return impl_->receiveRefusal(request_id, error_code, message);
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
