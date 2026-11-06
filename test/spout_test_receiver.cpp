#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <SpoutDX.h>

#include "spout_test_receiver.hpp"

struct SpoutTestReceiver::Impl {
    spoutDX receiver;
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

SpoutTestReceiver::SpoutTestReceiver(const std::string &sender)
    : impl_(std::make_unique<Impl>()) {
    impl_->receiver.SetReceiverName(sender.c_str());
    (void)impl_->receiver.OpenDirectX11();
}
SpoutTestReceiver::~SpoutTestReceiver() = default;

bool SpoutTestReceiver::deviceAvailable() const {
    return impl_->receiver.GetDX11Device() != nullptr &&
           impl_->receiver.GetDX11Context() != nullptr;
}

bool SpoutTestReceiver::poll() {
    auto &receiver = impl_->receiver;
    const auto connected = receiver.ReceiveImage(
        impl_->pixels.empty() ? nullptr : impl_->pixels.data(), impl_->width, impl_->height);
    if (receiver.IsUpdated()) {
        impl_->width = receiver.GetSenderWidth();
        impl_->height = receiver.GetSenderHeight();
        impl_->pixels.assign(
            static_cast<std::size_t>(impl_->width) * impl_->height * 4, 0);
    }
    return connected;
}
std::uint32_t SpoutTestReceiver::width() const { return impl_->width; }
std::uint32_t SpoutTestReceiver::height() const { return impl_->height; }
const std::vector<std::uint8_t> &SpoutTestReceiver::pixels() const { return impl_->pixels; }

bool spoutTestSenderListed(const std::string &name) {
    spoutSenderNames names;
    return names.FindSenderName(name.c_str());
}

struct SpoutTestSender::Impl {
    spoutDX sender;
    bool published = false;
};
SpoutTestSender::SpoutTestSender(const std::string &name) : impl_(std::make_unique<Impl>()) {
    (void)impl_->sender.OpenDirectX11();
    if (impl_->sender.GetDX11Device() == nullptr) return;
    impl_->sender.SetSenderName(name.c_str());
    std::vector<unsigned char> pixels(16, 255);
    impl_->published = impl_->sender.SendImage(pixels.data(), 2, 2) &&
                       impl_->sender.IsInitialized();
}
SpoutTestSender::~SpoutTestSender() {
    impl_->sender.ReleaseSender();
    impl_->sender.CloseDirectX11();
}
bool SpoutTestSender::published() const { return impl_->published; }

struct SpoutTestTextureHold::Impl {
    spoutFrameCount frame;
    bool held = false;
};
SpoutTestTextureHold::SpoutTestTextureHold(const std::string &sender)
    : impl_(std::make_unique<Impl>()) {
    if (impl_->frame.CreateAccessMutex(sender.c_str()))
        impl_->held = impl_->frame.CheckAccess();
}
SpoutTestTextureHold::~SpoutTestTextureHold() {
    if (impl_->held) impl_->frame.AllowAccess();
    impl_->frame.CloseAccessMutex();
}
bool SpoutTestTextureHold::held() const { return impl_->held; }
