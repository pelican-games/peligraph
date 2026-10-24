#pragma once

// NB-3 (docs/design_node_bodies.md 2.5): the canvas nodes' pictures, made off
// the UI thread. A body asks for one (request) while it draws; the answer in
// the cache is drawn at once, and a picture not made yet is a grey frame
// until a worker has drawn it with the CPU reference (graphcanvasthumbnail).
// The frames on the scene that wait for it are then given it in place --
// nothing is drawn again.

#include "../model/graphcanvasthumbnail.hpp"

#include <QGraphicsRectItem>
#include <QImage>
#include <QObject>
#include <QString>
#include <QThreadPool>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QGraphicsScene;

namespace PelicanStudio {

class GraphCanvasModel;

// The picture's digest (the cache key), and its state: "pending" while it is
// made, "ready" once drawn, "failed" when it cannot be (the tooltip says
// why), "none" without a model to draw from.
inline constexpr int GraphCanvasThumbnailDigestRole = Qt::UserRole + 3159;
inline constexpr int GraphCanvasThumbnailStateRole = Qt::UserRole + 3160;
// NB-4: the part of the canvas a cut picture shows (a QRectF in canvas units);
// empty for one of the whole canvas.
inline constexpr int GraphCanvasThumbnailRangeRole = Qt::UserRole + 3162;

struct GraphCanvasThumbnailState {
    QString digest;
    QImage image;
    QString error;
    bool pending = false;
    // NB-4: the part shown, when the picture is cut to where the layer draws.
    std::optional<QRectF> range;
};

// A picture's frame: the frame's own grey, and the picture over it once
// there is one (kept to its aspect, centred). Its kind and data are the
// body's; the picture is painted by the frame itself, so a press on it is a
// press on the frame.
class GraphCanvasThumbnailFrame final : public QGraphicsRectItem {
  public:
    GraphCanvasThumbnailFrame(const QRectF &rect, QGraphicsItem *parent, QString tooltip);
    void show(const GraphCanvasThumbnailState &state);
    [[nodiscard]] const QImage &image() const noexcept { return image_; }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

  private:
    QImage image_;
    QString tooltip_;
};

class GraphCanvasThumbnails final : public QObject {
  public:
    explicit GraphCanvasThumbnails(QGraphicsScene &scene);
    ~GraphCanvasThumbnails() override;
    GraphCanvasThumbnails(const GraphCanvasThumbnails &) = delete;
    GraphCanvasThumbnails &operator=(const GraphCanvasThumbnails &) = delete;

    void setModel(const GraphCanvasModel *model);
    // Where `project://` references are read (images, fonts).
    void setProjectRoot(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path &projectRoot() const noexcept { return root_; }

    // The picture for `target` as it is now: drawn, failed, or pending (a
    // worker is making it). Without a model, an empty state.
    [[nodiscard]] GraphCanvasThumbnailState request(const GraphCanvasThumbnailTarget &target);

    // --- for tests: what the cache did, and the worker's side.
    struct Counts {
        std::size_t hits = 0;
        std::size_t misses = 0;
        std::size_t drawn = 0;
    };
    [[nodiscard]] const Counts &counts() const noexcept { return counts_; }
    // The targets that missed, in order.
    [[nodiscard]] const std::vector<GraphCanvasThumbnailTarget> &missed() const noexcept { return missed_; }
    void resetCounts();
    // Run on the worker before each picture is drawn.
    void setWorkerHook(std::function<void()> hook) { worker_hook_ = std::move(hook); }
    [[nodiscard]] std::size_t inFlight() const noexcept { return in_flight_; }
    // Runs the event loop until no picture is being made (false on timeout).
    bool waitUntilIdle(int timeout_ms);

  private:
    struct Entry {
        bool done = false;
        QImage image;
        QString error;
        std::optional<QRectF> range;
    };
    void finished(const std::string &digest, GraphCanvasThumbnailPixels pixels);
    // NB-5: what a frame shows while its new picture is made -- the last
    // picture drawn for the same target (a stack's composite after its
    // layers moved), so it does not go grey for the wait.
    void showPrevious(const GraphCanvasThumbnailTarget &target, const std::string &digest,
                      GraphCanvasThumbnailState &state);
    [[nodiscard]] const Pelican::CanvasValues &liveValues(const std::string &scope);

    QGraphicsScene &scene_;
    const GraphCanvasModel *model_ = nullptr;
    std::filesystem::path root_;
    QThreadPool pool_;
    std::map<std::string, Entry, std::less<>> cache_;
    std::size_t in_flight_ = 0;
    // The published values, per scope, for the document and reports in hand.
    std::uint64_t values_content_ = ~std::uint64_t{0};
    std::uint64_t values_report_ = ~std::uint64_t{0};
    std::map<std::string, Pelican::CanvasValues, std::less<>> values_;
    // NB-5: the last picture each target was given, and the targets each
    // picture being made is for.
    std::map<GraphCanvasThumbnailTarget, Entry> shown_;
    std::map<std::string, std::vector<GraphCanvasThumbnailTarget>, std::less<>> waiting_;
    Counts counts_;
    std::vector<GraphCanvasThumbnailTarget> missed_;
    std::function<void()> worker_hook_;
};

// Studio's picture decoder (QImage): straight RGBA bytes.
[[nodiscard]] std::optional<Pelican::CanvasReferenceImage> decodeGraphCanvasImage(const std::filesystem::path &file);

} // namespace PelicanStudio
