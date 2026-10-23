#include "graphcanvasthumbnails.hpp"

#include "../model/graphcanvasmodel.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGraphicsScene>
#include <QPainter>
#include <QThread>

#include <cstring>
#include <utility>

namespace PelicanStudio {

GraphCanvasThumbnailFrame::GraphCanvasThumbnailFrame(const QRectF &rect, QGraphicsItem *parent, QString tooltip)
    : QGraphicsRectItem{rect, parent}, tooltip_{std::move(tooltip)} {
    setToolTip(tooltip_);
    setData(GraphCanvasThumbnailStateRole, QStringLiteral("none"));
}

void GraphCanvasThumbnailFrame::show(const GraphCanvasThumbnailState &state) {
    setData(GraphCanvasThumbnailDigestRole, state.digest);
    image_ = state.image;
    // NB-4: a cut picture says which part of the canvas it shows.
    setData(GraphCanvasThumbnailRangeRole, state.range ? QVariant{*state.range} : QVariant{});
    const QString shown =
        state.range ? QObject::tr("%1\nRange %2,%3 \xE2\x80\x93 %4,%5")
                          .arg(tooltip_)
                          .arg(state.range->left(), 0, 'f', 0)
                          .arg(state.range->top(), 0, 'f', 0)
                          .arg(state.range->right(), 0, 'f', 0)
                          .arg(state.range->bottom(), 0, 'f', 0)
                          .trimmed()
                    : tooltip_;
    if (state.pending) {
        setData(GraphCanvasThumbnailStateRole, QStringLiteral("pending"));
        setToolTip(tooltip_.isEmpty() ? QObject::tr("Drawing the picture...")
                                      : QObject::tr("%1 (drawing the picture...)").arg(tooltip_));
    } else if (!state.error.isEmpty()) {
        setData(GraphCanvasThumbnailStateRole, QStringLiteral("failed"));
        setToolTip(tooltip_.isEmpty() ? QObject::tr("No picture: %1").arg(state.error)
                                      : QObject::tr("%1\nNo picture: %2").arg(tooltip_, state.error));
    } else if (!image_.isNull()) {
        setData(GraphCanvasThumbnailStateRole, QStringLiteral("ready"));
        setToolTip(shown);
    } else {
        setData(GraphCanvasThumbnailStateRole, QStringLiteral("none"));
        setToolTip(tooltip_);
    }
    update();
}

void GraphCanvasThumbnailFrame::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) {
    QGraphicsRectItem::paint(painter, option, widget);
    if (image_.isNull()) return;
    const QRectF frame = rect();
    const QSizeF size = QSizeF{image_.size()}.scaled(frame.size(), Qt::KeepAspectRatio);
    const QRectF target{frame.center() - QPointF{size.width() / 2.0, size.height() / 2.0}, size};
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawImage(target, image_);
    painter->restore();
}

std::optional<Pelican::CanvasReferenceImage> decodeGraphCanvasImage(const std::filesystem::path &file) {
    QImage image;
    if (!image.load(QString::fromStdWString(file.wstring()))) return std::nullopt;
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull()) return std::nullopt;
    Pelican::CanvasReferenceImage decoded;
    decoded.width = static_cast<std::uint32_t>(image.width());
    decoded.height = static_cast<std::uint32_t>(image.height());
    const std::size_t row = std::size_t{decoded.width} * 4;
    decoded.rgba.resize(row * decoded.height);
    for (int y = 0; y < image.height(); ++y)
        std::memcpy(decoded.rgba.data() + row * static_cast<std::size_t>(y), image.constScanLine(y), row);
    return decoded;
}

GraphCanvasThumbnails::GraphCanvasThumbnails(QGraphicsScene &scene) : scene_{scene} {
    // Two at once: a stack's composite and its rows come together, and the
    // pictures must not take every core from the engine and the build.
    pool_.setMaxThreadCount(2);
}

GraphCanvasThumbnails::~GraphCanvasThumbnails() {
    // What is queued is dropped; what runs finishes before this goes (its
    // answer, posted to this object, goes with it).
    pool_.clear();
    pool_.waitForDone();
}

void GraphCanvasThumbnails::setModel(const GraphCanvasModel *model) {
    model_ = model;
    values_.clear();
    values_content_ = ~std::uint64_t{0};
}

void GraphCanvasThumbnails::setProjectRoot(std::filesystem::path root) { root_ = std::move(root); }

void GraphCanvasThumbnails::resetCounts() {
    counts_ = {};
    missed_.clear();
}

const Pelican::CanvasValues &GraphCanvasThumbnails::liveValues(const std::string &scope) {
    if (values_content_ != model_->contentRevision() || values_report_ != model_->reportRevision()) {
        values_.clear();
        values_content_ = model_->contentRevision();
        values_report_ = model_->reportRevision();
    }
    if (const auto found = values_.find(scope); found != values_.end()) return found->second;
    Pelican::CanvasValues values;
    for (const auto &node : model_->nodes(scope))
        for (const auto &output : node.outputs)
            if (output.has_value && !output.bundle) values.emplace(Pelican::CanvasValueSource{node.id, output.port}, output.value);
    return values_.emplace(scope, std::move(values)).first->second;
}

GraphCanvasThumbnailState GraphCanvasThumbnails::request(const GraphCanvasThumbnailTarget &target) {
    if (model_ == nullptr) return {};
    const nlohmann::json empty = nlohmann::json::array();
    const nlohmann::json *nodes = &model_->documentNodes();
    if (!target.scope.empty()) {
        nodes = &empty;
        const auto &definitions = model_->documentDefinitions();
        if (const auto found = definitions.find(target.scope);
            found != definitions.end() && found->is_object() && found->contains("nodes"))
            nodes = &found->at("nodes");
    }
    auto job = graphCanvasThumbnailJob(*nodes, model_->documentDefinitions(), target, liveValues(target.scope), root_);
    GraphCanvasThumbnailState state;
    state.digest = QString::fromStdString(job.digest);
    if (const auto found = cache_.find(job.digest); found != cache_.end()) {
        ++counts_.hits;
        state.pending = !found->second.done;
        state.image = found->second.image;
        state.error = found->second.error;
        state.range = found->second.range;
        return state;
    }
    ++counts_.misses;
    missed_.push_back(target);
    // A long session's pictures: what is drawn is dropped past this many,
    // what is being made is kept.
    if (cache_.size() > 2048)
        std::erase_if(cache_, [](const auto &entry) { return entry.second.done; });
    cache_.emplace(job.digest, Entry{});
    ++in_flight_;
    state.pending = true;
    pool_.start([this, job = std::move(job), root = root_, hook = worker_hook_]() mutable {
        if (hook) hook();
        auto pixels = renderGraphCanvasThumbnail(job, root, &decodeGraphCanvasImage);
        QMetaObject::invokeMethod(
            this, [this, digest = job.digest, pixels = std::move(pixels)]() mutable {
                finished(digest, std::move(pixels));
            },
            Qt::QueuedConnection);
    });
    return state;
}

void GraphCanvasThumbnails::finished(const std::string &digest, GraphCanvasThumbnailPixels pixels) {
    if (in_flight_ > 0) --in_flight_;
    ++counts_.drawn;
    auto found = cache_.find(digest);
    if (found == cache_.end()) found = cache_.emplace(digest, Entry{}).first;
    Entry &entry = found->second;
    entry.done = true;
    if (pixels.ok()) {
        entry.image = QImage{pixels.rgba.data(), static_cast<int>(pixels.width), static_cast<int>(pixels.height),
                             static_cast<qsizetype>(pixels.width) * 4, QImage::Format_RGBA8888}
                          .copy();
        if (pixels.crop) {
            const auto &crop = *pixels.crop;
            entry.range = QRectF{crop[0], crop[1], crop[2], crop[3]};
        }
    } else {
        entry.error = QString::fromStdString(pixels.error.empty() ? std::string{"Nothing was drawn"} : pixels.error);
    }
    GraphCanvasThumbnailState state;
    state.digest = QString::fromStdString(digest);
    state.image = entry.image;
    state.error = entry.error;
    state.range = entry.range;
    for (QGraphicsItem *item : scene_.items())
        if (auto *frame = dynamic_cast<GraphCanvasThumbnailFrame *>(item);
            frame != nullptr && frame->data(GraphCanvasThumbnailDigestRole).toString() == state.digest)
            frame->show(state);
}

bool GraphCanvasThumbnails::waitUntilIdle(int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (in_flight_ > 0) {
        if (timer.elapsed() > timeout_ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    QCoreApplication::processEvents();
    return true;
}

} // namespace PelicanStudio
