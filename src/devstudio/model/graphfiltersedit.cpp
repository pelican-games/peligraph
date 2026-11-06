#include "graphfiltersedit.hpp"

#include "graphfilters.hpp"

#include <algorithm>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;
using Pelican::GraphFilter;
using Pelican::GraphFilterMatch;
using Pelican::GraphFilters;

Json documentJson(const GraphFilters &document) {
    return Json::parse(Pelican::renderGraphFilters(document));
}

std::optional<GraphFilters> parsedDocument(const Json &value) {
    if (!value.is_object()) return std::nullopt;
    return Pelican::parseGraphFilters(value).document;
}

std::size_t countChildren(const GraphFilter &filter) {
    std::size_t count = filter.children.size();
    for (const auto &child : filter.children) count += countChildren(child);
    return count;
}

void childIds(const std::vector<GraphFilter> &filters, std::vector<std::string> &ids) {
    for (const auto &filter : filters) {
        ids.push_back(filter.id);
        childIds(filter.children, ids);
    }
}

std::size_t countCategoryFilters(const std::vector<GraphFilter> &filters, const std::string &id) {
    std::size_t count = 0;
    for (const auto &filter : filters) {
        const auto &categories = filter.match.feature_categories;
        count += static_cast<std::size_t>(std::ranges::count(categories, id));
        count += countCategoryFilters(filter.children, id);
    }
    return count;
}

void eraseCategory(std::vector<GraphFilter> &filters, const std::string &id) {
    for (auto &filter : filters) {
        auto &categories = filter.match.feature_categories;
        std::erase(categories, id);
        eraseCategory(filter.children, id);
    }
}

// A filter's position as indexes from the top level. Indexes stay meaningful in
// a copy of the document, and across an erase or insert, where pointers don't.
using FilterPath = std::vector<std::size_t>;

bool findPath(const std::vector<GraphFilter> &filters, const std::string &id, FilterPath &path) {
    for (std::size_t index = 0; index < filters.size(); ++index) {
        path.push_back(index);
        if (filters[index].id == id || findPath(filters[index].children, id, path)) return true;
        path.pop_back();
    }
    return false;
}

std::vector<GraphFilter> &siblingsAt(GraphFilters &document, const FilterPath &path) {
    auto *siblings = &document.filters;
    for (std::size_t level = 0; level + 1 < path.size(); ++level)
        siblings = &(*siblings)[path[level]].children;
    return *siblings;
}

GraphFilter &filterAt(GraphFilters &document, const FilterPath &path) {
    return siblingsAt(document, path)[path.back()];
}

bool categoriesDeclared(const GraphFilters &document, const GraphFilterMatch &match) {
    return std::ranges::all_of(match.feature_categories, [&](const std::string &id) {
        return std::ranges::any_of(document.categories, [&](const auto &category) {
            return category.id == id;
        });
    });
}

} // namespace

void GraphFiltersEdit::activateProject(std::uint64_t generation) {
    project_generation_ = generation;
    project_current_ = true;
    pending_.reset();
    external_modification_ = false;
    confirm_load_ = false;
    confirm_remove_.reset();
    state_text_ = loaded_ ? "Saved" : "Not loaded";
    syncState();
}

void GraphFiltersEdit::engineReady(std::uint64_t generation) {
    engine_generation_ = generation;
    engine_ready_ = true;
    syncState();
}

void GraphFiltersEdit::engineGone() {
    if (pending_ && (pending_->kind == RequestKind::save || pending_->kind == RequestKind::undo)) {
        auto parsed = parsedDocument(pending_->params.at("document"));
        if (parsed) {
            UnknownCommit unknown;
            unknown.kind = pending_->kind;
            unknown.document = std::move(*parsed);
            if (unknown.kind == RequestKind::undo && !undo_.empty()) unknown.previous = undo_.back().previous;
            unknown_ = std::move(unknown);
        }
    }
    pending_.reset();
    engine_ready_ = false;
    confirm_load_ = false;
    confirm_remove_.reset();
    state_text_ = unknown_ ? "Connection lost; the save result is unknown. Reload after reconnecting."
                           : "No engine connected";
    syncState();
}

bool GraphFiltersEdit::start(RequestKind kind, Json params) {
    if (!project_current_ || !engine_ready_ || pending_) return false;
    Request request{++next_request_, project_generation_, engine_generation_, kind,
                    std::string{kind == RequestKind::load ? "get_graph_filters" : "edit_graph_filters"},
                    std::move(params)};
    pending_ = request;
    requests_.push_back(std::move(request));
    confirm_load_ = false;
    confirm_remove_.reset();
    external_modification_ = false;
    syncState();
    return true;
}

bool GraphFiltersEdit::requestLoad() {
    return start(RequestKind::load, Json::object());
}

bool GraphFiltersEdit::save() {
    if (!editable() || !(draft_ != saved_)) return false;
    return start(RequestKind::save, {{"base_digest", digest_ ? Json(*digest_) : Json(nullptr)},
                                     {"document", documentJson(draft_)}});
}

bool GraphFiltersEdit::undo() {
    if (!editable() || draft_ != saved_ || undo_.empty()) return false;
    const auto &previous = undo_.back().previous;
    return start(RequestKind::undo, {{"base_digest", digest_ ? Json(*digest_) : Json(nullptr)},
                                     {"document", previous ? documentJson(*previous) : documentJson(emptyDocument())}});
}

bool GraphFiltersEdit::discard() {
    if (!editable()) return false;
    cancelRemoveConfirmation();
    if (draft_ == saved_) return false;
    draft_ = saved_;
    state_text_ = "Saved";
    external_modification_ = false;
    confirm_load_ = false;
    syncState();
    return true;
}

bool GraphFiltersEdit::loadReplacesDirtyDraft() {
    if (!project_current_ || !engine_ready_ || pending_ || !loaded_) return false;
    cancelRemoveConfirmation();
    if (draft_ != saved_ && !confirm_load_) {
        confirm_load_ = true;
        state_text_ = "Load from disk will discard the draft. Press it again to confirm.";
        syncState();
        return false;
    }
    return requestLoad();
}

std::vector<GraphFiltersEdit::Request> GraphFiltersEdit::takeRequests() {
    return std::exchange(requests_, {});
}

bool GraphFiltersEdit::editable() const {
    return project_current_ && engine_ready_ && loaded_ && !pending_;
}

void GraphFiltersEdit::markEdited() {
    external_modification_ = false;
    confirm_load_ = false;
    confirm_remove_.reset();
    state_text_ = "Unsaved changes";
    syncState();
}

bool GraphFiltersEdit::refuse(std::string reason) {
    confirm_remove_.reset();
    state_text_ = std::move(reason);
    syncState();
    return false;
}

bool GraphFiltersEdit::replaceDraft(GraphFilters candidate, const std::string &what) {
    const auto checked = Pelican::parseGraphFilters(documentJson(candidate));
    if (!checked.document) return refuse(what + ": " + checked.error);
    draft_ = std::move(candidate);
    markEdited();
    return true;
}

bool GraphFiltersEdit::cancelRemoveConfirmation() {
    if (!confirm_remove_) return false;
    confirm_remove_.reset();
    state_text_ = draft_ != saved_ ? "Unsaved changes" : "Saved";
    return true;
}

std::string GraphFiltersEdit::makeId(const std::string &label, const std::string &fallback,
                                     const std::vector<std::string> &used) {
    std::string stem;
    bool separator = false;
    for (const unsigned char c : label) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            if (separator && !stem.empty() && stem.back() != '_') stem.push_back('_');
            separator = false;
            stem.push_back(static_cast<char>(c));
        } else if (c >= 'A' && c <= 'Z') {
            if (separator && !stem.empty() && stem.back() != '_') stem.push_back('_');
            separator = false;
            stem.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            separator = true;
        }
    }
    if (stem.empty()) stem = fallback;
    if (stem.front() < 'a' || stem.front() > 'z') stem.insert(0, "f_");
    stem.resize(std::min<std::size_t>(stem.size(), 64));
    auto fits = [&](const std::string &candidate) { return candidate.size() <= 64; };
    std::string candidate = stem;
    for (unsigned index = 2; std::ranges::find(used, candidate) != used.end() || !fits(candidate); ++index) {
        const auto suffix = "_" + std::to_string(index);
        candidate = stem.substr(0, std::min<std::size_t>(stem.size(), 64 - suffix.size())) + suffix;
    }
    return candidate;
}

bool GraphFiltersEdit::addFilter(const std::string &label) {
    if (!editable()) return false;
    std::vector<std::string> ids;
    childIds(draft_.filters, ids);
    draft_.filters.push_back({makeId(label, "filter", ids), label.empty() ? "Filter" : label, {}, {}});
    markEdited();
    return true;
}

bool GraphFiltersEdit::addChildFilter(const std::string &parent_id, const std::string &label) {
    if (!editable()) return false;
    FilterPath path;
    if (!findPath(draft_.filters, parent_id, path)) return false;
    std::vector<std::string> ids;
    childIds(draft_.filters, ids);
    auto candidate = draft_;
    filterAt(candidate, path).children.push_back(
        {makeId(label, "filter", ids), label.empty() ? "Filter" : label, {}, {}});
    return replaceDraft(std::move(candidate), "Can't add a child to \"" + filterAt(draft_, path).label + "\"");
}

GraphFilter *GraphFiltersEdit::findFilter(const std::string &id) {
    FilterPath path;
    return findPath(draft_.filters, id, path) ? &filterAt(draft_, path) : nullptr;
}

bool GraphFiltersEdit::removeFilter(const std::string &id) {
    if (!editable()) return false;
    FilterPath path;
    if (!findPath(draft_.filters, id, path)) return false;
    const auto &filter = filterAt(draft_, path);
    if (!filter.children.empty() &&
        !(confirm_remove_ && confirm_remove_->id == id && confirm_remove_->draft == draft_)) {
        confirm_remove_ = ConfirmRemove{id, draft_};
        confirm_load_ = false;
        state_text_ = "Remove \"" + filter.label + "\" and " + std::to_string(countChildren(filter)) +
                      " more under it? Press Remove again.";
        syncState();
        return false;
    }
    const auto what = "Can't remove \"" + filter.label + "\"";
    auto candidate = draft_;
    auto &siblings = siblingsAt(candidate, path);
    siblings.erase(siblings.begin() + static_cast<std::ptrdiff_t>(path.back()));
    return replaceDraft(std::move(candidate), what);
}

bool GraphFiltersEdit::moveFilter(const std::string &id, int delta) {
    if (!editable()) return false;
    FilterPath path;
    if (!findPath(draft_.filters, id, path)) return false;
    const auto destination = static_cast<std::ptrdiff_t>(path.back()) + delta;
    auto candidate = draft_;
    auto &siblings = siblingsAt(candidate, path);
    if (destination < 0 || destination >= static_cast<std::ptrdiff_t>(siblings.size())) return false;
    std::swap(siblings[path.back()], siblings[static_cast<std::size_t>(destination)]);
    return replaceDraft(std::move(candidate), "Can't move \"" + filterAt(draft_, path).label + "\"");
}

bool GraphFiltersEdit::indentFilter(const std::string &id) {
    if (!editable()) return false;
    FilterPath path;
    if (!findPath(draft_.filters, id, path) || path.back() == 0) return false;
    auto candidate = draft_;
    auto &siblings = siblingsAt(candidate, path);
    auto moved = siblings[path.back()];
    siblings.erase(siblings.begin() + static_cast<std::ptrdiff_t>(path.back()));
    siblings[path.back() - 1].children.push_back(std::move(moved));
    return replaceDraft(std::move(candidate), "Can't move \"" + filterAt(draft_, path).label + "\" in");
}

bool GraphFiltersEdit::outdentFilter(const std::string &id) {
    if (!editable()) return false;
    FilterPath path;
    if (!findPath(draft_.filters, id, path) || path.size() < 2) return false;
    auto candidate = draft_;
    auto &siblings = siblingsAt(candidate, path);
    auto moved = siblings[path.back()];
    siblings.erase(siblings.begin() + static_cast<std::ptrdiff_t>(path.back()));
    FilterPath parent = path;
    parent.pop_back();
    auto &outer = siblingsAt(candidate, parent);
    outer.insert(outer.begin() + static_cast<std::ptrdiff_t>(parent.back()) + 1, std::move(moved));
    return replaceDraft(std::move(candidate), "Can't move \"" + filterAt(draft_, path).label + "\" out");
}

bool GraphFiltersEdit::setFilterId(const std::string &id, const std::string &new_id) {
    if (!editable() || id == new_id) return false;
    FilterPath path;
    if (!findPath(draft_.filters, id, path)) return false;
    auto candidate = draft_;
    filterAt(candidate, path).id = new_id;
    return replaceDraft(std::move(candidate), "Can't rename \"" + id + "\"");
}

bool GraphFiltersEdit::setLabel(const std::string &id, const std::string &label) {
    auto *filter = editable() ? findFilter(id) : nullptr;
    if (!filter || label.empty() || filter->label == label) return false;
    filter->label = label;
    markEdited();
    return true;
}

bool GraphFiltersEdit::setMatch(const std::string &id, const GraphFilterMatch &match) {
    auto *filter = editable() ? findFilter(id) : nullptr;
    if (!filter || !categoriesDeclared(draft_, match) || filter->match == match) return false;
    filter->match = match;
    markEdited();
    return true;
}

bool GraphFiltersEdit::addCategory(const std::string &label) {
    if (!editable()) return false;
    std::vector<std::string> ids;
    for (const auto &category : draft_.categories) ids.push_back(category.id);
    draft_.categories.push_back({makeId(label, "category", ids), label.empty() ? "Category" : label});
    markEdited();
    return true;
}

bool GraphFiltersEdit::removeCategory(const std::string &id) {
    if (!editable()) return false;
    const auto found = std::ranges::find_if(draft_.categories, [&](const auto &category) { return category.id == id; });
    if (found == draft_.categories.end()) return false;
    draft_.categories.erase(found);
    for (auto &[feature, categories] : draft_.feature_categories) std::erase(categories, id);
    std::erase_if(draft_.feature_categories, [](const auto &entry) { return entry.second.empty(); });
    eraseCategory(draft_.filters, id);
    markEdited();
    return true;
}

bool GraphFiltersEdit::setCategoryLabel(const std::string &id, const std::string &label) {
    auto found = editable() ? std::ranges::find_if(draft_.categories, [&](const auto &category) {
                                  return category.id == id;
                              }) : draft_.categories.end();
    if (found == draft_.categories.end() || label.empty() || found->label == label) return false;
    found->label = label;
    markEdited();
    return true;
}

bool GraphFiltersEdit::setFeatureCategories(const std::string &feature, const std::vector<std::string> &ids) {
    if (!editable() || feature.empty()) return false;
    if (!std::ranges::all_of(ids, [&](const std::string &id) {
            return std::ranges::any_of(draft_.categories, [&](const auto &category) { return category.id == id; });
        })) return false;
    auto found = std::ranges::find_if(draft_.feature_categories, [&](const auto &entry) {
        return entry.first == feature;
    });
    if (ids.empty()) {
        if (found == draft_.feature_categories.end()) return false;
        draft_.feature_categories.erase(found);
    } else if (found == draft_.feature_categories.end()) {
        draft_.feature_categories.emplace_back(feature, ids);
    } else {
        if (found->second == ids) return false;
        found->second = ids;
    }
    markEdited();
    return true;
}

bool GraphFiltersEdit::matchingPending(std::int64_t id, std::uint64_t project,
                                       std::uint64_t engine, RequestKind kind) const {
    return project_current_ && pending_ && pending_->id == id &&
           pending_->project_generation == project && pending_->engine_generation == engine &&
           pending_->kind == kind && project == project_generation_ && engine == engine_generation_;
}

void GraphFiltersEdit::acceptCommit(GraphFilters document, std::string digest,
                                    std::optional<GraphFilters> previous, bool changed_bytes,
                                    bool is_undo) {
    if (is_undo) {
        if (changed_bytes && !undo_.empty()) undo_.pop_back();
    } else if (changed_bytes) {
        undo_.push_back({std::move(previous)});
    }
    saved_ = std::move(document);
    draft_ = saved_;
    digest_ = std::move(digest);
    loaded_ = true;
    external_modification_ = false;
    confirm_load_ = false;
    state_text_ = "Saved";
    const auto accepted = saved_;
    if (committed) committed(accepted);
}

bool GraphFiltersEdit::applyReply(std::int64_t request_id, std::uint64_t project_generation,
                                  std::uint64_t engine_generation, RequestKind kind,
                                  bool committed_reply, const Json &result) {
    if (!matchingPending(request_id, project_generation, engine_generation, kind)) return false;
    pending_.reset();
    if (kind == RequestKind::load) {
        const auto document = result.find("document");
        const auto error = result.find("error");
        const bool has_error = error != result.end() && error->is_string() && !error->get<std::string>().empty();
        const auto parsed = document != result.end() ? parsedDocument(*document) : std::nullopt;
        if (unknown_) {
            const auto unknown = std::exchange(unknown_, std::nullopt);
            if (parsed && *parsed == unknown->document && result.value("digest", Json()).is_string()) {
                acceptCommit(*parsed, result.at("digest").get<std::string>(), unknown->previous,
                             true, unknown->kind == RequestKind::undo);
            } else {
                state_text_ = "The reconnect read differs from the edit whose result was lost. Keep or discard the draft.";
                external_modification_ = true;
            }
        } else if (!has_error && parsed && result.value("digest", Json()).is_string()) {
            saved_ = *parsed;
            digest_ = result.at("digest").get<std::string>();
            draft_ = saved_;
            loaded_ = true;
            undo_.clear();
            external_modification_ = false;
            state_text_ = "Saved";
        } else if (!has_error && document != result.end() && document->is_null() &&
                   result.value("digest", Json()).is_null()) {
            saved_ = emptyDocument();
            draft_ = saved_;
            digest_.reset();
            loaded_ = true;
            undo_.clear();
            external_modification_ = false;
            state_text_ = "Saved";
        } else {
            state_text_ = has_error ? error->get<std::string>() : "graph_filters.json is not a valid document";
            if (loaded_) external_modification_ = true;
        }
        confirm_load_ = false;
        syncState();
        return true;
    }
    const auto digest = result.find("digest");
    const auto changed_bytes = result.find("changed");
    const auto parsed = parsedDocument(result.value("document", Json()));
    if (!committed_reply || !parsed || digest == result.end() || !digest->is_string() || digest->empty() ||
        changed_bytes == result.end() || !changed_bytes->is_boolean()) {
        state_text_ = "The engine returned an invalid graph filter reply; the draft was kept.";
        syncState();
        return true;
    }
    std::optional<GraphFilters> previous;
    if (const auto found = result.find("previous"); found != result.end() && !found->is_null()) {
        previous = parsedDocument(*found);
        if (!previous) {
            state_text_ = "The engine returned invalid Undo history; the draft was kept.";
            syncState();
            return true;
        }
    }
    acceptCommit(*parsed, digest->get<std::string>(), std::move(previous), changed_bytes->get<bool>(),
                 kind == RequestKind::undo);
    unknown_.reset();
    syncState();
    return true;
}

bool GraphFiltersEdit::applyRefusal(std::int64_t request_id, std::uint64_t project_generation,
                                    std::uint64_t engine_generation, RequestKind kind,
                                    const std::string &code, const std::string &message) {
    if (!matchingPending(request_id, project_generation, engine_generation, kind)) return false;
    pending_.reset();
    unknown_.reset();
    external_modification_ = code == "external_modification";
    state_text_ = external_modification_
                      ? "graph_filters.json changed on disk. Load from disk replaces the saved baseline."
                      : (message.empty() ? code : message);
    syncState();
    return true;
}

bool GraphFiltersEdit::applyFailure(std::int64_t request_id, std::uint64_t project_generation,
                                    std::uint64_t engine_generation, RequestKind kind,
                                    const std::string &message) {
    if (!matchingPending(request_id, project_generation, engine_generation, kind)) return false;
    pending_.reset();
    state_text_ = message.empty() ? "The engine request failed; the draft was kept." : message;
    syncState();
    return true;
}

GraphFiltersEdit::State GraphFiltersEdit::state() const {
    State result;
    result.loaded = loaded_;
    result.engine_ready = project_current_ && engine_ready_;
    result.dirty = draft_ != saved_;
    result.editing_enabled = editable();
    result.save_enabled = editable() && result.dirty;
    result.undo_enabled = editable() && !result.dirty && !undo_.empty();
    result.load_enabled = project_current_ && engine_ready_ && loaded_ && !pending_;
    result.discard_enabled = editable() && result.dirty;
    result.external_modification = external_modification_;
    result.commit_result_unknown = unknown_.has_value();
    result.confirm_load = confirm_load_;
    result.confirm_remove = confirm_remove_.has_value();
    if (pending_) {
        result.text = pending_->kind == RequestKind::load ? "Loading..." :
                          pending_->kind == RequestKind::save ? "Saving..." : "Undoing...";
    } else if (!engine_ready_) {
        result.text = unknown_ ? "Connection lost; the save result is unknown." : "No engine connected";
    } else if (result.dirty && state_text_ == "Saved") {
        result.text = "Unsaved changes";
    } else {
        result.text = state_text_;
    }
    return result;
}

bool GraphFiltersEdit::importFilters(const GraphFilters &incoming) {
    if (!editable()) return false;
    confirm_remove_.reset();
    auto merged = mergeGraphFilters(draft_, incoming);
    const auto validated = Pelican::parseGraphFilters(Json::parse(Pelican::renderGraphFilters(merged.merged)));
    if (!validated.document) {
        state_text_ = "Import refused: " + validated.error;
        syncState();
        return false;
    }
    const auto imported = merged.merged.filters.size() - draft_.filters.size();
    draft_ = std::move(merged.merged);
    std::vector<std::string> skipped = std::move(merged.skipped_filters);
    skipped.insert(skipped.end(), merged.skipped_categories.begin(), merged.skipped_categories.end());
    state_text_ = "Imported " + std::to_string(imported) + " filters, skipped " + std::to_string(skipped.size());
    if (!skipped.empty()) {
        state_text_ += ": ";
        for (std::size_t i = 0; i < skipped.size(); ++i) {
            if (i != 0) state_text_ += ", ";
            state_text_ += skipped[i];
        }
    }
    external_modification_ = false;
    confirm_load_ = false;
    syncState();
    return true;
}

const GraphFilter *GraphFiltersEdit::findFilter(const std::string &id) const {
    return findGraphFilter(draft_, id);
}

std::optional<GraphFiltersEdit::FilterPlace> GraphFiltersEdit::filterPlace(const std::string &id) const {
    FilterPath path;
    if (!findPath(draft_.filters, id, path)) return std::nullopt;
    const auto *siblings = &draft_.filters;
    std::string parent;
    for (std::size_t level = 0; level + 1 < path.size(); ++level) {
        parent = (*siblings)[path[level]].id;
        siblings = &(*siblings)[path[level]].children;
    }
    return FilterPlace{static_cast<int>(path.size()), path.back(), siblings->size(), std::move(parent)};
}

std::size_t GraphFiltersEdit::descendantCount(const std::string &id) const {
    const auto *filter = findFilter(id);
    return filter ? countChildren(*filter) : 0;
}

std::pair<std::size_t, std::size_t> GraphFiltersEdit::categoryReferenceCounts(const std::string &id) const {
    std::size_t features = 0;
    for (const auto &[feature, categories] : draft_.feature_categories) {
        if (std::ranges::find(categories, id) != categories.end()) ++features;
    }
    return {features, countCategoryFilters(draft_.filters, id)};
}

void GraphFiltersEdit::syncState() {
    if (changed) changed();
}

GraphFilters GraphFiltersEdit::emptyDocument() { return {}; }

} // namespace PelicanStudio
