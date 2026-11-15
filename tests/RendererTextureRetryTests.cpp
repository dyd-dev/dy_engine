// Current SyncTextures + public RHI, CPU-only returned-failure simulation.
// No real native allocation failure, malformed input, GPU or timing experiment.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#define private public
#include "dyf/Renderer.h"
#undef private
#include "dyf/RendererInternal.h"
#include "dyf/ImageDds.h"
#define main CanvasReuseEntryUnused
#include "CanvasReuseTests.cpp"
#undef main

namespace {
struct TextureDevice final : Device
{
    bool compressedAdvertised=true;
    unsigned compressedRejected=0;
    bool SupportsFormatNative(RHI::Format format,RHI::FormatFeature feature) const noexcept override
    {
        if(format==RHI::Format::BC1_UNORM && feature==RHI::FormatFeature::CompressedSampling)
            return compressedAdvertised;
        return NullDevice::SupportsFormatNative(format,feature);
    }
    RHI::TextureHandle CreateTextureNative(const RHI::TextureDesc& desc) override
    {
        if(desc.format==RHI::Format::BC1_UNORM){++compressedRejected;return nullptr;}
        return NullDevice::CreateTextureNative(desc);
    }
};

dyf::Image Dds()
{
    std::vector<uint8_t> bytes(128+8*3,0);
    const auto put=[&](size_t offset,uint32_t value){std::memcpy(bytes.data()+offset,&value,4);};
    put(0,0x20534444);put(4,124);put(8,0x21007);put(12,4);put(16,4);put(28,3);
    put(76,32);put(80,4);put(84,0x31545844);put(108,0x401008);
    for(size_t block=128;block<bytes.size();block+=8){bytes[block]=0;bytes[block+1]=0xf8;}
    dyf::Image image;Check(dyf::LoadImage(bytes.data(),bytes.size(),image),"Valid DDS fixture failed");
    image.SetColorSpace(dyf::ColorSpace::Linear);
    const auto* original=dyf::Detail::ImageDdsAccess::Get(image);
    Check(original&&original->format==RHI::Format::BC1_UNORM&&original->mips.size()==3,"DDS preservation absent");
    return image;
}

void Run(bool failFirstSubmit)
{
    TextureDevice device;
    dyf::RendererConfig config;config.enableProfilerHud=false;config.texturePreparationThreadCount=1;
    auto renderer=dyf::Renderer::Create(device,reinterpret_cast<void*>(1),config);
    Check(bool(renderer),"Texture probe Renderer create failed");
    auto& state=*renderer->m_impl;
    dyf::MaterialDesc material;material.baseColorTexture=Dds();
    dyf::MeshData mesh;mesh.vertices.resize(3);mesh.indices={0,1,2};
    dyf::Scene scene;Check(bool(scene.Add(mesh,material)),"Scene fixture failed");
    device.failSubmit=failFirstSubmit;
    const bool first=state.SyncTextures(scene,&device);
    Check(state.m_textures.size()==1 && state.m_textures[0].texture,"Expected retained fallback texture");
    auto& slot=state.m_textures[0];
    Check(slot.texture->GetDesc().format==RHI::Format::R8G8B8A8_UNORM && device.compressedRejected==1,"RGBA fallback was not taken");
    if(!failFirstSubmit)
    {
        Check(first && slot.state==RHI::ResourceState::ShaderResource,"Successful RGBA fallback control failed");
        const auto uploads=device.textureUploads;
        Check(state.SyncTextures(scene,&device)&&device.textureUploads==uploads,"Committed fallback was needlessly reuploaded");
        std::puts("PASS RGBA fallback without Submit rejection; committed cache reuse works.");
    }
    else
    {
        Check(!first && slot.state==RHI::ResourceState::Undefined,"First Submit failure did not retain uncommitted RGBA state");
        const bool second=state.SyncTextures(scene,&device);
        Check(second && slot.state==RHI::ResourceState::ShaderResource && device.compressedRejected==1,
            "Retry did not reuse RGBA bytes for the retained fallback texture");
        const auto uploads=device.textureUploads;
        Check(state.SyncTextures(scene,&device) && device.textureUploads==uploads,
            "Successful retry did not commit the texture cache");
        std::puts("PASS retained RGBA fallback retries after Submit failure with compression support unchanged.");
    }
    renderer.reset();Check(device.WaitIdle(),"Texture teardown wait failed");
    const auto counters=device.GetResourceAllocationCounters();
    Check(!counters.textures.live&&!counters.buffers.live&&!counters.pipelines.live,"Texture probe leaked owners");
}
}

int main()
{
    try
    {
        Run(false);Run(true);
        std::puts("PASS current linked texture retry CPU probe; no real native memory failure, GPU, pixel or FPS proof.");
        return 0;
    }
    catch(const std::exception& error){std::fprintf(stderr,"Texture retry probe failed: %s\n",error.what());return 1;}
}
