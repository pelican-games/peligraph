#pragma once

#include <QObject>
#include <QPointer>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace PelicanStudio {

// Q-13. The one place Studio keeps "which revision of the rendering document
// the engine holds now". Every panel that edits the rendering document sends a
// base_source_digest, and every such edit -- a pass, a render target, a node's
// placement, a feature -- moves it. A panel that only learns of its own commits
// sends the digest it read before somebody else's, and the engine refuses it as
// external_modification although nothing outside Studio touched the file.
//
// So a panel that reads or commits the document says what digest it saw
// (publish), and every other panel is told (subscribe) and reads its own
// context again. The panels keep their own copy on purpose: the digest is the
// version of the context a request was built from, and a request built from
// an old context must not travel under a new digest.
class RenderConfigDigest {
public:
    using Handler = std::function<void(const std::string &digest)>;

    [[nodiscard]] const std::string &current() const noexcept { return current_; }

    // `from` is the panel that saw it; it is not told its own news. An empty
    // digest, or the one already held, tells nobody anything.
    void publish(const std::string &digest, const QObject *from = nullptr) {
        if (digest.empty() || digest == current_) return;
        current_ = digest;
        auto subscribers = subscribers_;
        for (const auto &[context, handler] : subscribers) {
            if (context.isNull() || context.data() == from || !handler) continue;
            handler(digest);
        }
        std::erase_if(subscribers_, [](const auto &entry) {
            return entry.first.isNull();
        });
    }

    // UD-5: reads and commits both refresh panels, but only a confirmed
    // commit authorizes retaining Graph inverses. Notify that fact before any
    // subscriber can request the new document, even for an already seen digest.
    void publishCommitted(const std::string &digest, const QObject *from = nullptr) {
        if (digest.empty()) return;
        const auto subscribers = commit_subscribers_;
        for (const auto &[context, handler] : subscribers)
            if (!context.isNull() && handler) handler(digest);
        std::erase_if(commit_subscribers_, [](const auto &entry) {
            return entry.first.isNull();
        });
        publish(digest, from);
    }

    void subscribeCommitted(QObject *context, Handler handler) {
        commit_subscribers_.emplace_back(QPointer<QObject>{context}, std::move(handler));
    }

    // The handler lives as long as `context` does.
    void subscribe(QObject *context, Handler handler) {
        subscribers_.emplace_back(QPointer<QObject>{context}, std::move(handler));
    }

private:
    std::vector<std::pair<QPointer<QObject>, Handler>> commit_subscribers_;
    std::string current_;
    std::vector<std::pair<QPointer<QObject>, Handler>> subscribers_;
};

} // namespace PelicanStudio
