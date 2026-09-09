#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include "Graphics/Texture.h"
#include "RHI/IDevice.h"
#include "RHI/Readback.h"

namespace dy::Graphics::Private
{
    inline void CaptureFrame(RHI::IDevice& device, TextureAsset* image)
    {
        if (!image) return;
        RHI::TextureReadback readback;
        if (!device.ReadTexture(device.GetBackBuffer(), readback))
            throw std::runtime_error("Frame readback failed; enable RendererDesc::allowReadback on a native backend.");
        const uint64_t rowBytes = static_cast<uint64_t>(readback.width) * 4u;
        const uint64_t size = rowBytes * readback.height;
        if (!RHI::IsReadbackFormat(readback.format) || !readback.width || !readback.height ||
            readback.rowPitch < rowBytes || size > std::numeric_limits<size_t>::max() ||
            static_cast<uint64_t>(readback.height - 1) * readback.rowPitch + rowBytes > readback.pixels.size())
            throw std::runtime_error("RHI returned an invalid readback image.");
        TextureAsset captured;
        captured.width = readback.width;
        captured.height = readback.height;
        captured.rgba8.resize(static_cast<size_t>(size));
        for (uint32_t row = 0; row < readback.height; ++row)
            std::memcpy(captured.rgba8.data() + static_cast<size_t>(row) * rowBytes,
                readback.pixels.data() + static_cast<size_t>(row) * readback.rowPitch, static_cast<size_t>(rowBytes));
        if (readback.format == RHI::Format::B8G8R8A8_UNORM || readback.format == RHI::Format::B8G8R8A8_UNORM_SRGB)
            for (size_t i = 0; i < captured.rgba8.size(); i += 4) std::swap(captured.rgba8[i], captured.rgba8[i + 2]);
        *image = std::move(captured);
    }
}
