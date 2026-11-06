#pragma once

#include "../../project/graphfiltersdocument.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace PelicanStudio {

// A project's draft, its accepted digest and its document-only Undo history.
// Requests carry both generations so a delayed reply cannot cross a project or
// engine switch. The Graph panel owns one session per project root.
class GraphFiltersEdit {
  public:
    using Json = nlohmann::json;
    enum class RequestKind : std::uint8_t { load, save, undo };
    struct Request {
        std::int64_t id = 0;
        std::uint64_t project_generation = 0;
        std::uint64_t engine_generation = 0;
        RequestKind kind = RequestKind::load;
        std::string method;
        Json params;
    };
    struct State {
        bool loaded = false;
        bool engine_ready = false;
        bool dirty = false;
        bool editing_enabled = false;
        bool save_enabled = false;
        bool undo_enabled = false;
        bool load_enabled = false;
        bool discard_enabled = false;
        bool external_modification = false;
        bool commit_result_unknown = false;
        bool confirm_load = false;
        bool confirm_remove = false;
        std::string text;
    };
    // Where a filter sits. depth counts as the parser does: the top level is 1.
    struct FilterPlace {
        int depth = 1;
        std::size_t index = 0;
        std::size_t siblings = 0;
        std::string parent;
    };

    void activateProject(std::uint64_t generation);
    void engineReady(std::uint64_t generation);
    void engineGone();

    bool requestLoad();
    bool save();
    bool undo();
    bool discard();
    bool loadReplacesDirtyDraft();

    bool addFilter(const std::string &label);
    bool addChildFilter(const std::string &parent_id, const std::string &label);
    // A filter with descendants takes two presses: the first arms and returns
    // false, the second removes, if the id and the whole draft are unchanged.
    bool removeFilter(const std::string &id);
    bool moveFilter(const std::string &id, int delta);
    bool indentFilter(const std::string &id);
    bool outdentFilter(const std::string &id);
    bool setFilterId(const std::string &id, const std::string &new_id);
    // Doesn't fire `changed`: a selection change calls it from inside the
    // tree's own signal, where the tree mustn't be rebuilt.
    bool cancelRemoveConfirmation();
    bool setLabel(const std::string &id, const std::string &label);
    bool setMatch(const std::string &id, const Pelican::GraphFilterMatch &match);
    bool addCategory(const std::string &label);
    bool removeCategory(const std::string &id);
    bool setCategoryLabel(const std::string &id, const std::string &label);
    bool setFeatureCategories(const std::string &feature, const std::vector<std::string> &ids);
    bool importFilters(const Pelican::GraphFilters &incoming);

    [[nodiscard]] std::vector<Request> takeRequests();
    bool applyReply(std::int64_t request_id, std::uint64_t project_generation,
                    std::uint64_t engine_generation, RequestKind kind, bool committed,
                    const Json &result);
    bool applyRefusal(std::int64_t request_id, std::uint64_t project_generation,
                      std::uint64_t engine_generation, RequestKind kind,
                      const std::string &code, const std::string &message);
    bool applyFailure(std::int64_t request_id, std::uint64_t project_generation,
                      std::uint64_t engine_generation, RequestKind kind,
                      const std::string &message);

    [[nodiscard]] State state() const;
    [[nodiscard]] const Pelican::GraphFilters &draft() const noexcept { return draft_; }
    [[nodiscard]] const Pelican::GraphFilters &saved() const noexcept { return saved_; }
    [[nodiscard]] const std::optional<std::string> &digest() const noexcept { return digest_; }
    [[nodiscard]] const Pelican::GraphFilter *findFilter(const std::string &id) const;
    [[nodiscard]] std::optional<FilterPlace> filterPlace(const std::string &id) const;
    [[nodiscard]] std::size_t undoCount() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t descendantCount(const std::string &id) const;
    [[nodiscard]] std::pair<std::size_t, std::size_t> categoryReferenceCounts(const std::string &id) const;

    // Called only for an accepted current save/undo, including changed:false.
    std::function<void(const Pelican::GraphFilters &)> committed;
    std::function<void()> changed;

  private:
    struct UndoEntry { std::optional<Pelican::GraphFilters> previous; };
    struct ConfirmRemove {
        std::string id;
        Pelican::GraphFilters draft;
    };
    struct UnknownCommit {
        RequestKind kind = RequestKind::save;
        Pelican::GraphFilters document;
        std::optional<Pelican::GraphFilters> previous;
    };

    bool matchingPending(std::int64_t id, std::uint64_t project, std::uint64_t engine,
                         RequestKind kind) const;
    Pelican::GraphFilter *findFilter(const std::string &id);
    bool start(RequestKind kind, Json params);
    bool editable() const;
    void markEdited();
    bool refuse(std::string reason);
    // The strict parser decides every structural edit. A refusal leaves draft_ alone.
    bool replaceDraft(Pelican::GraphFilters candidate, const std::string &what);
    void acceptCommit(Pelican::GraphFilters document, std::string digest,
                      std::optional<Pelican::GraphFilters> previous, bool changed_bytes, bool is_undo);
    void syncState();
    static Pelican::GraphFilters emptyDocument();
    static std::string makeId(const std::string &label, const std::string &fallback,
                              const std::vector<std::string> &used);

    std::uint64_t project_generation_ = 0;
    std::uint64_t engine_generation_ = 0;
    bool project_current_ = false;
    bool engine_ready_ = false;
    bool loaded_ = false;
    Pelican::GraphFilters saved_;
    Pelican::GraphFilters draft_;
    std::optional<std::string> digest_;
    std::vector<UndoEntry> undo_;
    std::optional<Request> pending_;
    std::int64_t next_request_ = 0;
    std::vector<Request> requests_;
    std::optional<UnknownCommit> unknown_;
    std::string state_text_ = "Saved";
    bool external_modification_ = false;
    bool confirm_load_ = false;
    std::optional<ConfirmRemove> confirm_remove_;
};

} // namespace PelicanStudio
