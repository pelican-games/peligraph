#include "outputsinks.hpp"

#include "../launchconfig.hpp"
#include "../log.hpp"
#include "../appflow/enginetime.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/renderer.hpp"
#include "../watch/problemlist.hpp"
#include "outputslotqueue.hpp"
#include "outputvideo.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <thread>

namespace Pelican {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::string_view outputProblemDomain = "output";

std::string sinkError(const OutputSinkSpec &sink) {
    if (sink.error_code.empty()) return {};
    if (sink.error_message.empty()) return sink.error_code;
    return sink.error_code + ": " + sink.error_message;
}
bool sameIdentity(const OutputSinks::State &state, const OutputSinkSpec &sink) {
    return state.name == sink.name && state.kind == sink.kind &&
           state.graph == sink.graph && state.target == sink.target;
}
bool rateMatches(double rate, std::uint64_t num, std::uint64_t den) {
    if (rate <= 0.0 || !std::isfinite(rate)) return false;
    std::uint64_t previous_num = 0, current_num = 1;
    std::uint64_t previous_den = 1, current_den = 0;
    double remainder = rate;
    for (int step = 0; step < 32; ++step) {
        const auto coefficient = static_cast<std::uint64_t>(std::floor(remainder));
        const auto next_num = coefficient * current_num + previous_num;
        const auto next_den = coefficient * current_den + previous_den;
        if (next_den == 0 || next_den > (std::numeric_limits<std::uint32_t>::max)())
            return false;
        previous_num = current_num; current_num = next_num;
        previous_den = current_den; current_den = next_den;
        if (current_num * den == current_den * num) return true;
        const auto fraction = remainder - std::floor(remainder);
        if (fraction <= 0.0) break;
        remainder = 1.0 / fraction;
    }
    return false;
}

std::string problemCode(std::string_view error, std::string_view fallback) {
    const auto colon = error.find(':');
    auto code = error.substr(0, colon);
    while (!code.empty() && (code.back() == ' ' || code.back() == '\t'))
        code.remove_suffix(1);
    if (code.empty() || !code.starts_with("output."))
        return std::string{fallback};
    return std::string{code};
}
bool replaceOutputProblems(const std::vector<nlohmann::json> &problems) {
    auto *list = FastModuleContainer::tryGet<ProblemList>();
    if (list == nullptr) return false;
    list->replace(outputProblemDomain, problems);
    return true;
}
std::vector<OutputSinks::State> statesToValues(
    const std::vector<std::shared_ptr<OutputSinks::State>> &states) {
    std::vector<OutputSinks::State> values;
    values.reserve(states.size());
    for (const auto &state : states) values.push_back(*state);
    return values;
}
} // namespace

std::vector<nlohmann::json> outputSinkProblems(
    const std::vector<OutputSinks::State> &states) {
    std::vector<nlohmann::json> problems;
    for (const auto &state : states) {
        std::string code;
        if (state.state == "refused") {
            code = problemCode(state.last_error, "output.write_failed@1");
        } else if (state.state == "finished" && state.stop_reason != "none") {
            code = "output.limit_reached@1";
        } else {
            continue;
        }
        nlohmann::json fields{{"graph", state.graph}, {"sink", state.name}};
        if (state.state == "finished" && state.stop_reason != "none") {
            fields["limit"] = state.stop_reason;
            fields["value"] = state.stop_value;
        }
        auto message = state.last_error;
        if (message.empty()) {
            message = code;
            if (state.state == "finished") {
                message += ": " + state.stop_reason + " " +
                           std::to_string(state.stop_value) + " reached";
            }
        }
        problems.push_back({{"severity", "error"},
                            {"source", "runtime"}, {"code", code},
                            {"message", message}, {"scope", state.graph},
                            {"node", state.name}, {"fields", std::move(fields)}});
    }
    return problems;
}

struct OutputSinks::Impl {
    struct Pool;
    struct Runtime {
        OutputSinkSpec spec;
        std::shared_ptr<State> state;
        std::shared_ptr<Pool> pool;
        std::shared_ptr<OutputVideoWriter> video;
        std::uint64_t generation = 0;
    };
    struct SlotResource {
        BufferWrapper buffer;
        vk::UniqueCommandPool command_pool;
        vk::UniqueCommandBuffer command_buffer;
        vk::UniqueFence fence;
        std::shared_ptr<const void> source_lease;
        vk::DeviceSize bytes = 0;
        bool coherent = true;
    };
    struct Pool {
        OutputSlotQueue queue;
        std::array<std::unique_ptr<SlotResource>, output_slot_count> resources;
        vk::Extent2D extent{};
        vk::Format format = vk::Format::eUndefined;
        bool retired = false;
    };
    struct Work {
        std::shared_ptr<Runtime> runtime;
        std::shared_ptr<Pool> pool;
        std::size_t slot = 0;
        std::uint64_t frame = 0;
        std::uint64_t generation = 0;
        vk::Extent2D extent{};
        vk::Format format = vk::Format::eUndefined;
        OutputFileEncoding encoding = OutputFileEncoding::raw;
        bool discard = false;
        bool wait_for_fence = false;
        bool submitted = false;
        std::shared_ptr<OutputVideoFrame> video_frame;
    };

    struct CaptureTiming {
        std::uint64_t frame = 0;
        double selection_ms = 0.0;
        double record_submit_ms = 0.0;
        double total_ms = 0.0;
        std::uint64_t queue_depth = 0;
    };

    mutable std::mutex mutex;
    mutable std::condition_variable work_cv;
    std::condition_variable free_cv;
    std::condition_variable pause_cv;
    std::shared_ptr<const OutputSinkTable> table;
    std::vector<std::shared_ptr<Runtime>> current;
    std::vector<std::shared_ptr<Pool>> retired;
    std::vector<std::shared_ptr<OutputVideoWriter>> retired_videos;
    std::deque<Work> work;
    std::thread worker;
    bool active = false;
    bool stopping = false;
    bool device_lost = false;
    bool worker_started = false;
    bool worker_busy = false;
    bool paused = false;
    bool pause_acknowledged = false;
    bool drain_expired = false;
    std::uint64_t generation = 0;
    std::uint64_t problems_revision = 0;
    std::uint64_t published_revision = 0;
    std::uint64_t bytes_written = 0;
    std::uint32_t max_frames = 1000;
    std::uint64_t max_bytes = 0;
    std::uint64_t frames_skipped_inactive = 0;
    std::uint64_t frames_skipped_no_contents = 0;
    std::filesystem::path directory;
    std::optional<Clock::time_point> resume_at;
    std::vector<CaptureTiming> capture_timings;
    std::uint64_t maximum_queue_depth = 0;
    std::optional<std::filesystem::path> ffmpeg;
    bool headless = false;
    double fps = 60.0;

    void syncCounters(const std::shared_ptr<Runtime> &runtime) {
        if (runtime->video) {
            const auto video = runtime->video->status();
            runtime->state->state = video.state;
            runtime->state->stop_reason = video.stop_reason;
            runtime->state->stop_value = video.stop_value;
            runtime->state->last_error = video.last_error;
            runtime->state->frames_written = video.frames_written;
            runtime->state->frames_repeated = video.frames_repeated;
            runtime->state->frames_dropped = video.frames_dropped;
            runtime->state->frames_discarded = video.frames_discarded;
            runtime->state->queue_depth = video.queue_depth;
            runtime->state->encoder_pid = video.encoder_pid;
            runtime->state->encoder_state = video.encoder_state;
            runtime->state->final_bytes = video.final_bytes;
            return;
        }
        runtime->state->frames_dropped = runtime->pool->queue.framesDropped();
        runtime->state->frames_discarded = runtime->pool->queue.framesDiscarded();
        runtime->state->queue_depth = runtime->pool->queue.queueDepth();
    }
    void noteProblem(const Runtime &runtime) {
        ++problems_revision;
        if (runtime.generation == generation) published_revision = 0;
    }
    void ensureWorker() {
        if (worker_started) return;
        stopping = false;
        device_lost = false;
        worker_started = true;
        worker = std::thread([this] { run(); });
    }
    void run();
    void process(Work entry);
    void discardUnstarted();
    void finishSlot(Work &entry, bool written, std::uint64_t written_bytes,
                    std::string error, bool limit, bool device_error);
    static void releaseResource(SlotResource &resource);
};

OutputSinks::OutputSinks() : impl_(std::make_unique<Impl>()) {
    // Reverse-order module teardown must join the worker and release slot
    // resources before the device and allocator are destroyed.
    (void)GET_MODULE(VulkanManageCore);
    if (const auto *config = FastModuleContainer::tryGet<EngineLaunchConfig>();
        config != nullptr) {
        impl_->max_frames = config->output_max_frames;
        impl_->max_bytes = config->output_max_bytes;
        impl_->ffmpeg = config->ffmpeg;
        impl_->headless = config->headless;
        impl_->fps = config->fps;
        if (config->output_dir) {
            impl_->active = true;
            impl_->directory = *config->output_dir;
        }
    }
}

OutputSinks::~OutputSinks() {
    {
        std::lock_guard lock{impl_->mutex};
        impl_->stopping = true;
        impl_->paused = false;
        impl_->pause_acknowledged = false;
        impl_->resume_at.reset();
        impl_->drain_expired = false;
    }
    impl_->work_cv.notify_all();
    impl_->free_cv.notify_all();
    impl_->pause_cv.notify_all();
    if (impl_->worker_started) {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        std::unique_lock lock{impl_->mutex};
        impl_->work_cv.wait_until(lock, deadline, [&] {
            return impl_->work.empty();
        });
        if (!impl_->work.empty()) {
            impl_->drain_expired = true;
            const auto count = impl_->work.size();
            LOG_WARNING(logger, "Output sink teardown discarded {} queued frames", count);
            lock.unlock();
            impl_->work_cv.notify_all();
            impl_->free_cv.notify_all();
        }
    }
    if (impl_->worker.joinable()) impl_->worker.join();
    std::vector<std::shared_ptr<OutputVideoWriter>> videos;
    {
        std::lock_guard lock{impl_->mutex};
        for (const auto &runtime : impl_->current)
            if (runtime->video) videos.push_back(runtime->video);
        videos.insert(videos.end(), impl_->retired_videos.begin(),
                      impl_->retired_videos.end());
    }
    for (const auto &video : videos) video->requestStop();
}

void OutputSinks::install(std::shared_ptr<const OutputSinkTable> next) {
    std::vector<nlohmann::json> problems;
    std::uint64_t installed_generation = 0;
    {
        std::lock_guard lock{impl_->mutex};
        ++impl_->generation;
        installed_generation = impl_->generation;
        for (auto &entry : impl_->work) {
            if (entry.generation == impl_->generation || entry.video_frame) continue;
            const auto *queued = entry.pool->queue.entry(entry.slot);
            if (queued == nullptr ||
                entry.pool->queue.state(entry.slot) != OutputSlotState::sending) {
                entry.discard = true;
                if (queued != nullptr)
                    const_cast<OutputSlotEntry *>(queued)->discard = true;
            }
        }
        auto previous = std::move(impl_->current);
        for (auto &old : previous) old->pool->retired = true;
        impl_->current.clear();
        impl_->table = std::move(next);
        if (impl_->table) {
            for (const auto &sink : impl_->table->sinks) {
                auto runtime = std::make_shared<Impl::Runtime>();
                runtime->spec = sink;
                runtime->generation = impl_->generation;
                auto state = std::make_shared<State>();
                state->name = sink.name;
                state->kind = sink.kind;
                state->graph = sink.graph;
                state->target = sink.target;
                state->format = sink.format;
                state->when_full = sink.when_full;
                state->mode = sink.mode;
                runtime->state = state;
                runtime->pool = std::make_shared<Impl::Pool>();
                for (const auto &old : previous) {
                    if (!sameIdentity(*old->state, sink)) continue;
                    if (sink.kind == "video" && old->spec != sink) continue;
                    runtime->state = old->state;
                    runtime->pool = old->pool;
                    runtime->pool->retired = false;
                    runtime->video = old->video;
                    state = old->state;
                    state->format = sink.format;
                    state->when_full = sink.when_full;
                    state->mode = sink.mode;
                    state->stop_reason = "none";
                    state->stop_value = 0;
                    state->last_error.clear();
                    break;
                }
                if (sink.kind == "video" && impl_->active && !runtime->video) {
                    runtime->video = std::make_shared<OutputVideoWriter>(sink.name,
                        impl_->directory, impl_->ffmpeg, sink.fps_num, sink.fps_den,
                        sink.mode == "live",
                        impl_->headless && rateMatches(impl_->fps, sink.fps_num,
                                                       sink.fps_den),
                        impl_->max_frames, impl_->max_bytes);
                }
                impl_->current.push_back(runtime);
            }
        }
        for (const auto &old : previous) {
            if (!old->video) continue;
            const auto reused = std::any_of(impl_->current.begin(), impl_->current.end(),
                [&](const auto &runtime) { return runtime->video == old->video; });
            if (!reused) {
                old->video->requestStop();
                impl_->retired_videos.push_back(old->video);
            }
        }
        std::erase_if(impl_->retired_videos,
                      [](const auto &video) { return video->finished(); });
        std::vector<std::shared_ptr<State>> states;
        for (auto &runtime : impl_->current) {
            auto &state = *runtime->state;
            if (!impl_->active) {
                state.state = "inactive";
            } else if (!runtime->spec.error_code.empty()) {
                state.state = "refused";
                state.last_error = sinkError(runtime->spec);
            } else if (impl_->max_bytes != 0 &&
                       impl_->bytes_written >= impl_->max_bytes) {
                state.state = "finished";
                state.stop_reason = "max_bytes";
                state.stop_value = impl_->max_bytes;
                state.last_error = "output.limit_reached@1: max_bytes " +
                    std::to_string(impl_->max_bytes) + " reached";
            } else if (impl_->max_frames != 0 &&
                       state.frames_written >= impl_->max_frames &&
                       std::none_of(runtime->pool->queue.entries().begin(),
                           runtime->pool->queue.entries().end(),
                           [](const OutputSlotEntry &queued) {
                               return !queued.discard;
                           })) {
                state.state = "finished";
                state.stop_reason = "max_frames";
                state.stop_value = impl_->max_frames;
                state.last_error = "output.limit_reached@1: max_frames " +
                    std::to_string(impl_->max_frames) + " reached";
            } else {
                state.state = "running";
            }
            states.push_back(runtime->state);
        }
        problems = outputSinkProblems(statesToValues(states));
        impl_->published_revision = impl_->problems_revision;
    }
    const bool published = replaceOutputProblems(problems);
    std::lock_guard lock{impl_->mutex};
    if (!published && impl_->generation == installed_generation) {
        impl_->published_revision = 0;
        impl_->problems_revision = impl_->generation;
    }
}

namespace {
void allocateSlot(OutputSinks::Impl::SlotResource &slot, vk::DeviceSize bytes) {
    auto &vulkan = GET_MODULE(VulkanManageCore);
    slot.buffer = vulkan.allocBuf(
        bytes, vk::BufferUsageFlagBits::eTransferDst,
        vma::MemoryUsage::eAutoPreferHost,
        vma::AllocationCreateFlagBits::eMapped |
            vma::AllocationCreateFlagBits::eHostAccessRandom);
    const auto info = vulkan.getAllocator().getAllocationInfo(slot.buffer.allocation.get());
    const auto memory = vulkan.getPhysDevice().getMemoryProperties();
    const auto properties = memory.memoryTypes[info.memoryType].propertyFlags;
    slot.coherent = bool(properties & vk::MemoryPropertyFlagBits::eHostCoherent);
    slot.bytes = bytes;
    slot.command_pool = vulkan.getDevice().createCommandPoolUnique(
        {vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
         vulkan.getGraphicsQueueFamilyIndex()});
    vk::CommandBufferAllocateInfo alloc_info{slot.command_pool.get(),
        vk::CommandBufferLevel::ePrimary, 1};
    slot.command_buffer = std::move(
        vulkan.getDevice().allocateCommandBuffersUnique(alloc_info).front());
    slot.fence = vulkan.getDevice().createFenceUnique({});
}
void invalidateSlot(OutputSinks::Impl::SlotResource &slot, vk::DeviceSize bytes) {
    if (slot.coherent) return;
    auto &vulkan = GET_MODULE(VulkanManageCore);
    const auto atom = vulkan.getPhysDevice().getProperties().limits.nonCoherentAtomSize;
    const auto allocation = vulkan.getAllocator().getAllocationInfo(
        slot.buffer.allocation.get());
    const vk::DeviceSize atom_mask = atom - 1;
    const auto end = std::min(allocation.size, (bytes + atom_mask) & ~atom_mask);
    vulkan.getAllocator().invalidateAllocation(slot.buffer.allocation.get(), 0, end);
}
}

void OutputSinks::Impl::discardUnstarted() {
    std::size_t index = 0;
    while (index < work.size()) {
        auto &entry = work[index];
        if (entry.pool->queue.state(entry.slot) == OutputSlotState::sending) {
            ++index;
            continue;
        }
        if (!entry.discard) {
            const auto *queued = entry.pool->queue.entry(entry.slot);
            if (queued != nullptr)
                const_cast<OutputSlotEntry *>(queued)->discard = true;
            entry.discard = true;
        }
        ++index;
    }
    noteProblem(*current.front());
}

void OutputSinks::Impl::finishSlot(Work &entry, bool written,
    std::uint64_t written_bytes, std::string error, bool limit,
    bool device_error) {
    std::lock_guard lock{mutex};
    auto &slot = *entry.pool->resources[entry.slot];
    if (written) {
        bytes_written += written_bytes;
        ++entry.runtime->state->frames_written;
    }
    const auto max_frame_complete = max_frames != 0 &&
        entry.runtime->state->frames_written >= max_frames &&
        entry.pool->queue.queueDepth() == 1;
    if (!limit && !error.empty() && entry.generation == generation) {
        entry.runtime->state->state = "refused";
        entry.runtime->state->last_error = error;
        noteProblem(*entry.runtime);
    } else if (!limit && max_frame_complete && !entry.discard) {
        entry.runtime->state->state = "finished";
        entry.runtime->state->stop_reason = "max_frames";
        entry.runtime->state->stop_value = max_frames;
        noteProblem(*entry.runtime);
    }
    if (limit) {
        for (auto &runtime : current) {
            if (runtime->state->state != "running") continue;
            runtime->state->state = "finished";
            runtime->state->stop_reason = "max_bytes";
            runtime->state->stop_value = max_bytes;
            runtime->state->last_error = error.empty()
                ? "output.limit_reached@1: max_bytes " +
                    std::to_string(max_bytes) + " reached"
                : error;
            noteProblem(*runtime);
        }
        discardUnstarted();
    }
    if (device_error) device_lost = true;
    entry.pool->queue.finish(entry.slot);
    slot.source_lease.reset();
    syncCounters(entry.runtime);
    worker_busy = false;
    work_cv.notify_all();
    free_cv.notify_all();
}

void OutputSinks::Impl::process(Work entry) {
    auto &slot = *entry.pool->resources[entry.slot];
    std::string error;
    bool device_error = false;
    bool ready = entry.discard && drain_expired && !entry.wait_for_fence;
    if (!ready) {
        while (true) {
            {
                std::lock_guard lock{mutex};
                if (device_lost || (drain_expired && !entry.wait_for_fence)) break;
            }
            try {
                auto &vulkan = GET_MODULE(VulkanManageCore);
                const vk::Fence fence = slot.fence.get();
                const auto result = vulkan.getDevice().waitForFences(
                    fence, vk::True,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::milliseconds(100)).count());
                if (result == vk::Result::eSuccess) {
                    ready = true;
                    break;
                }
                if (result != vk::Result::eTimeout) {
                    error = "output.read_failed@1: fence wait failed";
                    break;
                }
            } catch (const vk::DeviceLostError &) {
                device_error = true;
                break;
            } catch (const std::exception &e) {
                error = std::string{"output.read_failed@1: "} + e.what();
                break;
            }
        }
    }
    std::vector<std::uint8_t> pixels;
    std::optional<std::uint64_t> budget;
    const auto bytes = static_cast<vk::DeviceSize>(entry.extent.width) *
        entry.extent.height * 4;
    if (ready && !entry.discard && !device_error && error.empty()) {
        invalidateSlot(slot, bytes);
        const auto info = GET_MODULE(VulkanManageCore).getAllocator().
            getAllocationInfo(slot.buffer.allocation.get());
        pixels.resize(static_cast<std::size_t>(bytes));
        std::memcpy(pixels.data(), info.pMappedData, pixels.size());
        std::unique_lock lock{mutex};
        if (!entry.video_frame && max_bytes != 0) {
            if (bytes_written >= max_bytes) {
                lock.unlock();
                finishSlot(entry, false, 0,
                    "output.limit_reached@1: max_bytes " +
                        std::to_string(max_bytes) + " reached", true, false);
                return;
            }
            budget = max_bytes - bytes_written;
        }
    }
    if (entry.video_frame) {
        if (ready && !entry.discard && !device_error && error.empty()) {
            entry.runtime->video->complete(entry.video_frame, std::move(pixels));
        } else {
            entry.runtime->video->cancel(entry.video_frame);
        }
        finishSlot(entry, false, 0, std::move(error), false, device_error);
        return;
    }
    if ((!entry.discard && !device_error) && (ready || !error.empty())) {
        if (ready) {
            auto directory_copy = directory;
            const auto format = outputFileFormat(entry.format);
            const auto written = writeOutputFrameFile(directory_copy,
                entry.runtime->spec.name, entry.frame,
                OutputFileImage{.width = entry.extent.width,
                    .height = entry.extent.height, .format = *format,
                    .pixels = pixels},
                entry.encoding, budget);
            if (written.status == OutputFileWriteStatus::written) {
                finishSlot(entry, true, written.bytes_written, {}, false, false);
                return;
            }
            if (written.status == OutputFileWriteStatus::limit_reached) {
                finishSlot(entry, false, 0, written.message, true, false);
                return;
            }
            error = written.message.empty()
                ? (written.status == OutputFileWriteStatus::exists
                    ? "output.exists@1" : "output.write_failed@1")
                : written.message;
        }
        std::unique_lock lock{mutex};
        for (auto &queued : work) {
            if (queued.runtime.get() != entry.runtime.get() ||
                queued.pool->queue.state(queued.slot) == OutputSlotState::sending)
                continue;
            queued.discard = true;
            const auto *item = queued.pool->queue.entry(queued.slot);
            if (item != nullptr) const_cast<OutputSlotEntry *>(item)->discard = true;
        }
        lock.unlock();
        finishSlot(entry, false, 0, std::move(error), false, false);
        return;
    }
    finishSlot(entry, false, 0, {}, false, device_error);
}

void OutputSinks::Impl::run() {
    while (true) {
        std::optional<Work> selected;
        {
            std::unique_lock lock{mutex};
            work_cv.wait(lock, [&] {
                return (!work.empty() && work.front().submitted) ||
                       stopping || paused;
            });
            if (paused) {
                pause_acknowledged = true;
                pause_cv.notify_all();
                if (resume_at) {
                    const auto deadline = *resume_at;
                    work_cv.wait_until(lock, deadline, [&] {
                        return !paused || stopping;
                    });
                    if (paused && Clock::now() >= deadline) {
                        paused = false;
                        resume_at.reset();
                    }
                } else {
                    work_cv.wait(lock, [&] {
                        return !paused || stopping || resume_at.has_value();
                    });
                }
                free_cv.notify_all();
                pause_cv.notify_all();
            }
            if (work.empty() || !work.front().submitted) {
                if (work.empty() && stopping) return;
                continue;
            }
            selected = std::move(work.front());
            work.pop_front();
            worker_busy = true;
            selected->wait_for_fence = selected->pool->queue.state(selected->slot) ==
                OutputSlotState::gpu_pending;
            selected->pool->queue.markSending(selected->slot);
            syncCounters(selected->runtime);
            if (stopping && drain_expired) selected->discard = true;
        }
        try {
            process(*selected);
        } catch (const vk::DeviceLostError &) {
            if (selected->video_frame)
                selected->runtime->video->cancel(selected->video_frame);
            finishSlot(*selected, false, 0, {}, false, true);
        } catch (const std::exception &e) {
            if (selected->video_frame)
                selected->runtime->video->cancel(selected->video_frame);
            finishSlot(*selected, false, 0,
                std::string{"output.write_failed@1: "} + e.what(), false, false);
        } catch (...) {
            if (selected->video_frame)
                selected->runtime->video->cancel(selected->video_frame);
            finishSlot(*selected, false, 0,
                "output.write_failed@1: unknown output worker error", false, false);
        }
    }
}

void OutputSinks::captureFrame(Renderer &renderer) {
    const auto capture_begin = Clock::now();
    if (renderer.publishedLogicalFrameCount() == 0) return;
    const auto frame = renderer.publishedLogicalFrameCount() - 1;
    std::uint64_t capture_generation = 0;
    {
        std::lock_guard lock{impl_->mutex};
        if (impl_->device_lost) throw vk::DeviceLostError("Output capture device lost");
        if (!impl_->active || !impl_->table) return;
        capture_generation = impl_->generation;
        impl_->ensureWorker();
    }
    std::vector<std::shared_ptr<Impl::Runtime>> captures;
    {
        std::lock_guard lock{impl_->mutex};
        captures = impl_->current;
    }
    for (const auto &runtime : captures) {
        if (runtime->generation != capture_generation) continue;
        if (runtime->spec.every_n == 0 || frame % runtime->spec.every_n != 0)
            continue;
        double selection_ms = 0.0;
        double record_submit_ms = 0.0;
        Pelican::OutputCaptureResult read;
        try {
            read = renderer.submitOutputCapture(runtime->spec.target,
                runtime->spec.graph, {}, {}, {});
        } catch (const vk::DeviceLostError &) {
            throw;
        } catch (const std::exception &e) {
            std::lock_guard lock{impl_->mutex};
            runtime->state->state = "refused";
            runtime->state->last_error = std::string{"output.read_failed@1: "} + e.what();
            impl_->noteProblem(*runtime);
            continue;
        }
        if (read.status == OutputReadStatus::inactive) {
            std::lock_guard lock{impl_->mutex};
            ++impl_->frames_skipped_inactive;
            continue;
        }
        if (read.status == OutputReadStatus::no_contents) {
            std::lock_guard lock{impl_->mutex};
            ++impl_->frames_skipped_no_contents;
            continue;
        }
        if (read.status == OutputReadStatus::refused) {
            std::lock_guard lock{impl_->mutex};
            runtime->state->state = "refused";
            runtime->state->last_error = read.error;
            impl_->noteProblem(*runtime);
            continue;
        }
        if (!outputFileFormat(read.format)) {
            std::lock_guard lock{impl_->mutex};
            runtime->state->state = "refused";
            runtime->state->last_error = "output.format_unsupported@1: " +
                vk::to_string(read.format);
            impl_->noteProblem(*runtime);
            continue;
        }
        const auto drop = runtime->spec.when_full == "drop";
        std::shared_ptr<OutputVideoFrame> video_frame;
        if (runtime->video) {
            const auto &time = GET_MODULE(EngineTime);
            video_frame = runtime->video->reserve({
                .capture = frame,
                .monotonic_nanoseconds = std::chrono::duration_cast<
                    std::chrono::nanoseconds>(capture_begin.time_since_epoch()).count(),
                .time_revision = time.timeSetRevision(),
                .playing = time.playState() == EngineTime::PlayState::playing,
                .width = read.extent.width,
                .height = read.extent.height,
                .bgra = read.format == vk::Format::eB8G8R8A8Unorm ||
                        read.format == vk::Format::eB8G8R8A8Srgb,
            }, !drop);
            if (!video_frame) {
                std::lock_guard lock{impl_->mutex};
                impl_->syncCounters(runtime);
                impl_->noteProblem(*runtime);
                continue;
            }
        }
        OutputCaptureChoice choice = OutputCaptureChoice::full;
        std::size_t slot = 0;
        const auto selection_begin = Clock::now();
        try {
            std::unique_lock lock{impl_->mutex};
            while (true) {
                if (runtime->generation != impl_->generation ||
                    impl_->stopping || runtime->state->state != "running") break;
                if (!runtime->video && impl_->max_frames != 0 &&
                    runtime->state->frames_written + runtime->pool->queue.size() >=
                        impl_->max_frames) break;
                if (runtime->pool->queue.queueDepth() == output_slot_count) {
                    auto &vulkan = GET_MODULE(VulkanManageCore);
                    for (std::size_t candidate = 0; candidate < output_slot_count;
                         ++candidate) {
                        const auto *entry = runtime->pool->queue.entry(candidate);
                        if (entry == nullptr || !entry->discard ||
                            entry->generation == capture_generation ||
                            runtime->pool->queue.state(candidate) !=
                                OutputSlotState::gpu_pending) continue;
                        auto &resource = *runtime->pool->resources[candidate];
                        try {
                            if (vulkan.getDevice().getFenceStatus(resource.fence.get()) !=
                                vk::Result::eSuccess) continue;
                        } catch (const vk::DeviceLostError &) {
                            impl_->device_lost = true;
                            impl_->work_cv.notify_all();
                            impl_->free_cv.notify_all();
                            throw;
                        }
                        runtime->pool->queue.markCpuReady(candidate);
                        runtime->pool->queue.discardReady(candidate);
                        resource.source_lease.reset();
                        const auto found = std::find_if(impl_->work.begin(),
                            impl_->work.end(), [&](const Impl::Work &queued) {
                                return queued.pool == runtime->pool &&
                                       queued.slot == candidate;
                            });
                        if (found != impl_->work.end()) {
                            if (found->video_frame)
                                found->runtime->video->cancel(found->video_frame);
                            impl_->work.erase(found);
                        }
                        impl_->syncCounters(runtime);
                        impl_->free_cv.notify_all();
                    }
                }
                if (!drop && runtime->pool->queue.queueDepth() == output_slot_count) {
                    const auto begin = Clock::now();
                    impl_->free_cv.wait(lock, [&] {
                        return runtime->pool->queue.queueDepth() < output_slot_count ||
                               impl_->stopping ||
                               runtime->generation != impl_->generation ||
                               runtime->state->state != "running";
                    });
                    runtime->state->render_wait_ms += std::chrono::duration<double,
                        std::milli>(Clock::now() - begin).count();
                    continue;
                }
                if (drop && runtime->pool->queue.queueDepth() == output_slot_count) {
                    auto &vulkan = GET_MODULE(VulkanManageCore);
                    for (std::size_t candidate = 0; candidate < output_slot_count;
                         ++candidate) {
                        if (runtime->pool->queue.state(candidate) !=
                            OutputSlotState::gpu_pending) continue;
                        const auto &resource = *runtime->pool->resources[candidate];
                        try {
                            if (vulkan.getDevice().getFenceStatus(resource.fence.get()) ==
                                vk::Result::eSuccess)
                                runtime->pool->queue.markCpuReady(candidate);
                        } catch (const vk::DeviceLostError &) {
                            impl_->device_lost = true;
                            impl_->work_cv.notify_all();
                            impl_->free_cv.notify_all();
                            throw;
                        }
                    }
                }
                const auto selected = runtime->pool->queue.capture(
                    frame, capture_generation, drop);
                choice = selected.choice;
                if (selected.slot) slot = *selected.slot;
                if (choice == OutputCaptureChoice::dropped_new) {
                    if (video_frame) runtime->video->cancel(video_frame);
                    impl_->syncCounters(runtime);
                    break;
                }
                if (choice == OutputCaptureChoice::full) continue;
                if (selected.replaced_frame) {
                    const auto found = std::find_if(impl_->work.begin(),
                        impl_->work.end(), [&](const Impl::Work &entry) {
                            return entry.pool == runtime->pool && entry.slot == slot;
                        });
                    if (found != impl_->work.end()) {
                        if (found->video_frame)
                            found->runtime->video->cancel(found->video_frame);
                        impl_->work.erase(found);
                    }
                    runtime->pool->resources[slot]->source_lease.reset();
                }
                auto &resource = runtime->pool->resources[slot];
                const auto required = static_cast<vk::DeviceSize>(read.extent.width) *
                    read.extent.height * 4;
                if (!resource || resource->bytes != required) {
                    resource = std::make_unique<Impl::SlotResource>();
                    allocateSlot(*resource, required);
                }
                resource->source_lease = read.source_lease;
                runtime->pool->extent = read.extent;
                runtime->pool->format = read.format;
                impl_->work.push_back({runtime, runtime->pool, slot, frame,
                    capture_generation, read.extent, read.format,
                    runtime->spec.format == "png" ? OutputFileEncoding::png
                                                 : OutputFileEncoding::raw,
                    false, false, false, video_frame});
                impl_->syncCounters(runtime);
                break;
            }
            if (choice == OutputCaptureChoice::full && video_frame)
                runtime->video->cancel(video_frame);
        } catch (const vk::DeviceLostError &) {
            if (video_frame) runtime->video->cancel(video_frame);
            throw;
        } catch (const std::exception &e) {
            if (video_frame) runtime->video->cancel(video_frame);
            std::lock_guard lock{impl_->mutex};
            runtime->state->state = "refused";
            runtime->state->last_error = std::string{"output.read_failed@1: "} + e.what();
            impl_->noteProblem(*runtime);
            continue;
        }

        selection_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - selection_begin).count();
        const auto record_timing = [&] {
            std::lock_guard lock{impl_->mutex};
            const auto depth = runtime->pool->queue.queueDepth();
            impl_->maximum_queue_depth = std::max(impl_->maximum_queue_depth, depth);
            impl_->capture_timings.push_back({frame, selection_ms, record_submit_ms,
                std::chrono::duration<double, std::milli>(
                    Clock::now() - capture_begin).count(), depth});
        };
        if (choice == OutputCaptureChoice::dropped_new ||
            choice == OutputCaptureChoice::full) {
            if (video_frame && choice == OutputCaptureChoice::full)
                runtime->video->cancel(video_frame);
            record_timing();
            continue;
        }
        const auto cancel_work = [&] {
            const auto found = std::find_if(impl_->work.begin(), impl_->work.end(),
                [&](const Impl::Work &entry) {
                    return entry.pool == runtime->pool && entry.slot == slot;
                });
            if (found != impl_->work.end()) {
                if (found->video_frame)
                    found->runtime->video->cancel(found->video_frame);
                impl_->work.erase(found);
            }
        };
        auto &resource = *runtime->pool->resources[slot];
        const auto record_begin = Clock::now();
        try {
            const auto submitted = renderer.submitOutputCapture(
                runtime->spec.target, runtime->spec.graph,
                resource.buffer.buffer.get(), resource.command_buffer.get(),
                resource.fence.get());
            if (submitted.status != OutputReadStatus::ready) {
                std::lock_guard lock{impl_->mutex};
                runtime->pool->queue.cancel(slot);
                resource.source_lease.reset();
                cancel_work();
                if (runtime->video) runtime->video->abort(submitted.error);
                else {
                    runtime->state->state = "refused";
                    runtime->state->last_error = submitted.error;
                }
                impl_->noteProblem(*runtime);
                impl_->syncCounters(runtime);
                impl_->free_cv.notify_all();
            } else {
                std::lock_guard lock{impl_->mutex};
                const auto found = std::find_if(impl_->work.begin(), impl_->work.end(),
                    [&](const Impl::Work &entry) {
                        return entry.pool == runtime->pool && entry.slot == slot &&
                               entry.frame == frame;
                    });
                if (found != impl_->work.end()) found->submitted = true;
            }
        } catch (const vk::DeviceLostError &) {
            std::lock_guard lock{impl_->mutex};
            runtime->pool->queue.cancel(slot);
            resource.source_lease.reset();
            cancel_work();
            if (runtime->video)
                runtime->video->abort("output.video_encoder_failed@1: device lost");
            impl_->device_lost = true;
            impl_->syncCounters(runtime);
            impl_->work_cv.notify_all();
            impl_->free_cv.notify_all();
            throw;
        } catch (const std::exception &e) {
            std::lock_guard lock{impl_->mutex};
            runtime->pool->queue.cancel(slot);
            resource.source_lease.reset();
            cancel_work();
            const auto error = std::string{"output.read_failed@1: "} + e.what();
            if (runtime->video) runtime->video->abort(error);
            else {
                runtime->state->state = "refused";
                runtime->state->last_error = error;
            }
            impl_->noteProblem(*runtime);
            impl_->syncCounters(runtime);
            impl_->free_cv.notify_all();
        }
        record_submit_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - record_begin).count();
        record_timing();
        impl_->work_cv.notify_one();
    }

    std::vector<nlohmann::json> problems;
    std::uint64_t revision = 0;
    {
        std::lock_guard lock{impl_->mutex};
        if (impl_->published_revision == impl_->problems_revision) return;
        revision = impl_->problems_revision;
        std::vector<std::shared_ptr<State>> states;
        for (const auto &runtime : impl_->current) states.push_back(runtime->state);
        problems = outputSinkProblems(statesToValues(states));
    }
    if (replaceOutputProblems(problems)) {
        std::lock_guard lock{impl_->mutex};
        if (impl_->generation == capture_generation &&
            impl_->problems_revision == revision)
            impl_->published_revision = revision;
    }
}

nlohmann::json OutputSinks::status() const {
    std::unique_lock lock{impl_->mutex};
    if (impl_->worker_started && !impl_->paused) {
        impl_->work_cv.wait(lock, [&] {
            return (impl_->work.empty() && !impl_->worker_busy) ||
                   impl_->paused || impl_->stopping;
        });
    }
    for (const auto &runtime : impl_->current) {
        const auto previous = runtime->state->state + runtime->state->last_error +
                              runtime->state->stop_reason;
        impl_->syncCounters(runtime);
        const auto now = runtime->state->state + runtime->state->last_error +
                         runtime->state->stop_reason;
        if (previous != now) impl_->noteProblem(*runtime);
    }
    std::vector<nlohmann::json> problems;
    std::uint64_t generation = impl_->generation;
    std::uint64_t revision = impl_->problems_revision;
    const bool publish = impl_->published_revision != revision;
    if (publish) {
        std::vector<std::shared_ptr<State>> states;
        for (const auto &runtime : impl_->current) states.push_back(runtime->state);
        problems = outputSinkProblems(statesToValues(states));
    }
    lock.unlock();
    if (publish && replaceOutputProblems(problems)) {
        lock.lock();
        if (impl_->generation == generation && impl_->problems_revision == revision)
            impl_->published_revision = revision;
    } else {
        lock.lock();
    }
    nlohmann::json sinks = nlohmann::json::array();
    for (const auto &runtime : impl_->current) {
        const auto &state = *runtime->state;
        auto item = nlohmann::json{{"name", state.name}, {"kind", state.kind},
            {"format", state.format}, {"when_full", state.when_full},
            {"state", state.state}, {"stop_reason", state.stop_reason},
            {"stop_value", state.stop_value},
            {"frames_written", state.frames_written},
            {"last_error", state.last_error},
            {"queue_depth", runtime->video ? state.queue_depth
                                          : runtime->pool->queue.queueDepth()},
            {"frames_dropped", state.frames_dropped},
            {"frames_discarded", state.frames_discarded},
            {"render_wait_ms", state.render_wait_ms}};
        if (runtime->video) {
            item["mode"] = state.mode;
            item["frames_repeated"] = state.frames_repeated;
            item["encoder_pid"] = state.encoder_pid;
            item["encoder_state"] = state.encoder_state;
            item["final_bytes"] = state.final_bytes;
        }
        sinks.push_back(std::move(item));
    }
    return {{"active", impl_->active}, {"sinks", std::move(sinks)},
        {"frames_skipped_inactive", impl_->frames_skipped_inactive},
        {"frames_skipped_no_contents", impl_->frames_skipped_no_contents}};
}

#if PELICAN_OUTPUT_TEST_SEAMS
void OutputSinks::pauseWorkerForTesting(bool paused) {
    std::unique_lock lock{impl_->mutex};
    if (!impl_->worker_started) return;
    impl_->resume_at.reset();
    impl_->paused = paused;
    if (paused) {
        impl_->work_cv.notify_all();
        lock.unlock();
        impl_->work_cv.notify_all();
        lock.lock();
        impl_->pause_cv.wait(lock, [&] {
            return impl_->pause_acknowledged || !impl_->worker_started;
        });
    } else {
        impl_->pause_acknowledged = false;
        lock.unlock();
        impl_->work_cv.notify_all();
        impl_->free_cv.notify_all();
    }
}

void OutputSinks::resumeWorkerAfterForTesting(std::chrono::milliseconds delay) {
    std::lock_guard lock{impl_->mutex};
    impl_->resume_at = Clock::now() + delay;
    impl_->work_cv.notify_all();
}

nlohmann::json OutputSinks::takeCaptureTimingsForTesting() {
    std::lock_guard lock{impl_->mutex};
    nlohmann::json samples = nlohmann::json::array();
    for (const auto &timing : impl_->capture_timings) {
        samples.push_back({{"frame", timing.frame},
            {"selection_ms", timing.selection_ms},
            {"record_submit_ms", timing.record_submit_ms},
            {"total_ms", timing.total_ms},
            {"queue_depth", timing.queue_depth}});
    }
    const auto maximum_queue_depth = impl_->maximum_queue_depth;
    impl_->capture_timings.clear();
    impl_->maximum_queue_depth = 0;
    return {{"samples", std::move(samples)},
        {"maximum_queue_depth", maximum_queue_depth}};
}
#endif

} // namespace Pelican
