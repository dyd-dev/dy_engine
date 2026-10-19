#include "ExampleHost.h"
#include "HostedExample.h"
#include "ExampleControls.h"
#include "dyf/Extends/ImGui/ImGui.h"
#include "dyf/RHI/IDevice.h"
#include "dyf/RHI/ICommandList.h"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace dy_examples
{
namespace
{
namespace fs=std::filesystem;
using namespace dyf;
using Clock=std::chrono::steady_clock;
struct Value { bool enabled=false; std::vector<std::string> parts; };
struct Settings { std::vector<Value> values; std::vector<std::string> extra; };
std::unordered_map<std::string,Settings> settings;
std::string overlayLayout;

Settings& Options(const Example& example)
{
    auto [it,inserted]=settings.try_emplace(example.name);
    if(inserted)
        if(const auto* schema=controls::Find(example.name))
            for(std::size_t i=0;i<schema->argCount;++i)
            {
                const auto& arg=schema->args[i];
                Value value; value.enabled=!arg.optional;
                if(arg.kind==controls::ArgumentKind::Flag) value.enabled=std::string_view(arg.defaultValue)=="1";
                std::istringstream stream(arg.defaultValue);
                if(arg.arity==1) value.parts.push_back(arg.defaultValue);
                else for(unsigned p=0;p<arg.arity;++p)
                { std::string part; stream>>part; value.parts.push_back(std::move(part)); }
                it->second.values.push_back(std::move(value));
            }
    return it->second;
}

struct Session
{
    Platform::Window& window;
    const Example* examples;
    std::size_t count;
    const Example& example;
    const Example* configured=nullptr;
    ExampleResult result;
    bool stop=false,frame=false,drawn=false,look=false;
    RHI::IDevice* device=nullptr;
    std::unique_ptr<Gui> gui;
    std::bitset<349> pressed,held,previousHeld;
    Platform::InputPoint lookDelta;
    std::vector<Platform::InputEvent> events;
    Clock::time_point lastPresent=Clock::now();
    double intervalMs=0;

    void FinishGui()
    {
        if(gui && frame) { gui->EndFrame(); frame=false; }
    }
    void ReleaseGui() noexcept
    {
        try
        {
            if(gui)
            {
                FinishGui();
                std::size_t size=0; const char* layout=ImGui::SaveIniSettingsToMemory(&size);
                overlayLayout.assign(layout,size);
                gui.reset();
            }
        }
        catch(...) { gui.reset(); }
        device=nullptr; frame=false;
    }
};
thread_local Session* active=nullptr;
bool CaptureKeyboard() { return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard; }
bool CaptureMouse() { return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse; }

void Swapchain(void* context,RHI::IDevice& device,const RHI::SwapchainDesc& desc)
{
    auto& session=*static_cast<Session*>(context);
    if(desc.window==session.window.GetHandle()) session.device=&device;
}
bool Present(void* context,RHI::IDevice& device)
{
    auto& session=*static_cast<Session*>(context);
    if(session.device!=&device) return true;
    if(session.gui && session.frame)
    {
        session.FinishGui();
        RHI::ResourceScope resources(device);
        auto* commands=resources.Keep(device.AcquireCommandList());
        if(!commands || !session.gui->Record(*commands,device.GetBackBuffer()) ||
            !commands->Close() || !device.Submit(&commands,1)) return false;
    }
    if(CaptureMouse()) session.window.SetCursorMode(Platform::CursorMode::Normal);
    const auto now=Clock::now();
    session.intervalMs=std::chrono::duration<double,std::milli>(now-session.lastPresent).count();
    session.lastPresent=now;
    return true;
}
void Release(void* context,RHI::IDevice& device) noexcept
{
    auto& session=*static_cast<Session*>(context);
    if(session.device==&device) session.ReleaseGui();
}
bool Close(void* context)
{
    static_cast<Session*>(context)->stop=true;
    return true;
}
void Select(Session& session,const Example* next)
{
    session.result.next=next;
    if(next) session.result.arguments=GetExampleArguments(*next);
    session.stop=true;
}
}

void SetExampleArguments(const Example& example,const std::vector<std::string>& arguments)
{
    settings.erase(example.name);
    auto& options=Options(example);
    const auto* schema=controls::Find(example.name);
    if(!schema) { options.extra=arguments; return; }
    for(std::size_t n=0;n<arguments.size();++n)
    {
        bool found=false;
        for(std::size_t i=0;i<schema->argCount;++i)
        {
            const auto& arg=schema->args[i];
            const std::string flag=arg.flag;
            const bool joined=!flag.empty() && flag.back()=='=';
            if(arg.positional || flag.empty() ||
                !(joined?arguments[n].compare(0,flag.size(),flag)==0:arguments[n]==flag)) continue;
            auto& value=options.values[i]; value.enabled=true;
            if(arg.kind!=controls::ArgumentKind::Flag)
            {
                if(joined) value.parts[0]=arguments[n].substr(flag.size());
                else for(unsigned p=0;p<arg.arity && n+1<arguments.size();++p) value.parts[p]=arguments[++n];
            }
            found=true; break;
        }
        if(!found && !arguments[n].empty() && arguments[n][0]!='-')
        {
            // Prefer a named preset, otherwise the custom positional path.
            for(std::size_t i=0;i<schema->argCount && !found;++i)
            {
                const auto& arg=schema->args[i];
                if(!arg.positional) continue;
                bool accepts=arg.kind==controls::ArgumentKind::Path;
                for(std::size_t c=0;c<arg.choiceCount;++c) accepts|=arguments[n]==arg.choices[c].value;
                if(accepts) { auto& value=options.values[i]; value.enabled=true; value.parts[0]=arguments[n]; found=true; }
            }
        }
        if(!found) options.extra.push_back(arguments[n]);
    }
}

std::vector<std::string> GetExampleArguments(const Example& example)
{
    auto& options=Options(example);
    const auto* schema=controls::Find(example.name);
    if(!schema) return options.extra;
    bool capture=false,custom=false;
    for(std::size_t i=0;i<schema->argCount;++i)
    {
        const auto& arg=schema->args[i]; const auto& value=options.values[i];
        if(!value.enabled || value.parts.empty() || value.parts[0].empty()) continue;
        capture|=std::string_view(arg.flag).find("--capture")==0;
        custom|=arg.positional && arg.kind==controls::ArgumentKind::Path;
    }
    std::vector<std::string> result;
    for(std::size_t i=0;i<schema->argCount;++i)
    {
        const auto& arg=schema->args[i]; const auto& value=options.values[i];
        if(!value.enabled) continue;
        if(arg.kind==controls::ArgumentKind::Flag) { result.emplace_back(arg.flag); continue; }
        if(value.parts.empty() || value.parts[0].empty()) continue;
        if(custom && arg.positional && arg.kind!=controls::ArgumentKind::Path) continue;
        if(capture && std::string_view(arg.flag)=="--frames" && value.parts[0]=="0") continue;
        const std::string flag=arg.flag;
        if(!flag.empty() && flag.back()=='=') { result.push_back(flag+value.parts[0]); continue; }
        if(!arg.positional) result.push_back(flag);
        result.insert(result.end(),value.parts.begin(),value.parts.end());
    }
    result.insert(result.end(),options.extra.begin(),options.extra.end());
    return result;
}

void EditExampleArguments(const Example& example)
{
    auto& options=Options(example);
    const auto* schema=controls::Find(example.name);
    if(!schema) { ImGui::TextWrapped("This example has no startup options."); return; }
    ImGui::PushID(example.name);
    if(schema->notes && *schema->notes) ImGui::TextWrapped("%s",schema->notes);
    for(std::size_t i=0;i<schema->argCount;++i)
    {
        ImGui::PushID(static_cast<int>(i));
        const auto& arg=schema->args[i]; auto& value=options.values[i];
        if(arg.kind==controls::ArgumentKind::Flag) ImGui::Checkbox(arg.label,&value.enabled);
        else
        {
            if(arg.optional) { ImGui::Checkbox("##enabled",&value.enabled); ImGui::SameLine(); }
            ImGui::TextUnformatted(arg.label);
            ImGui::BeginDisabled(!value.enabled);
            for(unsigned p=0;p<arg.arity;++p)
            {
                ImGui::PushID(static_cast<int>(p));
                ImGui::SetNextItemWidth(-1);
                if(arg.kind==controls::ArgumentKind::Choice)
                {
                    const char* label=value.parts[p].c_str();
                    for(std::size_t c=0;c<arg.choiceCount;++c)
                        if(value.parts[p]==arg.choices[c].value) label=arg.choices[c].label;
                    if(ImGui::BeginCombo("##value",label))
                    {
                        for(std::size_t c=0;c<arg.choiceCount;++c)
                            if(ImGui::Selectable(arg.choices[c].label,value.parts[p]==arg.choices[c].value))
                                value.parts[p]=arg.choices[c].value;
                        ImGui::EndCombo();
                    }
                }
                else if(arg.kind==controls::ArgumentKind::Path)
                {
                    std::array<char,2048> buffer{};
                    std::copy_n(value.parts[p].data(),std::min(value.parts[p].size(),buffer.size()-1),buffer.data());
                    if(ImGui::InputText("##value",buffer.data(),buffer.size())) value.parts[p]=buffer.data();
                }
                else
                {
                    double number=std::strtod(value.parts[p].c_str(),nullptr);
                    double minimum=arg.minimum,maximum=arg.maximum;
                    if(std::string_view(arg.flag)=="--shadow-lights" && p==0) maximum=4;
                    const bool integer=arg.kind==controls::ArgumentKind::Integer;
                    // +/- work with touch and require no software keyboard.
                    const double step=integer?1.:.05;
                    if(ImGui::InputDouble("##value",&number,step,step*10,integer?"%.0f":"%.4f"))
                    {
                        number=std::clamp(number,minimum,maximum);
                        std::ostringstream formatted; formatted.imbue(std::locale::classic());
                        if(integer) formatted<<static_cast<uint64_t>(number); else formatted<<number;
                        value.parts[p]=formatted.str();
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
    if(!options.extra.empty()) ImGui::TextWrapped("Additional command-line arguments are retained.");
    if(schema->argCount) ImGui::TextWrapped("Startup options take effect with Open / Apply and reload.");
    ImGui::PopID();
}

Platform::Window* ActiveExampleWindow() noexcept { return active?&active->window:nullptr; }
bool ExampleRunning(const Platform::Window& window) noexcept { return window.IsRunning() && (!active || !active->stop); }
void RequestExampleClose(Platform::Window& window) { if(active) active->stop=true; else window.RequestClose(); }
bool CancelExampleClose(Platform::Window& window)
{
    if(active) active->stop=false;
    return window.IsRunning() || window.CancelClose();
}
void CancelExampleNavigation()
{
    if(active) { active->stop=false; active->result.next=nullptr; active->result.arguments.clear(); }
}
bool BeginExampleFrame(Platform::Window& window)
{
    if(!active) return window.IsRunning();
    auto& session=*active;
    session.FinishGui(); session.drawn=false;
    session.previousHeld=session.held; session.held.reset(); session.pressed.reset();
    session.look=false; session.lookDelta={};
    if(!ExampleRunning(window)) return false;
    // Samples with their own Gui draw the host controls in that same context.
    if(!session.gui && session.device && !ImGui::GetCurrentContext())
    {
        session.gui=Gui::Create(window,*session.device);
        if(!session.gui) throw std::runtime_error("Example controls GUI creation failed.");
        auto& io=ImGui::GetIO(); io.ConfigFlags|=ImGuiConfigFlags_DockingEnable; io.IniFilename=nullptr;
        if(const auto* font=std::getenv("DY_EXAMPLES_FONT"))
        {
            std::error_code error;
            if(fs::is_regular_file(fs::u8path(font),error)) io.Fonts->AddFontFromFileTTF(font,16.f);
        }
        ImGui::StyleColorsDark();
        if(!overlayLayout.empty()) ImGui::LoadIniSettingsFromMemory(overlayLayout.data(),overlayLayout.size());
    }
    if(session.gui && session.device && !window.IsMinimized())
    {
        if(!session.gui->BeginFrame())
        { if(session.device->IsLost()) throw std::runtime_error("Example GUI device lost."); return true; }
        session.frame=true; DrawExampleControls();
    }
    return ExampleRunning(window);
}

void DrawExampleControls()
{
    if(!active || active->drawn || !ImGui::GetCurrentContext()) return;
    auto& session=*active; session.drawn=true;
    const auto* viewport=ImGui::GetMainViewport();
    const float width=std::min(390.f,std::max(230.f,viewport->WorkSize.x-24));
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x+12,viewport->WorkPos.y+12),ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(width,std::min(650.f,viewport->WorkSize.y-24)),ImGuiCond_FirstUseEver);
    if(ImGui::Begin("Examples controls"))
    {
        if(ImGui::Button("Back to workbench",ImVec2(-1,36))) Select(session,nullptr);
        ImGui::TextWrapped("%s",session.example.name);
        if(session.intervalMs>0) ImGui::Text("Frame interval %.2f ms / %.1f FPS",session.intervalMs,1000/session.intervalMs);
        ImGui::TextDisabled("Frame interval includes this GUI; CPU/GPU timings belong to the sample.");
        if(ImGui::CollapsingHeader("Live controls",ImGuiTreeNodeFlags_DefaultOpen))
        {
            const auto* schema=controls::Find(session.example.name);
            if(schema && schema->actionCount)
            {
                for(std::size_t i=0;i<schema->actionCount;++i)
                {
                    const auto& action=schema->actions[i]; const auto key=static_cast<std::size_t>(action.key);
                    ImGui::PushID(static_cast<int>(i));
                    const bool clicked=ImGui::Button(action.label,ImVec2(-1,30));
                    if(key<349)
                    {
                        if(action.hold && ImGui::IsItemActive()) session.held.set(key);
                        else if(clicked) session.pressed.set(key);
                    }
                    ImGui::PopID();
                }
                const bool camera=std::any_of(schema->actions,schema->actions+schema->actionCount,
                    [](const auto& action){return action.hold && action.key==Platform::Key::W;});
                if(camera)
                {
                    ImGui::TextUnformatted("Drag here to look");
                    ImGui::Button("Look pad",ImVec2(-1,80));
                    if(ImGui::IsItemActive())
                    { session.look=true; const auto delta=ImGui::GetIO().MouseDelta; session.lookDelta={delta.x,delta.y}; }
                }
            }
            else ImGui::TextWrapped("This sample has no runtime switches. Use startup options below.");
        }
        if(ImGui::CollapsingHeader("Startup options"))
        {
            ImGui::PushID("current");
            EditExampleArguments(session.example);
            if(ImGui::Button("Apply and reload",ImVec2(-1,34))) Select(session,&session.example);
            ImGui::PopID();
        }
        if(ImGui::CollapsingHeader("Choose another example"))
        {
            ImGui::PushID("selected");
            if(!session.configured) session.configured=&session.example;
            if(ImGui::BeginCombo("Example",session.configured->name))
            {
                for(std::size_t i=0;i<session.count;++i)
                    if(ImGui::Selectable(session.examples[i].name,session.configured==&session.examples[i]))
                        session.configured=&session.examples[i];
                ImGui::EndCombo();
            }
            EditExampleArguments(*session.configured);
            if(ImGui::Button("Open example",ImVec2(-1,34))) Select(session,session.configured);
            ImGui::PopID();
        }
        if(session.device && ImGui::CollapsingHeader("Device resources"))
        {
            const auto counters=session.device->GetResourceAllocationCounters();
            const auto upload=session.device->GetUploadMemoryStatistics();
            ImGui::Text("Live buffers %llu / textures %llu / pipelines %llu",
                static_cast<unsigned long long>(counters.buffers.live),static_cast<unsigned long long>(counters.textures.live),
                static_cast<unsigned long long>(counters.pipelines.live));
            ImGui::Text("Upload in use %.2f MiB / peak %.2f MiB",upload.inUseBytes/1048576.,upload.peakInUseBytes/1048576.);
        }
    }
    ImGui::End();
}

bool ExampleAction(Platform::Key key,unsigned kind)
{
    const auto index=static_cast<std::size_t>(key);
    if(!active || index>=349) return false;
    return kind==0?(active->held[index]||active->pressed[index]):kind==1?
        (active->pressed[index] || (active->held[index]&&!active->previousHeld[index])):
        (active->previousHeld[index]&&!active->held[index]);
}
bool ExampleLooking() { return active && active->look; }
bool ExampleKey(const Platform::Window& window,Platform::Key key,unsigned kind)
{
    const auto& input=window.GetInput();
    return ExampleAction(key,kind) || ((!active || !CaptureKeyboard()) &&
        (kind==0?input.IsDown(key):kind==1?input.WasPressed(key):input.WasReleased(key)));
}
bool ExampleButton(const Platform::Window& window,Platform::MouseButton key,unsigned kind)
{
    if(active && active->look && key==Platform::MouseButton::Right) return kind==0;
    const auto& input=window.GetInput();
    return (!active || !CaptureMouse()) && (kind==0?input.IsDown(key):kind==1?input.WasPressed(key):input.WasReleased(key));
}
Platform::InputPoint ExamplePointer(const Platform::Window& window,unsigned kind)
{
    if(active && kind==1 && active->look) return active->lookDelta;
    if(active && kind!=0 && CaptureMouse()) return {};
    const auto& input=window.GetInput();
    return kind==0?input.GetCursorPosition():kind==1?input.GetCursorDelta():input.GetScrollDelta();
}
const std::vector<Platform::InputEvent>& ExampleEvents(const Platform::Window& window)
{
    if(!active) return window.GetInput().GetEvents();
    auto& events=active->events; events.clear();
    for(const auto& event:window.GetInput().GetEvents())
    {
        const bool keyboard=event.type==Platform::InputEventType::Key || event.type==Platform::InputEventType::Text;
        const bool mouse=event.type==Platform::InputEventType::MouseButton || event.type==Platform::InputEventType::Cursor || event.type==Platform::InputEventType::Scroll;
        if((keyboard && CaptureKeyboard()) || (mouse && CaptureMouse())) continue;
        events.push_back(event);
    }
    for(std::size_t key=0;key<349;++key)
        for(unsigned kind=1;kind<=2;++kind)
            if(ExampleAction(static_cast<Platform::Key>(key),kind))
            {
                Platform::InputEvent event; event.type=Platform::InputEventType::Key; event.code=static_cast<int>(key);
                event.action=kind==1?Platform::InputAction::Press:Platform::InputAction::Release; events.push_back(event);
            }
    return events;
}

ExampleResult RunHostedExample(Platform::Window& window,const Example* examples,std::size_t count,
    const Example& example,const std::vector<std::string>& arguments,const fs::path& repositoryRoot)
{
    if(!example.entry) return {77,nullptr,{},std::string(example.name)+": module unavailable"};
    SetExampleArguments(example,arguments);
    Session session{window,examples,count,example};
    const auto previousDirectory=fs::current_path();
    auto directory=fs::path(example.executable).is_absolute()?fs::path(example.executable).parent_path():repositoryRoot/example.executable;
    std::error_code error;
    if(!fs::is_directory(directory,error)) directory=repositoryRoot;
    fs::current_path(directory,error);
    auto* previousSession=active; active=&session;
    const auto previousObserver=RHI::IDevice::SetPresentationObserver({&session,Swapchain,Present,Release});
    const auto previousClose=window.SetCloseRequestHandler({&session,Close});
    struct Restore
    {
        Session& session; Session* previous; fs::path directory;
        RHI::DevicePresentationObserver observer; Platform::Window::CloseRequestHandler close;
        ~Restore()
        {
            session.ReleaseGui(); RHI::IDevice::SetPresentationObserver(observer);
            session.window.SetCloseRequestHandler(close); session.window.SetCursorMode(Platform::CursorMode::Normal);
            active=previous; std::error_code ignored; fs::current_path(directory,ignored);
        }
    } restore{session,previousSession,previousDirectory,previousObserver,previousClose};
    std::vector<std::string> values{example.name}; values.insert(values.end(),arguments.begin(),arguments.end());
    std::vector<char*> pointers; for(auto& value:values) pointers.push_back(value.data()); pointers.push_back(nullptr);
    try { session.result.code=example.entry(static_cast<int>(values.size()),pointers.data()); }
    catch(const std::exception& exception) { session.result.code=1; session.result.message=exception.what(); }
    if(session.result.message.empty() && session.result.code)
        session.result.message=std::string(example.name)+(session.result.code==77?": unsupported on this device":": exited with code "+std::to_string(session.result.code));
    return std::move(session.result);
}
}
