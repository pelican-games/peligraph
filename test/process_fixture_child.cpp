#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

std::string childMode() {
#ifdef _WIN32
    char *buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, "PELICAN_WP173_CHILD_MODE") != 0 || buffer == nullptr) return {};
    const std::string result{buffer};
    std::free(buffer);
    return result;
#else
    const char *value = std::getenv("PELICAN_WP173_CHILD_MODE");
    return value == nullptr ? std::string{} : std::string{value};
#endif
}

std::string rpcStringField(const std::string &request,
                           std::string_view field) {
    const std::string prefix = "\"" + std::string{field} + "\":\"";
    const std::size_t begin = request.find(prefix);
    if (begin == std::string::npos) return {};
    const std::size_t value_begin = begin + prefix.size();
    const std::size_t end = request.find('"', value_begin);
    if (end == std::string::npos) return {};
    return request.substr(value_begin, end - value_begin);
}

long long rpcIntegerField(const std::string &request,
                          std::string_view field) {
    const std::string prefix = "\"" + std::string{field} + "\":";
    const std::size_t begin = request.find(prefix);
    if (begin == std::string::npos) return 0;
    return std::stoll(request.substr(begin + prefix.size()));
}

// Q-2. One `result` member whose bytes a document round trip would not
// preserve: the escaped quote, the braces inside the string, the trailing
// fraction and the member order all survive only if the reply is handed on as
// the bytes it arrived as.
const char *envelopeTrickyResult() {
    return R"json({"text":"a\"b{}[],:","nested":[{"}":"]"}],"n":1.5,"b":true})json";
}

int environmentInteger(const char *name, int fallback) {
#ifdef _WIN32
    char *buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr)
        return fallback;
    const std::string value{buffer};
    std::free(buffer);
#else
    const char *raw = std::getenv(name);
    if (raw == nullptr) return fallback;
    const std::string value{raw};
#endif
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

void answerRpcOnStandardInput() {
    std::string request;
    while (std::getline(std::cin, request)) {
        const long long id = rpcIntegerField(request, "id");
        if (rpcStringField(request, "method") == "wait_for_shutdown") {
            // Leave a known request pending so the viewport shutdown test
            // exercises transport failure emitted while QProcess is reaped.
            std::cout << "fixture-rpc-held\n" << std::flush;
            continue;
        }
        std::cout << "{\"id\":" << id
                  << ",\"jsonrpc\":\"2.0\",\"result\":{\"contract\":1}}\n"
                  << std::flush;
    }
}

#ifdef _WIN32

// Q-4. What the engine does to Studio, in one fixture: a real top-level window
// that is visible almost at once, on a thread that does not take a single
// message until its "project" has finished loading. Re-parenting such a window
// blocks the caller for the rest of that load, which is the freeze Q-4 is
// about; with the wait in place nothing is re-parented until this process
// starts pumping.
LRESULT CALLBACK fixtureWindowProc(HWND window, UINT message, WPARAM wparam,
                                   LPARAM lparam) {
    if (message == WM_CLOSE || message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int runViewportWindowFixture() {
    // Qt runs per-monitor aware, and NativeWindowHost refuses to embed across
    // a DPI-awareness boundary. Match it, or the test would measure that
    // refusal instead of the wait.
    using SetAwareness = BOOL(WINAPI *)(void *);
    if (auto *set_awareness = reinterpret_cast<SetAwareness>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"),
            "SetProcessDpiAwarenessContext"))) {
        set_awareness(reinterpret_cast<void *>(static_cast<intptr_t>(-4)));
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(WNDCLASSEXW);
    window_class.lpfnWndProc = fixtureWindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"PelicanQ4EngineFixture";
    if (RegisterClassExW(&window_class) == 0) return 6;
    // Off every monitor: the fixture's window is a fact about HWNDs, not
    // something anybody should have flashed at them during a test run.
    HWND window = CreateWindowExW(
        0, window_class.lpszClassName, L"pelican fixture engine",
        WS_OVERLAPPEDWINDOW, -32000, -32000, 320, 200, nullptr, nullptr,
        window_class.hInstance, nullptr);
    if (window == nullptr) return 7;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    std::cout << "viewport-window-ready\n" << std::flush;

    // The load. Not one message is taken off the queue for this long -- and
    // the heartbeat, when asked for, is the engine writing its log while it
    // loads: the thing that must stop Studio from calling it unresponsive.
    const int pump_delay_ms = environmentInteger("PELICAN_Q4_PUMP_DELAY_MS", 0);
    const int heartbeat_ms = environmentInteger("PELICAN_Q4_HEARTBEAT_MS", 0);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{pump_delay_ms};
    auto next_heartbeat = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline) {
        if (heartbeat_ms > 0 &&
            std::chrono::steady_clock::now() >= next_heartbeat) {
            std::cerr << "fixture-engine-loading\n" << std::flush;
            next_heartbeat = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds{heartbeat_ms};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    // A window that has started pumping has not necessarily answered an RPC.
    // The delay is what separates the two facts for the test that says which
    // one Studio calls "ready".
    const int rpc_delay_ms = environmentInteger("PELICAN_Q4_RPC_DELAY_MS", 0);
    std::thread{[rpc_delay_ms] {
        std::this_thread::sleep_for(std::chrono::milliseconds{rpc_delay_ms});
        answerRpcOnStandardInput();
    }}.detach();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

#endif

} // namespace

int main(int argc, char *argv[]) {
#ifdef _WIN32
    if (childMode() == "viewport-window") {
        return runViewportWindowFixture();
    }
#endif
    if (childMode() == "viewport-hang") {
        std::cout << "viewport-hang-ready\n" << std::flush;
        for (;;) std::this_thread::sleep_for(std::chrono::seconds{1});
    }
    if (argc >= 2 && std::string_view{argv[1]} == "psd-extract") {
        if (childMode() == "hang") {
            std::cout << "importer-stdout-marker\n" << std::flush;
            std::cerr << "importer-stderr-marker\n" << std::flush;
            for (;;) std::this_thread::sleep_for(std::chrono::seconds{1});
        }
        return 3;
    }
    if (argc >= 2 && std::string_view{argv[1]} == "emit") {
        const std::string_view value = argc >= 3 ? argv[2] : "none";
        std::cout << "stdout-capture:" << value << '\n';
        std::cerr << "stderr-capture:" << value << '\n';
        return 0;
    }
    if (argc >= 2 && std::string_view{argv[1]} == "hang") {
        std::cout << "hang-ready\n" << std::flush;
        for (;;) std::this_thread::sleep_for(std::chrono::seconds{1});
    }
    if (argc >= 2 && std::string_view{argv[1]} == "rpc") {
        std::cout << "ordinary-stdout-before-rpc\n" << std::flush;
        std::cerr << "ordinary-stderr-before-rpc\n" << std::flush;
        std::string request;
        if (!std::getline(std::cin, request)) {
            return 4;
        }
        std::cerr << "rpc-request:" << request << '\n' << std::flush;
        std::cout
            << R"json({"jsonrpc":"2.0","id":1,"result":{"contract":1,"hit":null}})json"
            << '\n'
            << std::flush;
        return 0;
    }
    if (argc >= 2 && std::string_view{argv[1]} == "rpc-loop") {
        std::string request;
        while (std::getline(std::cin, request)) {
            const long long id = rpcIntegerField(request, "id");
            const std::string method = rpcStringField(request, "method");
            std::cerr << "rpc-loop-request:id=" << id
                      << ",method=" << method << '\n'
                      << std::flush;
            if (method == "get_gpu_timing") {
                std::cout
                    << "{\"id\":" << id
                    << R"json(,"jsonrpc":"2.0","result":{"enabled":false,"frame_count":0,"nodes":[],"reason":"wp351a_fixture_response","schema":"pelican.gpu_timing_node_averages","supported":false,"version":1,"window_size":30}})json"
                    << '\n'
                    << std::flush;
            } else {
                std::cout << "{\"id\":" << id
                          << ",\"jsonrpc\":\"2.0\",\"result\":{}}\n"
                          << std::flush;
            }
        }
        return 0;
    }
    // Q-2: replies shaped the way the single-pass envelope scanner has to
    // survive -- braces and brackets and an escaped quote inside a string,
    // spaces around every token, members in an order nobody promised, and a
    // line that begins like a reply but is not one.
    if (argc >= 2 && std::string_view{argv[1]} == "rpc-envelope") {
        std::string request;
        while (std::getline(std::cin, request)) {
            const long long id = rpcIntegerField(request, "id");
            if (id == 1) {
                std::cout << R"json({ "result" : )json"
                          << envelopeTrickyResult()
                          << R"json( , "jsonrpc" : "2.0" , "id" : 1 })json"
                          << '\n' << std::flush;
                continue;
            }
            if (id == 2) {
                std::cout
                    << R"json({"jsonrpc":"2.0","id":2,"result":{"a":[1,2})json"
                    << '\n' << std::flush;
                std::cout
                    << R"json({"jsonrpc":"2.0","id":2,"error":{"code":-32001,"message":"envelope probe"}})json"
                    << '\n' << std::flush;
                continue;
            }
            std::cout << R"json({"jsonrpc":"2.0","id":)json" << id
                      << R"json(,"result":null})json" << '\n'
                      << std::flush;
        }
        return 0;
    }
    return 2;
}
