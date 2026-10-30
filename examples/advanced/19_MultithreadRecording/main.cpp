#include "dyf/RHI.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RHI = dyf::RHI;

int main()
{
    std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));
    RHI::ResourceScope resources(*device);
    constexpr uint32_t width=256, height=256;
    std::array<RHI::TextureHandle,2> textures{};
    for(auto& texture:textures)
    {
        RHI::TextureDesc desc;
        desc.width=width;
        desc.height=height;
        desc.format=RHI::Format::R8G8B8A8_UNORM;
        desc.usage=RHI::TextureUsage::ShaderResource;
        texture=resources.Keep(device->CreateTexture(desc));
    }

    RHI::RenderGraph graph;
    std::array<std::vector<uint8_t>,2> inputs;
    for(uint32_t i=0;i<2;++i)
    {
        const auto texture=graph.ImportTexture(std::to_string(i),textures[i],
            RHI::ResourceState::Undefined,RHI::ResourceState::ShaderResource);
        // 서로 다른 텍스처와 CPU 저장소에만 쓰므로 두 콜백은 독립적으로 기록할 수 있다.
        graph.AddPass("upload-"+std::to_string(i))
            .Write(texture,RHI::ResourceState::CopyDestination)
            .SetExecute([&,i](RHI::ICommandList* commands)
            {
                auto& pixels=inputs[i];
                pixels.resize(width*height*4);
                for(uint32_t y=0;y<height;++y)
                    for(uint32_t x=0;x<width;++x)
                    {
                        auto* pixel=pixels.data()+(y*width+x)*4;
                        pixel[0]=uint8_t(x);
                        pixel[1]=uint8_t(y+i*71);
                        pixel[2]=uint8_t((x^y)+i*113);
                        pixel[3]=255;
                    }
                (void)device->UpdateTexture(*commands,textures[i],0,0,pixels.data(),
                    uint32_t(pixels.size()),width*4,width*height*4);
            });
    }
    (void)graph.Compile();
    std::vector<RHI::ICommandList*> commandLists;
    // RHI 명령과 네이티브 명령의 기록을 두 작업자에게 나눈다.
    (void)graph.ExecuteParallel(device.get(),commandLists,2);
    RHI::FenceHandle completion;
    (void)device->Submit({commandLists.data(),uint32_t(commandLists.size()),nullptr,0},completion);
    (void)device->WaitIdle();
    for(auto* commands:commandLists)
        device->DestroyCommandList(commands);
}
