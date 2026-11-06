#pragma once

// VO-4: the test's own side of Spout. A real receiver, a raw sender that holds a
// name, and a holder of a sender's texture mutex. No Windows types here, so a
// test source that has its own windows.h habits can use it.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class SpoutTestReceiver {
  public:
    explicit SpoutTestReceiver(const std::string &sender);
    ~SpoutTestReceiver();
    // False means no D3D11 device: a skip, not a pass.
    bool deviceAvailable() const;
    // One ReceiveImage. Handles an update of the sender's size. Returns whether
    // the receiver is connected; that says nothing about new pixels.
    bool poll();
    std::uint32_t width() const;
    std::uint32_t height() const;
    // Tightly packed, top-first, in the texture's own channel order.
    const std::vector<std::uint8_t> &pixels() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool spoutTestSenderListed(const std::string &name);

// A sender that isn't the code under test, publishing one frame under a name.
class SpoutTestSender {
  public:
    explicit SpoutTestSender(const std::string &name);
    ~SpoutTestSender();
    bool published() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Holds a sender's texture access mutex from the calling thread, as a receiver
// that's slow to copy does.
class SpoutTestTextureHold {
  public:
    explicit SpoutTestTextureHold(const std::string &sender);
    ~SpoutTestTextureHold();
    bool held() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
