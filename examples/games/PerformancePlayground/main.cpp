#include "World.h"
#include "Evidence.h"
#include "Controls.h"
#ifdef PLAYGROUND_TRACY_ENABLED
#include <dyf/Extends/Tracy.h>
#endif
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
using namespace playground;
using namespace dyf;
using namespace dyf::Math;
using Key=Platform::Key;
using Clock=std::chrono::steady_clock;
struct Options {uint32_t frames=0;int station=0;bool validateAssets=false,validation=false;std::filesystem::path captures;};
uint32_t Number(const char* text) {
    uint32_t value=0;const auto end=text+std::char_traits<char>::length(text);const auto result=std::from_chars(text,end,value);
    if(result.ec!=std::errc{}||result.ptr!=end||text==end)throw std::runtime_error("Expected an unsigned integer");return value;
}
std::filesystem::path ExecutableDirectory() {
    std::wstring path(32768,L'\0');const auto count=GetModuleFileNameW(nullptr,path.data(),DWORD(path.size()));
    if(!count||count>=path.size())throw std::runtime_error("Executable path unavailable");path.resize(count);return std::filesystem::path(path).parent_path();
}
void Text(Canvas& canvas,Font& font,std::string_view text,float x,float y,float4 color={.9f,.94f,.96f,1}) {
    if(!canvas.Text(font,text,{x,y},color))throw std::runtime_error("HUD text failed");
}
int Zone(Point p) {
    if(p.x<-7&&p.y>8&&p.y<28) return 2;
    if(p.x>6&&p.y>26) return 3;
    if(p.x>7&&p.y>-9&&p.y<26) return 1;
    return 0;
}
void Hud(Canvas& canvas,Font& font,Font& title,uint32_t width,uint32_t height,const Controller& player,const World& world,const Renderer& renderer,int zone,bool shake,bool locked,bool autoWalk,const std::string& notice) {
    const float w=float(width),h=float(height);const float4 panel={.035f,.06f,.085f,.93f},cyan={.26f,.87f,.87f,1},gold={1,.73f,.32f,1},muted={.58f,.69f,.76f,1};
    canvas.FillRect({20,20,460,112},panel);canvas.FillRect({20,20,4,112},cyan);
    Text(canvas,font,"DY_ENGINE  /  NATIVE RENDER WALK",38,31,cyan);
    Text(canvas,title,"FOUNDRY DISTRICT",36,54);
    Text(canvas,font,Stations[zone].name,38,102,gold);
    canvas.FillRect({w-326,20,306,246},panel);
    Text(canvas,font,"RENDER OBSERVATIONS",w-308,32,cyan);
    const auto& stats=renderer.GetLastRenderStatistics();const auto& timing=renderer.GetProfilerTimingSnapshot();char text[220];
    std::snprintf(text,sizeof(text),"Main draws    %u   /   instances %u",stats.mainDrawCount,stats.mainInstanceCount);Text(canvas,font,text,w-308,61);
    std::snprintf(text,sizeof(text),"Frustum culled    %u / %u",stats.culledEntityCount,world.scene.GetEntityCount());Text(canvas,font,text,w-308,86);
    std::snprintf(text,sizeof(text),"CPU render   %.2f ms",timing.cpuRenderAverageMilliseconds);Text(canvas,font,text,w-308,111);
    if(timing.hasGpuMain)std::snprintf(text,sizeof(text),"GPU main     %.2f ms",timing.gpuMainAverageMilliseconds);else std::snprintf(text,sizeof(text),"GPU main     Pending / unavailable");Text(canvas,font,text,w-308,136);
    if(timing.hasGpuShadow)std::snprintf(text,sizeof(text),"GPU shadow   %.2f ms",timing.gpuShadowAverageMilliseconds);else std::snprintf(text,sizeof(text),"GPU shadow   Pending / disabled");Text(canvas,font,text,w-308,161);
    std::snprintf(text,sizeof(text),"Frame %.1f ms   /   %.0f fps",timing.frameAverageMilliseconds,timing.fps);Text(canvas,font,text,w-308,186);
    Text(canvas,font,"Main counts exclude shadow + HUD",w-308,216,muted);
    Text(canvas,font,"No occlusion / GPU culling in this path",w-308,239,gold);

    canvas.FillRect({20,h-280,202,157},panel);
    Text(canvas,font,"COMPARISON ROUTE",34,h-273,cyan);
    const auto mapPoint=[&](Point p){return float2{120+p.x*2.f,h-205-p.y*1.35f};};
    const auto a=mapPoint(Stations[0].position),b=mapPoint(Stations[1].position),c=mapPoint(Stations[2].position),d=mapPoint(Stations[3].position);
    canvas.Line(a,{a.x,b.y},muted,2);canvas.Line({a.x,b.y},b,muted,2);canvas.Line({a.x,c.y},c,muted,2);canvas.Line({a.x,b.y},{a.x,d.y},muted,2);canvas.Line({a.x,d.y},d,muted,2);
    for(int i=0;i<4;++i) {const auto p=mapPoint(Stations[i].position);canvas.FillCircle(p,9,i==zone?gold:float4{.18f,.38f,.45f,1});Text(canvas,font,std::to_string(i+1),p.x-4,p.y-9);}
    const auto p=mapPoint(player.position);canvas.FillCircle(p,4,{1,1,1,1});canvas.Line(p,{p.x+std::sin(player.yaw)*12,p.y-std::cos(player.yaw)*12},cyan,2);
    const auto& cfg=renderer.GetAppliedConfig();
    canvas.FillRect({240,h-180,w-260,57},panel);
    std::snprintf(text,sizeof(text),"I  Instancing %s     C  Frustum %s     L  Lighting %s     H  Shadows %s",cfg.enableInstancing?"ON":"OFF",cfg.enableFrustumCulling?"ON":"OFF",cfg.lighting.enabled?"ON":"OFF",cfg.lighting.shadows?"ON":"OFF");Text(canvas,font,text,256,h-172);
    std::snprintf(text,sizeof(text),"P  Parallel %s     V  VSync %s     B  Camera bob %s     %s",cfg.enableParallelRenderGraph?"ON":"OFF",cfg.vsync?"ON":"OFF",shake?"ON":"OFF",player.sprinting?"RUNNING":player.speed>.1f?"WALKING":"STANDING");Text(canvas,font,text,256,h-146,gold);
    canvas.FillRect({20,h-109,w-40,89},panel);
    Text(canvas,font,"WASD / arrows  Move     Shift  Run     Mouse drag  Orbit     Wheel  Zoom     Tab  Capture mouse     Esc  Release / quit",36,h-98);
    Text(canvas,font,"1-4  Return to comparison pads     5  Hall entrance     R  Auto-walk     F9  Capture     F10  Evidence report",36,h-72);
    std::snprintf(text,sizeof(text),"%s  |  x %.1f  y %.1f  |  %.1f m/s  |  %s%s",Stations[zone].detail,player.position.x,player.position.y,player.speed,locked?"Mouse captured":"Drag mouse to look",autoWalk?" / AUTO-WALK":"");Text(canvas,font,text,36,h-46,muted);
    if(!notice.empty()) {canvas.FillRect({w*.5f-280,145,560,37},panel);Text(canvas,font,notice,w*.5f-264,153,gold);}
    canvas.Line({w/2-6,h/2},{w/2+6,h/2},{1,1,1,.55f});canvas.Line({w/2,h/2-6},{w/2,h/2+6},{1,1,1,.55f});
}
}

int main(int argc,char** argv) try {
#ifdef PLAYGROUND_TRACY_ENABLED
    TracyAdapter tracy;
#endif
    Options options;
    for(int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if(arg=="--help") {std::puts("PerformancePlayground [--frames N] [--station 1..4] [--capture-dir PATH] [--validate-assets] [--validation]");return 0;}
        if(arg=="--validate-assets")options.validateAssets=true;
        else if(arg=="--validation")options.validation=true;
        else if((arg=="--frames"||arg=="--station"||arg=="--capture-dir")&&i+1<argc) {
            const auto value=argv[++i];if(arg=="--frames")options.frames=Number(value);
            else if(arg=="--capture-dir")options.captures=value;
            else {const auto station=Number(value);if(station<1||station>4)throw std::runtime_error("Station must be 1..4");options.station=int(station-1);}
        } else throw std::runtime_error("Unknown/incomplete argument: "+arg);
    }
    const auto folder=ExecutableDirectory();
    World world(folder/"Assets");
    if(options.validateAssets) {std::puts("All external assets, textures, skeleton and clips loaded successfully.");return 0;}
    if(options.captures.empty()) options.captures=folder/"Captures";
    Platform::Window window(1440,900,"dy_engine / FOUNDRY DISTRICT / Performance Playground");
    if(!window.GetHandle())throw std::runtime_error("Window creation failed");
    RHI::DeviceDesc deviceDesc;deviceDesc.enableValidation=options.validation;
    std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create(deviceDesc));
    if(!device)throw std::runtime_error("D3D12 device creation failed");
    RendererConfig config;config.vsync=false;config.enableProfilerHud=false;config.recordingThreadCount=2;
    config.clearColor={.40f,.61f,.72f,1};config.lighting.shadows=true;config.lighting.shadowQuality=ShadowQuality::High;
    config.lighting.ambientColor={.62f,.77f,1};config.lighting.ambientIntensity=.15f;
    config.lighting.environment.diffuseIntensity=.28f;config.lighting.environment.specularIntensity=.1f;
    auto renderer=Renderer::Create(*device,window.GetHandle(),config);if(!renderer)throw std::runtime_error("Renderer creation failed");
    std::wstring windowsPath(32768,L'\0');const auto windowsLength=GetWindowsDirectoryW(windowsPath.data(),UINT(windowsPath.size()));
    if(!windowsLength||windowsLength>=windowsPath.size())throw std::runtime_error("Windows font directory unavailable");
    windowsPath.resize(windowsLength);const auto windows=std::filesystem::path(windowsPath);
    auto font=Font::Load((windows/"Fonts/segoeui.ttf").string(),17);
    auto title=Font::Load((windows/"Fonts/segoeuib.ttf").string(),30);
    if(!font||!title)throw std::runtime_error("Windows Segoe UI font unavailable");
    Controller player;player.position=Stations[options.station].position;player.yaw=Stations[options.station].yaw;player.facing=player.yaw;player.pitch=Stations[options.station].pitch;
    GameplayEvidence gameplay;
    float zoom=4.4f;bool shake=true,locked=false,autoWalk=false;uint32_t frames=0,captures=0;float noticeTime=5;
    std::string notice="Explore four pads. Keys 1-4 restore the same position and camera.";
    auto previous=Clock::now();
    while(window.IsRunning()&&(!options.frames||frames<options.frames)) {
        window.PollEvents();if(!window.IsRunning())break;
        const auto now=Clock::now();const float dt=std::min(.05f,std::chrono::duration<float>(now-previous).count());previous=now;
        const auto& input=window.GetInput();
        if(!window.HasFocus()) {autoWalk=false;if(locked){window.SetCursorMode(Platform::CursorMode::Normal);locked=false;}}
        if(input.WasPressed(Key::Escape)) {if(locked){locked=false;window.SetCursorMode(Platform::CursorMode::Normal);}else {window.RequestClose();break;}}
        if(input.WasPressed(Key::Tab)) {locked=!locked;window.SetCursorMode(locked?Platform::CursorMode::Locked:Platform::CursorMode::Normal);if(locked)(void)window.SetRawMouseMotion(true);}
        for(int i=0;i<4;++i) if(input.WasPressed(static_cast<Key>(int(Key::Num1)+i))) {
            player.position=Stations[i].position;player.yaw=Stations[i].yaw;player.facing=player.yaw;player.pitch=Stations[i].pitch;player.phase=0;zoom=4.4f;autoWalk=false;
        }
        if(input.WasPressed(Key::Num5)) {player.position={-18,6.5f};player.yaw=0;player.facing=0;player.pitch=.12f;zoom=4.4f;autoWalk=false;}
        if(input.WasPressed(Key::B))shake=!shake;
        if(input.WasPressed(Key::R))autoWalk=!autoWalk;
        const auto toggle=[&](Key key,const char* name){if(input.WasPressed(key)){notice=RequestToggle(*renderer,name)?std::string(name)+" requested":"Settings request rejected";noticeTime=3;}};
        toggle(Key::I,"instancing");toggle(Key::C,"culling");
        toggle(Key::L,"lighting");toggle(Key::H,"shadows");
        toggle(Key::P,"parallelRecording");toggle(Key::V,"vsync");
        if(window.HasFocus()&&(locked||input.IsDown(Platform::MouseButton::Left)||input.IsDown(Platform::MouseButton::Right))) {
            const auto motion=input.GetCursorDelta();player.yaw+=float(motion.x)*.004f;gameplay.mouseRadians+=std::abs(float(motion.x)*.004f);player.pitch=std::clamp(player.pitch+float(motion.y)*.003f,-.15f,.95f);
        }
        zoom=std::clamp(zoom-float(input.GetScrollDelta().y)*.4f,1.5f,7.f);
        const auto down=[&](Key key){return input.IsDown(key)||input.WasPressed(key);};
        const float right=float(down(Key::D)||down(Key::Right))-float(down(Key::A)||down(Key::Left));
        const float forward=float(down(Key::W)||down(Key::Up)||autoWalk)-float(down(Key::S)||down(Key::Down));
        for(int i=0;i<4;++i)if(down(std::array<Key,4>{Key::W,Key::A,Key::S,Key::D}[i]))gameplay.movementKeys|=1u<<i;
        const auto beforeMove=player.position;
        player.Move(window.HasFocus()?right:0,window.HasFocus()?forward:0,down(Key::LeftShift)||down(Key::RightShift),dt,world.solids);
        const float moved=Length(player.position-beforeMove);
        (player.sprinting?gameplay.runMeters:gameplay.walkMeters)+=moved;
        if(beforeMove.y<9&&player.position.y>=9&&player.position.x>-19.5f&&player.position.x<-16.5f)gameplay.walkedThroughDoor=true;
        world.AnimateCharacter(player,dt);
        const auto size=window.GetFramebufferSize();
        if(size.width<=0||size.height<=0||window.IsMinimized()) {window.WaitEvents(.05);continue;}
        const float bob=shake&&player.speed>.1f?std::sin(player.phase)*(player.sprinting?.075f:.027f):0;
        gameplay.maxBob=std::max(gameplay.maxBob,std::abs(bob));
        const float sway=shake&&player.speed>.1f?std::cos(player.phase*.5f)*(player.sprinting?.035f:.012f):0;
        const float3 target={player.position.x,player.position.y,1.2f+bob};
        const float3 desired={target.x-std::sin(player.yaw)*std::cos(player.pitch)*zoom,target.y-std::cos(player.yaw)*std::cos(player.pitch)*zoom,target.z+std::sin(player.pitch)*zoom};
        const float fraction=CameraFraction({target.x,target.y,target.z},{desired.x,desired.y,desired.z},world.solids);
        const auto eye=target+(desired-target)*fraction;
        Camera camera;if(!camera.LookAt(eye,target+float3{std::cos(player.yaw)*sway,-std::sin(player.yaw)*sway,0})||!camera.SetPerspective(float(size.width)/size.height,1.05f,.06f,160))throw std::runtime_error("Camera setup failed");
        Canvas overlay(uint32_t(size.width),uint32_t(size.height));noticeTime-=dt;
        const int zone=Zone(player.position);
        Hud(overlay,*font,*title,uint32_t(size.width),uint32_t(size.height),player,world,*renderer,zone,shake,locked,autoWalk,noticeTime>0?notice:"");
        const bool capture=input.WasPressed(Key::F9),report=input.WasPressed(Key::F10);
        if(capture&&!renderer->SetReadbackEnabled(true))throw std::runtime_error("Readback enable failed");
        Image readback;if(!renderer->Render(world.scene,camera,overlay,capture?&readback:nullptr))throw std::runtime_error("Render failed");
        if(capture||report) {
            std::filesystem::create_directories(options.captures);
            const auto name="station-"+std::to_string(zone+1)+"-"+std::to_string(++captures);
            if(capture) {SavePpm(options.captures/(name+".ppm"),readback);if(!renderer->SetReadbackEnabled(false))throw std::runtime_error("Readback restore failed");}
            SaveEvidence(options.captures/(name+".json"),player,world,*renderer,zone,gameplay);
            notice=capture?"Saved PPM + JSON. Capture readback affects this frame's timing.":"Saved renderer timing snapshot and applied settings to JSON.";noticeTime=4;
        }
        ++frames;
        // Keep the desktop responsive; this cap is outside Renderer timing and is part of frame cadence.
        std::this_thread::sleep_until(now+std::chrono::milliseconds(8));
    }
    std::filesystem::create_directories(options.captures);
    SaveEvidence(options.captures/"session.json",player,world,*renderer,Zone(player.position),gameplay);
    std::printf("EXIT: frames=%u position=(%.3f,%.3f) distance=%.3fm\n",frames,player.position.x,player.position.y,player.distance);
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"PerformancePlayground: %s\n",e.what());return 1;}
