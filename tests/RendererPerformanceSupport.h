#pragma once
#include <dyf/Renderer.h>
#include <dyf/Camera.h>
#include <dyf/Scene.h>
#include <dyf/RHI.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace RendererPerformance
{
namespace RHI = dyf::RHI;
namespace Math = dyf::Math;
inline void Check(bool condition, const std::string& message)
{ if(!condition) throw std::runtime_error(message); }

// A native hidden window avoids showing/focusing a helper during automated runs.
class HiddenWindow
{
    HWND window = nullptr;
public:
    HiddenWindow(uint32_t width, uint32_t height)
    {
        const auto module = GetModuleHandleW(nullptr);
        WNDCLASSW desc{};
        desc.lpfnWndProc = DefWindowProcW;
        desc.hInstance = module;
        desc.lpszClassName = L"DyRendererPerformanceHiddenWindow";
        Check(RegisterClassW(&desc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
            "Hidden window class registration failed");
        RECT size{0, 0, LONG(width), LONG(height)};
        Check(AdjustWindowRect(&size, WS_OVERLAPPEDWINDOW, FALSE) != 0, "Window sizing failed");
        window = CreateWindowExW(0, desc.lpszClassName, L"Renderer performance validation",
            WS_OVERLAPPEDWINDOW, 0, 0, size.right-size.left, size.bottom-size.top,
            nullptr, nullptr, module, nullptr);
        Check(window != nullptr, "Hidden window creation failed");
    }
    ~HiddenWindow() { if(window) DestroyWindow(window); }
    HWND Get() const { return window; }
};

inline dyf::RendererConfig Configuration(bool optimized, bool shadows = false)
{
    dyf::RendererConfig config;
    config.vsync = false;
    config.enableProfilerHud = false;
    config.allowReadback = true;
    config.clearColor = {.03f, .04f, .06f, 1};
    config.lighting.enabled = shadows;
    config.lighting.shadows = shadows;
    config.lighting.shadowQuality = dyf::ShadowQuality::Low;
    config.lighting.environment.diffuseIntensity = .05f;
    config.lighting.environment.specularIntensity = 0;
#ifndef RENDER_PERFORMANCE_BASELINE
    config.enableInstancing = optimized;
    config.enableFrustumCulling = optimized;
#else
    (void)optimized;
#endif
    return config;
}

struct Context
{
    HiddenWindow window;
    std::unique_ptr<RHI::IDevice> device;
    std::unique_ptr<dyf::Renderer> renderer;
    Context(uint32_t width, uint32_t height, bool optimized, bool shadows = false, bool validation = false)
        : window(width, height)
    {
        RHI::DeviceDesc desc;
        desc.maxFramesInFlight = 3;
        desc.enableValidation = validation;
        device.reset(RHI::IDevice::Create(desc));
        Check(device != nullptr && device->Supports(RHI::Feature::Rasterization),
            "Native rasterizing device required (Null is not pixel verification)");
        renderer = dyf::Renderer::Create(*device, window.Get(), Configuration(optimized, shadows));
        Check(renderer != nullptr, "Renderer creation failed");
    }
    ~Context() { if(device) (void)device->WaitIdle(); }
};

#ifdef DY_LIVE_PROFILING_ENABLED
inline bool BooleanAvailable(Context& context,const char* key)
{
    const auto snapshot=dyf::Diagnostics::ReadLiveSnapshot();Check(bool(snapshot),"Live profiling is required for optimization tests");
    for(const auto& control:snapshot->controls)if(control.source==context.renderer->GetLiveSourceId()&&control.owner==context.renderer->GetLiveSourceId()&&control.key==key){if(control.writable)return true;Check(!control.unavailableReason.empty(),"Unavailable optimization needs a reason");std::printf("native optimization skipped key=%s reason=%s\n",key,control.unavailableReason.c_str());return false;}
    throw std::runtime_error("Missing optimization availability: "+std::string(key));
}
inline dyf::Diagnostics::LiveId RequestBoolean(Context& context,const char* key,bool value,dyf::Diagnostics::LiveId owner=0)
{
    const auto snapshot=dyf::Diagnostics::ReadLiveSnapshot();Check(bool(snapshot),"Live profiling is required for optimization control tests");
    for(const auto& control:snapshot->controls)if(control.source==context.renderer->GetLiveSourceId()&&control.owner==(owner?owner:context.renderer->GetLiveSourceId())&&control.key==key){Check(control.writable,"Optimization control is unavailable: "+std::string(key));return dyf::Diagnostics::RequestLiveControl(control.id,value);}
    throw std::runtime_error("Missing optimization control: "+std::string(key));
}
inline dyf::Diagnostics::LiveFrameSummary AppliedBoolean(Context& context,dyf::Diagnostics::LiveId request,bool value)
{
    const auto snapshot=dyf::Diagnostics::ReadLiveSnapshot();bool applied=false;
    for(const auto& status:snapshot->controlStatus)if(status.request==request)applied=status.state==dyf::Diagnostics::ControlState::Applied&&status.actual==dyf::Diagnostics::ControlValue(value);
    Check(applied,"Optimization control did not apply at the captured frame boundary");
    for(const auto& source:snapshot->sources)if(source.source==context.renderer->GetLiveSourceId())return source.latest;
    throw std::runtime_error("Missing native optimization frame");
}
#endif
struct Target
{
    RHI::IDevice& device;
    RHI::TextureHandle texture = nullptr;
    RHI::ResourceState state = RHI::ResourceState::Undefined;
    RHI::FenceHandle completion;
    Target(RHI::IDevice& owner, uint32_t width, uint32_t height) : device(owner)
    {
        RHI::TextureDesc desc;
        desc.width = width; desc.height = height;
        desc.format = RHI::Format::R8G8B8A8_UNORM;
        desc.usage = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::ShaderResource;
        texture = device.CreateTexture(desc);
        Check(texture != nullptr, "Offscreen target creation failed");
    }
    ~Target() { device.DestroyTexture(texture); }
    void Render(dyf::Renderer& renderer, const dyf::Scene& scene, const dyf::Camera& camera)
    {
        Check(renderer.RenderToTexture(scene, camera, texture, state), "RenderToTexture failed");
        state = RHI::ResourceState::ShaderResource;
#ifndef RENDER_PERFORMANCE_BASELINE
        completion = renderer.GetLastRenderCompletion();
        Check(bool(completion), "Successful render did not publish completion");
#endif
    }
    std::vector<uint8_t> Capture()
    {
        RHI::TextureReadback image;
        Check(device.ReadTexture(texture, image), "Offscreen readback failed");
        Check(image.width == texture->GetDesc().width && image.height == texture->GetDesc().height &&
            image.rowPitch >= image.width * 4 && image.pixels.size() >= size_t(image.rowPitch)*image.height,
            "Readback dimensions/pitch differ from target");
        Check(image.format == RHI::Format::R8G8B8A8_UNORM, "Unexpected offscreen readback format");
        std::vector<uint8_t> rgba(size_t(image.width)*image.height*4);
        for(uint32_t y=0; y<image.height; ++y)
            std::copy_n(image.pixels.data()+size_t(y)*image.rowPitch, image.width*4,
                rgba.data()+size_t(y)*image.width*4);
        return rgba;
    }
};

inline dyf::Camera TopCamera(float extent = 8, float height = 8)
{
    dyf::Camera camera;
    Check(camera.LookAt({0,0,height}, {0,0,0}, {0,1,0}), "Camera pose failed");
    Check(camera.SetOrthographic(extent, extent, .1f, 50), "Camera projection failed");
    return camera;
}

struct Difference { unsigned max = 0; uint64_t changedPixels = 0; double mean = 0; };
inline Difference Compare(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual)
{
    Check(!expected.empty() && expected.size()==actual.size(), "Pixel buffers missing or different size");
    Difference result;
    uint64_t sum = 0;
    for(size_t p=0; p<expected.size(); p+=4)
    {
        bool changed = false;
        for(size_t c=0; c<3; ++c)
        {
            const auto delta = unsigned(std::abs(int(expected[p+c])-int(actual[p+c])));
            result.max = std::max(result.max, delta); sum += delta;
            changed = changed || delta > 2;
        }
        Check(expected[p+3]==actual[p+3], "Output alpha changed");
        result.changedPixels += changed;
    }
    result.mean = double(sum)/(expected.size()/4*3);
    return result;
}
inline void Equivalent(const char* label, const std::vector<uint8_t>& expected,
    const std::vector<uint8_t>& actual)
{
    const auto difference = Compare(expected, actual);
    std::printf("pixels %s max=%u mean=%.6f changed_gt_2=%llu/%llu\n", label, difference.max,
        difference.mean, static_cast<unsigned long long>(difference.changedPixels),
        static_cast<unsigned long long>(expected.size()/4));
    Check(difference.max <= 4 && difference.mean <= .03 &&
        difference.changedPixels <= expected.size()/4/2000,
        std::string(label)+": optimization changed rendered pixels beyond tolerance");
}
inline void NonBlank(const std::vector<uint8_t>& image)
{
    Check(image.size() >= 4, "Empty capture");
    uint64_t different = 0;
    for(size_t p=0;p<image.size();p+=4)
        different += std::abs(int(image[p])-int(image[0])) +
            std::abs(int(image[p+1])-int(image[1])) + std::abs(int(image[p+2])-int(image[2])) > 12;
    Check(different > image.size()/4/100, "Fixture did not render enough visible geometry");
}
inline RHI::FenceHandle Marker(RHI::IDevice& device, RHI::TimestampQueryHandle query = nullptr,
    uint32_t index = 0, bool reset = false)
{
    auto* commands = device.AcquireCommandList();
    Check(commands != nullptr, "Marker command list acquisition failed");
    struct Cleanup { RHI::IDevice& device; RHI::ICommandList* list;
        ~Cleanup() { device.DestroyCommandList(list); } } cleanup{device, commands};
    if(query)
    {
        if(reset) commands->ResetTimestamps(query, 0, 2);
        commands->WriteTimestamp(query, index);
    }
    else commands->GlobalBarrier();
    RHI::FenceHandle completion;
    Check(commands->Close() && device.Submit({&commands,1,nullptr,0}, completion) && bool(completion),
        "Marker submission failed");
    return completion;
}
inline void Wait(RHI::IDevice& device, RHI::FenceHandle fence)
{ Check(device.Wait(fence, 30000000000ull), "GPU completion wait failed"); }
}
