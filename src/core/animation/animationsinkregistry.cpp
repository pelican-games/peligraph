#include "animationsinkregistry.hpp"

#include "animationserviceabi.hpp"
#include "../container.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

#include <algorithm>

namespace Pelican::Animation::Internal {

SinkRegistry::SinkRecord *SinkRegistry::findSink(AnimationSinkHandle sink) {
    if (!isValid(sink)) return nullptr;
    const auto found = sinks_.find(sink.identity);
    return found != sinks_.end() && sameHandle(found->second.handle, sink) ? &found->second : nullptr;
}

SinkRegistry::SourceRecord *SinkRegistry::findSource(
    AnimationSourceHandle source) {
    if (!isValid(source)) return nullptr;
    const auto found = sources_.find(source.identity);
    if (found == sources_.end()) return nullptr;
    return found->second.active && sameHandle(found->second.handle, source) ? &found->second : nullptr;
}

Status SinkRegistry::resolveSinkByInstance(InstanceHandle instance,
                                           SinkRecord *&out) {
    out = nullptr;
    if (!isValid(instance)) return Status::invalid_handle;
    const auto found = instances_.find(instance.identity);
    if (found == instances_.end()) return Status::invalid_handle;
    auto *sink = findSink(sinks_.at(found->second).handle);
    if (!sink || !sameHandle(sink->instance, instance))
        return Status::stale_generation;
    out = sink;
    return Status::ok;
}

Status SinkRegistry::sinkForObject(ObjectRecord &object,
                                   AnimationSinkKind kind,
                                   std::uint64_t &next_identity,
                                   AnimationSinkHandle &out) {
    for (const auto &[_, sink] : sinks_) {
        if (sink.object == &object && sink.kind == kind) {
            out = sink.handle;
            return Status::ok;
        }
    }
    SinkRecord sink;
    sink.handle = {next_identity++, 1, 0};
    sink.object = &object;
    sink.kind = kind;
    sink.instance = object.renderer_instance
                        ? GET_MODULE(PolygonInstanceContainer).animationInstance(*object.renderer_instance)
                        : InstanceHandle{next_identity++, 1, 0};
    instances_.emplace(sink.instance.identity, sink.handle.identity);
    out = sink.handle;
    sinks_.emplace(sink.handle.identity, sink);
    return Status::ok;
}

std::vector<AnimationSinkHandle> SinkRegistry::sinkHandles() const {
    std::vector<AnimationSinkHandle> handles;
    handles.reserve(sinks_.size());
    for (const auto &[_, sink] : sinks_) handles.push_back(sink.handle);
    return handles;
}

Status SinkRegistry::claimSource(AnimationOwnerHandle owner,
                                 AnimationSinkHandle sink_handle,
                                 std::uint32_t ordinal,
                                 AnimationSourceAuthorityV1 authority,
                                 std::uint64_t &next_identity,
                                 AnimationSourceHandle &out) {
    auto *sink = findSink(sink_handle);
    if (!sink) return Status::invalid_handle;
    if (authority == AnimationSourceAuthorityV1::graph_apply &&
        findSource(sink->active_source))
        return Status::authority_conflict;
    SourceRecord source;
    source.handle = {next_identity++, owner.generation, 0};
    source.owner = owner;
    source.sink = sink_handle;
    source.ordinal = ordinal;
    source.authority = authority;
    if (source.authority == AnimationSourceAuthorityV1::graph_apply)
        sink->active_source = source.handle;
    out = source.handle;
    sources_.emplace(source.handle.identity, source);
    return Status::ok;
}

Status SinkRegistry::releaseSource(AnimationOwnerHandle owner,
                                   AnimationSourceHandle source_handle) {
    auto *source = findSource(source_handle);
    if (!source) return Status::stale_generation;
    if (!sameHandle(source->owner, owner)) return Status::authority_conflict;
    auto *sink = findSink(source->sink);
    if (!sink) return Status::authority_conflict;
    if (source->authority == AnimationSourceAuthorityV1::graph_apply) {
        if (!sameHandle(sink->active_source, source->handle))
            return Status::authority_conflict;
        sink->active_source = invalidHandle<AnimationSourceHandle>();
    }
    source->active = false;
    if (++source->handle.generation == 0) ++source->handle.generation;
    return Status::ok;
}

AnimationSourceHandle SinkRegistry::transferSource(
    SourceRecord &current, SinkRecord &sink, AnimationOwnerHandle next_owner,
    std::uint32_t next_ordinal, std::uint64_t &next_identity) {
    SourceRecord next;
    next.handle = {next_identity++, next_owner.generation, 0};
    next.owner = next_owner;
    next.sink = current.sink;
    next.ordinal = next_ordinal;
    next.authority = AnimationSourceAuthorityV1::graph_apply;
    current.active = false;
    if (++current.handle.generation == 0) ++current.handle.generation;
    sink.active_source = next.handle;
    sources_.emplace(next.handle.identity, next);
    return next.handle;
}

Status SinkRegistry::notify(const AnimationNotificationDescV1 &desc) {
    auto *source = findSource(desc.source);
    auto *sink = findSink(desc.sink);
    if (!source || source->authority !=
                       AnimationSourceAuthorityV1::graph_apply ||
        !sink || !sameHandle(source->sink, sink->handle) ||
        !sameHandle(sink->active_source, source->handle))
        return Status::authority_conflict;
    if (desc.notification_revision <= sink->last_notification_revision)
        return Status::duplicate_revision;
    if (!sink->object) return Status::stale_generation;
    if (desc.kind == AnimationNotificationKind::layout_generation_mismatch &&
        sink->object->asset &&
        sameHandle(desc.observed_layout, sink->object->asset->rig.layout))
        return Status::invalid_argument;
    sink->last_notification_revision = desc.notification_revision;
    sink->reset_history = true;
    return Status::ok;
}

void SinkRegistry::blockCommit(InstanceHandle instance,
                               std::uint64_t revision) {
    blocked_commits_.push_back(
        {instance.identity, instance.generation, revision});
}

bool SinkRegistry::commitBlocked(InstanceHandle instance,
                                 std::uint64_t revision) const {
    for (const auto &blocked : blocked_commits_) {
        if (blocked.instance_identity == instance.identity &&
            blocked.instance_generation == instance.generation &&
            blocked.revision == revision)
            return true;
    }
    return false;
}

void SinkRegistry::retireSourcesForOwner(std::uint64_t owner_identity) {
    for (auto &[_, source] : sources_) {
        if (!source.active || source.owner.identity != owner_identity) continue;
        if (auto *sink = findSink(source.sink); sink && sameHandle(sink->active_source, source.handle))
            sink->active_source = invalidHandle<AnimationSourceHandle>();
        source.active = false;
        if (++source.handle.generation == 0) ++source.handle.generation;
    }
}

void SinkRegistry::compactRetiredSources() {
    std::erase_if(sources_, [](const auto &entry) { return !entry.second.active; });
}

void SinkRegistry::retireObject(const ObjectRecord *object) {
    for (auto it = sinks_.begin(); it != sinks_.end();) {
        if (it->second.object != object) { ++it; continue; }
        const auto sink = it->second.handle;
        const auto instance = it->second.instance;
        std::erase_if(sources_, [&](const auto &entry) {
            return sameHandle(entry.second.sink, sink);
        });
        std::erase_if(instances_, [&](const auto &entry) { return entry.second == sink.identity; });
        std::erase_if(blocked_commits_, [&](const auto &entry) {
            return entry.instance_identity == instance.identity;
        });
        it = sinks_.erase(it);
    }
    if (staged_frame_ && staged_frame_->object == object) staged_frame_.reset();
}

void SinkRegistry::rebindInstancesForObjects(
    const std::unordered_set<ObjectRecord *> &affected) {
    std::unordered_set<std::uint64_t> affected_instances;
    for (auto &[_, sink] : sinks_) {
        if (!affected.contains(sink.object)) continue;
        affected_instances.insert(sink.instance.identity);
        if (sink.object->renderer_instance) {
            if (auto *container =
                    FastModuleContainer::tryGet<PolygonInstanceContainer>()) {
                sink.instance = container->animationInstance(
                    *sink.object->renderer_instance);
            } else if (++sink.instance.generation == 0) {
                ++sink.instance.generation;
            }
        } else if (++sink.instance.generation == 0) {
            ++sink.instance.generation;
        }
        instances_[sink.instance.identity] = sink.handle.identity;
        sink.reset_history = true;
    }
    std::erase_if(blocked_commits_, [&](const auto &blocked) {
        return affected_instances.contains(blocked.instance_identity);
    });
    if (staged_frame_ && affected.contains(staged_frame_->object))
        staged_frame_.reset();
}

void SinkRegistry::clearForReset() {
    sources_.clear();
    blocked_commits_.clear();
    staged_frame_.reset();
    sinks_.clear();
    instances_.clear();
}

} // namespace Pelican::Animation::Internal
