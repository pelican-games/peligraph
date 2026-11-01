#pragma once
#include <android/log.h>
#include <quill/sinks/AndroidSink.h>

namespace Pelican::Android {
class LogSink final : public quill::AndroidSink {
public:
    void write_log(quill::MacroMetadata const *, uint64_t, std::string_view,
                   std::string_view, std::string const &, std::string_view,
                   quill::LogLevel level, std::string_view, std::string_view,
                   std::vector<std::pair<std::string, std::string>> const *,
                   std::string_view message, std::string_view) override {
        const int priority = level >= quill::LogLevel::Error ? ANDROID_LOG_ERROR
                           : level >= quill::LogLevel::Warning ? ANDROID_LOG_WARN
                           : ANDROID_LOG_INFO;
        // Preserve long compiler diagnostics across logcat's entry limit.
        do {
            const std::string chunk{message.substr(0, 3000)};
            __android_log_write(priority, "pelican", chunk.c_str());
            message.remove_prefix(chunk.size());
        } while (!message.empty());
    }
};
}
