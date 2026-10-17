#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dyf { class Gui; namespace RHI { class IDevice; } }
namespace tracy { class Worker; }

namespace viewer
{
// One instance on the Gui thread. Destroy it before Gui and the RHI device.
class EmbeddedTracy
{
public:
    // Add Korean fonts first; call once before the first Gui::BeginFrame.
    static bool PrepareFonts(std::string& error);
    EmbeddedTracy(dyf::Gui&, dyf::RHI::IDevice&);
    ~EmbeddedTracy();
    EmbeddedTracy(const EmbeddedTracy&) = delete;
    EmbeddedTracy& operator=(const EmbeddedTracy&) = delete;

    bool Connect(uint32_t pid, uint64_t creationTime, uint16_t port, std::string& error);
    // Tick also runs while hidden. It does not begin/end an ImGui frame.
    void Tick();
    // Hiding or closing the tab preserves its Worker and capture.
    void Draw(bool& visible);
    void Stop();
    bool Connected() const;
    bool HasCapture() const;
    // Poll until true; Draw (also when hidden) displays save/discard/cancel inside a GUI frame.
    bool RequestClose();
    bool ClosePending() const;
    bool CloseCancelled() const;
    // Explicit file operations preserve the current capture on failure.
    bool OpenCapture(const std::string& path, std::string& error);
    bool SaveCapture(const std::string& path, std::string& error);
    const std::string& LastError() const;
    // Nonblocking, live session only. Callback runs with the Worker data lock:
    // no nested locks, retained references, UI, file IO, or Worker mutation.
    bool ReadWorker(const std::function<void(tracy::Worker&)>& read);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
