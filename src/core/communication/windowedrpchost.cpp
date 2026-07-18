#include "rpcserver.hpp"

#include <atomic>
#include <deque>
#include <istream>
#include <mutex>
#include <ostream>
#include <thread>
#include <utility>

namespace Pelican {

namespace {

struct WindowedRpcQueueState {
    std::istream &input;
    std::ostream &output;
    const std::size_t capacity;
    mutable std::mutex queue_mutex;
    std::deque<std::string> requests;
    std::mutex output_mutex;
    std::atomic_bool stopping{false};
    std::atomic_bool reader_finished{false};

    WindowedRpcQueueState(std::istream &input_stream,
                          std::ostream &output_stream,
                          std::size_t queue_capacity)
        : input{input_stream}, output{output_stream}, capacity{queue_capacity} {}
};

void writeResponse(WindowedRpcQueueState &state, std::string_view response,
                   bool skip_when_stopping = false) {
    const std::lock_guard lock{state.output_mutex};
    if (skip_when_stopping && state.stopping.load(std::memory_order_acquire)) return;
    state.output << response << '\n';
    state.output.flush();
}

std::string busyResponse(std::string_view line, std::size_t capacity) {
    const auto parsed = parseJsonRpcRequest(line);
    auto id = nlohmann::json{nullptr};
    if (parsed.request) {
        id = parsed.request->id;
    } else if (parsed.error) {
        id = parsed.error->id;
    }
    return serializeJsonRpcError(makeJsonRpcError(
        std::move(id), JsonRpcErrorCodes::applicationError,
        "windowed rpc request queue is busy",
        {{"reason", "busy"}, {"queue_capacity", capacity}}));
}

void readInputLines(const std::shared_ptr<WindowedRpcQueueState> &state) {
    std::string line;
    while (std::getline(state->input, line)) {
        if (state->stopping.load(std::memory_order_acquire)) break;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        bool accepted = false;
        {
            const std::lock_guard lock{state->queue_mutex};
            if (state->requests.size() < state->capacity) {
                state->requests.push_back(line);
                accepted = true;
            }
        }
        if (!accepted) {
            writeResponse(*state, busyResponse(line, state->capacity), true);
        }
    }
    state->reader_finished.store(true, std::memory_order_release);
}

} // namespace

struct WindowedRpcHost::Impl {
    std::shared_ptr<WindowedRpcQueueState> state;
    LineHandler handler;
    std::thread reader;

    Impl(std::istream &input_stream, std::ostream &output_stream,
         LineHandler handler, std::size_t queue_capacity)
        : state{std::make_shared<WindowedRpcQueueState>(
              input_stream, output_stream, queue_capacity)},
          handler{std::move(handler)},
          reader{readInputLines, state} {}

    ~Impl() {
        state->stopping.store(true, std::memory_order_release);
        {
            // Finish any overflow response that began before stopping. A
            // later overflow writer observes stopping while holding this lock.
            const std::lock_guard lock{state->output_mutex};
        }
        if (!reader.joinable()) return;
        if (state->reader_finished.load(std::memory_order_acquire)) {
            reader.join();
        } else {
            // std::istream has no portable cancellation primitive. Production
            // uses process-lifetime std::cin, so leave a blocked reader with
            // shared queue state until process exit.
            reader.detach();
        }
    }
};

WindowedRpcHost::WindowedRpcHost(std::istream &input_stream,
                                 std::ostream &output_stream,
                                 LineHandler handler,
                                 std::size_t queue_capacity) {
    if (!handler) throw std::invalid_argument("WindowedRpcHost requires a line handler");
    if (queue_capacity == 0) throw std::invalid_argument("WindowedRpcHost queue capacity must be positive");
    impl_ = std::make_unique<Impl>(input_stream, output_stream,
                                   std::move(handler), queue_capacity);
}

WindowedRpcHost::~WindowedRpcHost() = default;

std::size_t WindowedRpcHost::processFrameBoundary() {
    std::deque<std::string> frame_requests;
    {
        const std::lock_guard lock{impl_->state->queue_mutex};
        frame_requests.swap(impl_->state->requests);
    }

    for (const auto &request : frame_requests) {
        writeResponse(*impl_->state, impl_->handler(request));
    }
    return frame_requests.size();
}

std::size_t WindowedRpcHost::queuedRequestCount() const {
    const std::lock_guard lock{impl_->state->queue_mutex};
    return impl_->state->requests.size();
}

bool WindowedRpcHost::readerFinished() const {
    return impl_->state->reader_finished.load(std::memory_order_acquire);
}

} // namespace Pelican
