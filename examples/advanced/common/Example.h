// 07·08의 공통 실행부. iOS의 CADisplayLink와 데스크톱 main이 동일한 Tick()을 호출한다.
// Tick: 프레임 확보 → 예제별 Draw(명령 제출 포함) → 선택적 캡처 → Present.
#pragma once
#include "dyf/RHI.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/Readback.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace advanced {
using namespace dyf::RHI;
#if defined(__ANDROID__)
inline constexpr Format ExampleColorFormat=Format::R8G8B8A8_UNORM;
#else
inline constexpr Format ExampleColorFormat=Format::B8G8R8A8_UNORM;
#endif
// GPU에 그대로 전달하는 64바이트 ABI다. 멤버 순서·정렬은 GLSL/HLSL/Metal과 맞춰야 한다.
// sizeTime은 화면 크기·시간·모드이며, settings/extra의 의미는 각 예제의 파서에서 정한다.
struct alignas(16) Params {
    float sizeTime[4] = {960, 640, 0, 0};
    float settings[4] = {1, 0.45f, 0.8f, 0.9f};
    float jitter[4] = {};
    float extra[4] = {};
};
static_assert(sizeof(Params) == 64 && alignof(Params) == 16);
static_assert(offsetof(Params, settings) == 16 && offsetof(Params, jitter) == 32 &&
              offsetof(Params, extra) == 48);

struct Options {
    Params params;
    uint64_t frameLimit = 0;
    bool framesSpecified = false;
    bool validation = false;
    bool fixedTime = false;
    bool help = false;
    float timeValue = 0;
    std::string capture;
};

inline double Number(const std::string &value, double minimum, double maximum) {
    size_t consumed = 0;
    double result = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(result) || result < minimum || result > maximum)
        throw std::runtime_error("Numeric option out of range: " + value);
    return result;
}
// 출력 텍스처만 RGB PPM으로 저장한다. UIKit의 선택 버튼 등은 캡처에 포함되지 않는다.
inline void Capture(IDevice &device, TextureHandle texture, const std::string &path) {
    TextureReadback image;
    if (!device.ReadTexture(texture, image))
        throw std::runtime_error("ReadTexture failed.");
    std::ofstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot open capture path.");
    stream << "P6\n" << image.width << " " << image.height << "\n255\n";
    const bool bgra = image.format == Format::B8G8R8A8_UNORM || image.format == Format::B8G8R8A8_UNORM_SRGB;
    for (uint32_t y = 0; y < image.height; y++)
        for (uint32_t x = 0; x < image.width; x++) {
            const auto *pixel = image.pixels.data() + size_t(y) * image.rowPitch + size_t(x) * 4;
            const char rgb[3] = {char(pixel[bgra ? 2 : 0]), char(pixel[1]), char(pixel[bgra ? 0 : 2])};
            stream.write(rgb, 3);
        }
    if (!stream)
        throw std::runtime_error("Capture write failed.");
    std::printf("Captured %s (%ux%u).\n", path.c_str(), image.width, image.height);
}

class Example {
  public:
    virtual ~Example() { (void)device.WaitIdle(); }

    // 그릴 이미지가 아직 준비되지 않으면 다음 호출에서 다시 시도한다.
    // false는 --frames로 지정한 프레임을 모두 그렸다는 뜻이다.
    bool Tick() {
        if (options.frameLimit && frame >= options.frameLimit)
            return false;
        if (!device.BeginFrame()) {
            if (device.IsLost())
                throw std::runtime_error("Device lost.");
            return true;
        }
        auto backbuffer = device.GetBackBuffer();
        if (!backbuffer)
            throw std::runtime_error("Backbuffer unavailable.");
        params.sizeTime[0] = float(backbuffer->GetDesc().width);
        params.sizeTime[1] = float(backbuffer->GetDesc().height);
        params.sizeTime[2] = options.fixedTime ? options.timeValue : float(frame) / 60.0f;
        Draw(backbuffer);
        ++frame;
        if (!options.capture.empty() && frame == options.frameLimit)
            Capture(device, backbuffer, options.capture);
        if (!device.Present())
            throw std::runtime_error("Present failed.");
        if (options.frameLimit && frame == options.frameLimit) {
            if (!device.WaitIdle())
                throw std::runtime_error("Final GPU wait failed.");
            return false;
        }
        return true;
    }

    void WaitIdle() {
        if (!device.WaitIdle())
            throw std::runtime_error("Final GPU wait failed.");
    }

  protected:
    // 화면 핸들은 플랫폼별 계약을 따른다. iOS는 CAMetalLayer를 가진 UIView 주소다.
    Example(const void *window, const Options &selected)
        : owner(CreateDevice(selected.validation)), device(*owner), resources(device),
          params(selected.params), options(selected) {
        SwapchainDesc swap;
        swap.window = window;
        swap.format = ExampleColorFormat;
#if defined(__ANDROID__)
        swap.compositeAlpha=CompositeAlpha::Inherit;
        swap.minimumImageCount=3;
#else
        swap.minimumImageCount=2;
#endif
        swap.presentMode = PresentMode::Fifo;
        swap.allowReadback = !options.capture.empty();
        if (!device.CreateSwapchain(swap))
            throw std::runtime_error("Swapchain creation failed.");
    }

    virtual void Draw(TextureHandle backbuffer) = 0;
    // 멤버는 선언 역순으로 소멸한다. ResourceScope가 장치보다 먼저 정리되도록 순서를 유지한다.
    std::unique_ptr<IDevice> owner;
    IDevice &device;
    ResourceScope resources;
    // WaitIdle 이후 텍스처·리소스 세트가 장치보다 먼저 해제되도록 보관한다.
    std::unique_ptr<ResourceScope> imageResources;
    Params params;
    uint32_t imageWidth = 0, imageHeight = 0;

  private:
    static std::unique_ptr<IDevice> CreateDevice(bool validation) {
        DeviceDesc desc;
        desc.enableValidation = validation;
        std::unique_ptr<IDevice> result(IDevice::Create(desc));
        if (!result)
            throw std::runtime_error("Device creation failed.");
        return result;
    }
    Options options;
    uint64_t frame = 0;
};

std::unique_ptr<Example> CreatePBRLighting(const void *view, int argc, char **argv);
std::unique_ptr<Example> CreateShadowMapping(const void *view, int argc, char **argv);
} // namespace advanced
