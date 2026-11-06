// VO-4: the one source that sees Windows, D3D11 or Spout headers. Everything in
// this file runs on one sink's sender thread, except the factory's one call to
// disable logging, which is made before any such thread starts.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <SpoutDX.h>

#include "outputspout.hpp"

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

namespace Pelican {
namespace {

// SpoutUtils keeps its last log line in unsynchronized process-global state,
// written from every sender and receiver. DisableLogs makes _doLog return first.
void disableSpoutLogsOnce() {
    static std::once_flag once;
    std::call_once(once, [] { spoututils::DisableLogs(); });
}

// The shared texture is protected, and whether it exists after a failed size
// change decides between a passing failure and a terminal one.
class SenderDx final : public spoutDX {
  public:
    bool hasSharedTexture() const { return m_pSharedTexture != nullptr; }
};

class SpoutDxBackend final : public OutputSpoutBackend {
  public:
    explicit SpoutDxBackend(bool create_device) : create_device_(create_device) {}
    ~SpoutDxBackend() override { close(); }

    OutputSpoutResult open(const std::string &name) override {
        sender_ = std::make_unique<SenderDx>();
        // OpenDirectX11 returns true whether or not it made a device (SpoutDX.cpp:215).
        if (create_device_) (void)sender_->OpenDirectX11();
        if (sender_->GetDX11Device() == nullptr || sender_->GetDX11Context() == nullptr)
            return {OutputSpoutResultKind::open_failed, "no D3D11 device"};
        // RegisterSenderName skips the registration past the limit and still
        // returns true (SpoutSenderNames.cpp:168), so the limit is checked first.
        const auto count = sender_->GetSenderCount();
        const auto limit = sender_->GetMaxSenders();
        if (count >= limit) {
            return {OutputSpoutResultKind::open_failed,
                    "sender limit " + std::to_string(limit) + " reached"};
        }
        // Appends _1, _2... when the name is registered (SpoutDX.cpp:328); the
        // sender compares actualName() with what it asked for. Nothing is
        // published yet: that's the first SendImage.
        sender_->SetSenderName(name.c_str());
        actual_ = sender_->GetName();
        return {};
    }

    OutputSpoutResult send(const std::uint8_t *bgra, std::uint32_t width,
                           std::uint32_t height) override {
        // No pitch in this tag: UpdateSubresource uses width * 4 and no flip.
        const auto accepted =
            sender_->SendImage(const_cast<unsigned char *>(bgra), width, height);
        if (auto *device = sender_->GetDX11Device()) {
            const auto reason = device->GetDeviceRemovedReason();
            if (reason != S_OK) {
                char text[64];
                std::snprintf(text, sizeof(text), "device removed, HRESULT 0x%08lX",
                              static_cast<unsigned long>(reason));
                return {OutputSpoutResultKind::device_removed, text};
            }
        }
        if (!accepted) {
            if (!published_ || !sender_->hasSharedTexture()) {
                return {OutputSpoutResultKind::open_failed,
                        "no shared texture or sender for " + std::to_string(width) +
                            "x" + std::to_string(height)};
            }
            return {OutputSpoutResultKind::failed, "SendImage returned false"};
        }
        if (!published_) {
            if (!sender_->IsInitialized() ||
                !sender_->sendernames.FindSenderName(actual_.c_str())) {
                return {OutputSpoutResultKind::open_failed,
                        "'" + actual_ + "' isn't in Spout's sender list"};
            }
            published_ = true;
        }
        return {};
    }

    std::string actualName() const override { return actual_; }

    void close() override {
        if (!sender_) return;
        sender_->ReleaseSender();
        sender_->CloseDirectX11();
        sender_.reset();
        published_ = false;
    }

  private:
    const bool create_device_;
    std::unique_ptr<SenderDx> sender_;
    std::string actual_;
    bool published_ = false;
};
} // namespace

OutputSpoutBackendFactory outputSpoutRealBackendFactory() {
    disableSpoutLogsOnce();
    return [] { return std::make_unique<SpoutDxBackend>(true); };
}

#if PELICAN_OUTPUT_TEST_SEAMS
std::unique_ptr<OutputSpoutBackend> makeOutputSpoutBackendWithoutDeviceForTesting() {
    disableSpoutLogsOnce();
    return std::make_unique<SpoutDxBackend>(false);
}
#endif

} // namespace Pelican
