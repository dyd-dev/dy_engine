#pragma once
#include "RendererMotionSupport.h"
#include <dyf/Canvas.h>

namespace CanvasPerformance
{
using namespace RendererPerformance;
inline dyf::Image Solid(uint8_t r, uint8_t g, uint8_t b)
{ return dyf::Image(1,1,{r,g,b,255}); }
inline dyf::Image Icon(unsigned variant)
{
    constexpr unsigned size=64;
    std::vector<uint8_t> pixels(size*size*4);
    for(unsigned y=0;y<size;++y) for(unsigned x=0;x<size;++x)
    {
        const bool bright=((x/8+y/8)%2)==0;
        const size_t p=(y*size+x)*4;
        pixels[p]=variant ? 230 : 55;
        pixels[p+1]=bright ? 200 : 80;
        pixels[p+2]=variant ? 45 : 235;
        pixels[p+3]=bright ? 230 : 150;
    }
    return dyf::Image(size,size,std::move(pixels));
}
struct Workload
{
    MovingWorkload scene;
    dyf::Camera camera;
    dyf::Canvas canvas;
    std::array<dyf::Image,2> icons={Icon(0),Icon(1)};
    uint32_t size, tick=0;
    Workload(uint32_t count,uint32_t extent)
        : scene(count),camera(scene.Camera()),canvas(extent,extent),size(extent) {}
    void Update(uint32_t next)
    {
        tick=next; scene.Update(next);
        canvas.Clear({0,0,0,1});
        const float t=float(next)/60.f;
        for(unsigned i=0;i<10;++i)
        {
            const float x=size*(.06f+.175f*float(i%5)+.018f*std::sin(t*2+float(i)));
            const float y=size*(.34f+.14f*float(i/5)+.018f*std::cos(t*1.7f+float(i)));
            canvas.Image(icons[i%2],{x,y,size*.09f,size*.09f},{1,1,1,.85f});
        }
    }
};
struct SubmissionCounts { uint64_t submitted=0,skipped=0; };
inline void Pump()
{
    MSG message{};
    while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE))
    { TranslateMessage(&message); DispatchMessageW(&message); }
}
// Frame acquisition is allowed to skip, especially on Vulkan. A skipped call
// never advances simulation and never counts as a submitted timing sample.
inline void Submit(Context& context,const Workload& workload,SubmissionCounts& counts,
    dyf::Image* capture=nullptr)
{
    for(unsigned attempt=0;attempt<64;++attempt)
    {
        const auto previous=context.renderer->GetLastRenderCompletion();
        Check(context.renderer->Render(workload.scene,workload.camera,workload.canvas,capture),
            "Moving Scene+Canvas render failed");
        const auto current=context.renderer->GetLastRenderCompletion();
        if(current && current!=previous)
        {
            Check(!capture || capture->IsValid(),"Submitted Canvas capture has no pixels");
            ++counts.submitted; return;
        }
        Check(!capture || !capture->IsValid(),"Skipped frame returned stale pixels");
        ++counts.skipped;
        Check(!context.device->IsLost(),"Device lost while Canvas frame acquisition skipped");
        Pump(); Sleep(2);
    }
    throw std::runtime_error("Canvas frame acquisition remained skipped after bounded retries");
}
inline std::vector<uint8_t> Capture(Context& context,const Workload& workload,SubmissionCounts& counts)
{
    dyf::Image image;
    Submit(context,workload,counts,&image);
    Check(image.GetWidth()==workload.size && image.GetHeight()==workload.size,
        "Canvas capture dimensions differ from requested window size");
    NonBlank(image.GetPixels());
    return image.GetPixels();
}
inline uint64_t RegionChanged(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b,
    uint32_t size,uint32_t firstY,uint32_t lastY)
{
    Check(a.size()==size_t(size)*size*4 && b.size()==a.size(),"Region capture size mismatch");
    uint64_t changed=0;
    for(unsigned y=firstY;y<lastY;++y) for(unsigned x=0;x<size;++x)
    {
        const size_t p=(size_t(y)*size+x)*4;
        changed+=std::abs(int(a[p])-int(b[p]))+std::abs(int(a[p+1])-int(b[p+1]))+
            std::abs(int(a[p+2])-int(b[p+2]))>6;
    }
    return changed;
}
}
