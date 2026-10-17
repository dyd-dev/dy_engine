#pragma once
#include <cstdint>
#include <memory>

namespace dyf { class Gui; namespace RHI { class IDevice; } }
namespace tracy { void EmbeddedToolbarNext(float width); }
namespace viewer
{
struct TracyImageSize { uint32_t rowBytes, totalBytes; };
TracyImageSize CheckedTracyImageSize(int width, int height);
class TracyPlatform
{
public:
    TracyPlatform(dyf::Gui&, dyf::RHI::IDevice&);
    ~TracyPlatform();
    void Tick();
    void MouseFrame(bool visible);
    void BeginDraw();
    void EndDraw();
private:
    struct Impl;
    friend Impl& TracyGraphics();
    std::unique_ptr<Impl> impl;
};
}
