#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

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

} // namespace

int main(int argc, char *argv[]) {
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
    return 2;
}
