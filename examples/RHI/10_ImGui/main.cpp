#include "dyf/ImGui.h"
#include "dyf/Platform/Window.h"
#include "dyf/RHI.h"
#include <imgui.h>
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using Clock=std::chrono::steady_clock;
using namespace dyf;

static bool Capture(RHI::IDevice& device,RHI::TextureHandle target,const std::string& path)
{
    RHI::TextureReadback result;
    if(!device.ReadTexture(target,result)) return false;
    std::ofstream out(path,std::ios::binary);
    out<<"P6\n"<<result.width<<" "<<result.height<<"\n255\n";
    const bool bgra=result.format==RHI::Format::B8G8R8A8_UNORM || result.format==RHI::Format::B8G8R8A8_UNORM_SRGB;
    for(uint32_t y=0;y<result.height;++y) for(uint32_t x=0;x<result.width;++x)
    {
        const auto* p=result.pixels.data()+y*result.rowPitch+x*4;
        const char rgb[]={static_cast<char>(p[bgra?2:0]),static_cast<char>(p[1]),static_cast<char>(p[bgra?0:2])};
        out.write(rgb,3);
    }
    return out.good();
}

int main(int argc,char** argv)
{
    bool selfTest=false,baseline=false;
    int maxFrames=0;
    std::string capturePath;
    for(int i=1;i<argc;++i)
    {
        if(std::strcmp(argv[i],"--self-test")==0) {selfTest=true;maxFrames=100;}
        else if(std::strcmp(argv[i],"--baseline")==0) baseline=true;
        else if(std::strncmp(argv[i],"--frames=",9)==0)
        {
            const char* value=argv[i]+9;
            const auto parsed=std::from_chars(value,value+std::strlen(value),maxFrames);
            if(parsed.ec!=std::errc{} || *parsed.ptr || maxFrames<=0)
            {std::fprintf(stderr,"--frames requires a positive integer.\n");return 1;}
        }
        else if(std::strncmp(argv[i],"--capture=",10)==0) capturePath=argv[i]+10;
        else {std::fprintf(stderr,"Unknown option: %s\n",argv[i]);return 1;}
    }
    if(selfTest && baseline) {std::fprintf(stderr,"Choose --self-test or --baseline.\n");return 1;}
    if(selfTest && maxFrames<100) {std::fprintf(stderr,"Self-test requires at least 100 frames.\n");return 1;}
    if(!capturePath.empty() && !maxFrames) maxFrames=120;
    Platform::Window window(1000,700,"dy_engine / ImGui input");
    if(!window.GetHandle()) return 1;
    RHI::DeviceDesc deviceDesc; deviceDesc.enableValidation=selfTest;
    std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(deviceDesc));
    if(!device) return 1;
    RHI::SwapchainDesc swapchain;
    swapchain.window=window.GetHandle();swapchain.format=RHI::Format::B8G8R8A8_UNORM;
    swapchain.allowReadback=!capturePath.empty();swapchain.presentMode=RHI::PresentMode::Immediate;
    if(!device->CreateSwapchain(swapchain)) return 1;
    auto gui=baseline ? std::unique_ptr<Gui>{} : Gui::Create(window,*device);
    if(!baseline && !gui) return 1;
    if(gui)
    {
        ImGui::GetIO().IniFilename=nullptr;
#ifdef _WIN32
        ImFontConfig fontConfig; fontConfig.OversampleH=fontConfig.OversampleV=1;
        ImGui::GetIO().Fonts->AddFontFromFileTTF("C:/Windows/Fonts/malgun.ttf",18,&fontConfig,ImGui::GetIO().Fonts->GetGlyphRangesKorean());
#endif
    }
    int clicks=0,appPresses=0,textChanges=0,capturedKeys=0;
    float value=0.35f;
    char text[256]="";
    ImVec2 buttonPoint{},textPoint{},sliderPoint{};
    uint64_t receivedEvents=0;
    bool largeMeshVerified=false,mouseCaptureVerified=false,outsideMouseVerified=false;
    std::vector<double> cpuTimes,frameTimes,guiTimes,submitTimes;
    GuiStats stats{};
    for(int frame=0;window.IsRunning() && (!maxFrames || frame<maxFrames);++frame)
    {
        const auto frameStart=Clock::now();
        window.PollEvents();
        if(selfTest)
        {
            auto* h=window.GetGlfwHandle();
            // Deterministic callback injection, separate from physical-device testing.
            ImGui_ImplGlfw_CursorEnterCallback(h,true);
            ImVec2 point=frame<18 ? buttonPoint : frame<38 ? textPoint : frame<48 ? sliderPoint : ImVec2(900,620);
            ImGui_ImplGlfw_CursorPosCallback(h,point.x,point.y);
            if(frame==10 || frame==20 || frame==40 || frame==50) ImGui_ImplGlfw_MouseButtonCallback(h,GLFW_MOUSE_BUTTON_LEFT,GLFW_PRESS,0);
            if(frame==12 || frame==22 || frame==42 || frame==52) ImGui_ImplGlfw_MouseButtonCallback(h,GLFW_MOUSE_BUTTON_LEFT,GLFW_RELEASE,0);
            if(frame==25) ImGui_ImplGlfw_CharCallback(h,'a');
            if(frame==26) ImGui_ImplGlfw_CharCallback(h,0xD55C);
            if(frame==30 || frame==50) ImGui_ImplGlfw_KeyCallback(h,GLFW_KEY_W,0,GLFW_PRESS,0);
            if(frame==32 || frame==52) ImGui_ImplGlfw_KeyCallback(h,GLFW_KEY_W,0,GLFW_RELEASE,0);
            if(frame==65) glfwSetWindowSize(h,1100,760);
            if(frame==75) glfwSetWindowSize(h,1000,700);
            window.PollEvents();
        }
        const auto& input=window.GetInput();
        receivedEvents+=input.GetEvents().size();
        const auto guiStart=Clock::now();
        if(gui)
        {
            if(!gui->BeginFrame()) return 1;
            if(selfTest && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                if(frame==10) mouseCaptureVerified=gui->WantsMouse();
                if(frame==50) outsideMouseVerified=!gui->WantsMouse();
            }
            // ImGui may spread one poll over several frames. Match capture to its consumed key edge.
            if(ImGui::IsKeyPressed(ImGuiKey_W,false))
            {
                if(gui->WantsKeyboard()) ++capturedKeys;
                else ++appPresses;
            }
            ImGui::SetNextWindowPos({24,24},ImGuiCond_Always);
            ImGui::SetNextWindowSize({660,580},ImGuiCond_Always);
            ImGui::Begin("Mouse, keyboard and events",nullptr,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse);
            ImGui::TextUnformatted("dy_engine / GLFW 3.4 / Dear ImGui");
            ImGui::Separator();
            if(ImGui::Button("Receive click",{180,36})) ++clicks;
            buttonPoint=ImGui::GetItemRectMin();buttonPoint.x+=70;buttonPoint.y+=18;
            ImGui::SameLine();ImGui::Text("Button events: %d",clicks);
            ImGui::SliderFloat("Value",&value,0,1);
            sliderPoint=ImGui::GetItemRectMin();sliderPoint.x+=100;sliderPoint.y+=10;
            if(ImGui::InputText("Text (UTF-8)",text,sizeof(text))) ++textChanges;
            textPoint=ImGui::GetItemRectMin();textPoint.x+=50;textPoint.y+=10;
            ImGui::Text("Text changes: %d",textChanges);
            ImGui::Text("Application W presses: %d / UI captured: %d",appPresses,capturedKeys);
            ImGui::TextUnformatted("Click outside this panel, then press W for an application event.");
            ImGui::TextUnformatted("Use Tab / arrows / Enter for keyboard navigation.");
            ImGui::Separator();
            const auto p=input.GetCursorPosition(),d=input.GetCursorDelta(),s=input.GetScrollDelta();
            ImGui::Text("Mouse %.0f, %.0f   Delta %.0f, %.0f   Wheel %.1f",p.x,p.y,d.x,d.y,s.y);
            ImGui::Text("Capture mouse: %s   keyboard: %s",gui->WantsMouse()?"yes":"no",gui->WantsKeyboard()?"yes":"no");
            ImGui::Text("Raw events this poll: %zu   total: %llu",input.GetEvents().size(),static_cast<unsigned long long>(receivedEvents));
            ImGui::Text("Input memory: %zu bytes + event capacity",sizeof(Platform::Input));
            ImGui::Separator();
            ImGui::Text("Previous frame: %u vertices / %u indices / %u draws",stats.vertices,stats.indices,stats.drawCalls);
            ImGui::Text("Upload: %llu bytes   Font atlas: %llu bytes",static_cast<unsigned long long>(stats.uploadBytes),static_cast<unsigned long long>(stats.atlasBytes));
            ImGui::TextUnformatted("Raw input remains available while UI owns interaction.");
            if(selfTest && frame==85)
            {
                auto* list=ImGui::GetWindowDrawList();
                for(int i=0;i<17000;++i)
                    list->AddRectFilled({30.f+(i%100)*2,500.f},{31.f+(i%100)*2,501.f},IM_COL32_WHITE);
            }
            ImGui::End();
            gui->EndFrame();
        }
        double guiMilliseconds=std::chrono::duration<double,std::milli>(Clock::now()-guiStart).count();
        // Acquisition is nonblocking. Keep this GUI frame until a backbuffer is ready.
        const auto acquireStart=Clock::now();
        while(!device->BeginFrame())
        {
            if(device->IsLost() || (maxFrames && Clock::now()-acquireStart>std::chrono::seconds(10))) return 1;
            if(!window.IsRunning()) return selfTest ? 1 : 0;
            glfwWaitEventsTimeout(0.001);
        }
        RHI::ResourceScope resources(*device);
        auto* commands=resources.Keep(device->AcquireCommandList());
        auto* target=device->GetBackBuffer();
        if(!commands || !target) return 1;
        const RHI::ResourceBarrierDesc begin{nullptr,target,RHI::ResourceState::Present,RHI::ResourceState::RenderTarget,{}};
        commands->ResourceBarrier(&begin,1);
        RHI::ColorAttachment color; color.texture=target;color.loadOp=RHI::LoadOp::Clear;color.storeOp=RHI::StoreOp::Store;
        color.clearColor[0]=0.035f;color.clearColor[1]=0.065f;color.clearColor[2]=0.1f;color.clearColor[3]=1;
        commands->BeginRendering({&color,1,nullptr});commands->EndRendering();
        const RHI::ResourceBarrierDesc end{nullptr,target,RHI::ResourceState::RenderTarget,RHI::ResourceState::Present,{}};
        commands->ResourceBarrier(&end,1);
        const auto recordStart=Clock::now();
        if(gui && !gui->Record(*commands,target)) return 1;
        guiMilliseconds+=std::chrono::duration<double,std::milli>(Clock::now()-recordStart).count();
        const auto guiStop=Clock::now();
        if(gui) stats=gui->GetStats();
        if(selfTest && frame==85) largeMeshVerified=stats.vertices>65535 && stats.drawCalls>1;
        const auto submitStart=Clock::now();
        if(!commands->Close() || !device->Submit(&commands,1)) return 1;
        const double submitMilliseconds=std::chrono::duration<double,std::milli>(Clock::now()-submitStart).count();
        if(!capturePath.empty() && frame==maxFrames-1 && !Capture(*device,target,capturePath)) return 1;
        if(!device->Present()) return 1;
        if(frame>=10)
        {
            cpuTimes.push_back(std::chrono::duration<double,std::milli>(guiStop-guiStart).count());
            frameTimes.push_back(std::chrono::duration<double,std::milli>(Clock::now()-frameStart).count());
            guiTimes.push_back(guiMilliseconds);
            submitTimes.push_back(submitMilliseconds);
        }
    }
    if(!device->WaitIdle()) return 1;
    if(selfTest && (clicks!=1 || std::strcmp(text,"a한")!=0 || textChanges<2 || capturedKeys!=1 || appPresses!=1 || value==0.35f || !largeMeshVerified || !mouseCaptureVerified || !outsideMouseVerified))
    {
        std::fprintf(stderr,"FAIL clicks=%d text=%s changes=%d captured=%d app=%d value=%f\n",clicks,text,textChanges,capturedKeys,appPresses,value);return 1;
    }
    if(selfTest)
    {
        gui.reset();
        auto* h=window.GetGlfwHandle();
        const auto callback=glfwSetKeyCallback(h,nullptr);
        glfwSetKeyCallback(h,callback);
        if(!callback) return 1;
        callback(h,GLFW_KEY_X,0,GLFW_PRESS,0);
        window.PollEvents();
        if(!window.GetInput().WasPressed(Platform::Key::X)) return 1;
        std::puts("PASS mouse capture inside/outside; Window callbacks restored after Gui destruction.");
    }
    std::sort(cpuTimes.begin(),cpuTimes.end());std::sort(frameTimes.begin(),frameTimes.end());std::sort(guiTimes.begin(),guiTimes.end());
    std::sort(submitTimes.begin(),submitTimes.end());
    if(!cpuTimes.empty()) std::printf("RESULT mode=%s frames=%zu gui_cpu_p50_ms=%.4f gui_cpu_p95_ms=%.4f prepare_p50_ms=%.4f submit_p50_ms=%.4f frame_p50_ms=%.4f vertices=%u indices=%u draws=%u upload_bytes=%llu atlas_bytes=%llu clicks=%d text=%s app=%d captured=%d large_mesh=%d\n",
        baseline?"baseline":selfTest?"self-test":"gui",cpuTimes.size(),guiTimes[guiTimes.size()/2],guiTimes[guiTimes.size()*95/100],cpuTimes[cpuTimes.size()/2],submitTimes[submitTimes.size()/2],frameTimes[frameTimes.size()/2],stats.vertices,stats.indices,stats.drawCalls,static_cast<unsigned long long>(stats.uploadBytes),static_cast<unsigned long long>(stats.atlasBytes),clicks,text,appPresses,capturedKeys,largeMeshVerified);
    return 0;
}
