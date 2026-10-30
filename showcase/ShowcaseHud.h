#pragma once

#include "ShowcaseHudInteraction.h"
#include "dyf/Canvas.h"
#include "dyf/Font.h"
#include "dyf/Platform/Window.h"
#include "dyf/Renderer.h"
#include <array>
#include <bitset>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

namespace showcase
{
enum class ControlGroup { Scene, Render, Camera };
struct Control
{
    dyf::Platform::Key key;
    const char* label;
    bool active = false;
    bool enabled = true;
    ControlGroup group = ControlGroup::Scene;
    bool held = false;
};

// Showcase presentation only. Measurement and renderer configuration remain with
// Renderer; the same controls work with a mouse, a touchscreen, or a keyboard.
class ShowcaseHud
{
public:
    explicit ShowcaseHud(std::string fontPath = {}) : m_fontPath(std::move(fontPath))
    {
        if(!m_fontPath.empty()) return;
        const std::array<const char*,16> paths = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
            "C:/Windows/Fonts/segoeui.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/HelveticaNeue.ttc",
            "/System/Library/Fonts/Core/Helvetica.ttc", "/System/Library/Fonts/CoreUI/SFUI.ttf",
            "/System/Library/Fonts/CoreUI/SFUIText.ttf", "/system/fonts/Roboto-Regular.ttf",
            "/system/fonts/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
            "C:/Windows/Fonts/arial.ttf", "/system/fonts/RobotoStatic-Regular.ttf"};
        for(const auto* path : paths)
        {
            std::error_code error;
            if(std::filesystem::is_regular_file(path,error)) { m_fontPath = path; break; }
        }
    }

    void Update(const dyf::Platform::Window& window, std::initializer_list<Control> controls)
    {
        using namespace dyf::Platform;
        const auto logical = window.GetSize(), pixels = window.GetFramebufferSize();
        const auto density = window.GetContentScale();
        const auto space=detail::HudCoordinateSpace::Make(logical.width,logical.height,pixels.width,pixels.height,density.x,density.y);
        const float scale = space.scale;
        const bool resized = logical.width != m_windowWidth || logical.height != m_windowHeight ||
            pixels.width != m_pixelWidth || pixels.height != m_pixelHeight || scale != m_scale;
        m_windowWidth = logical.width; m_windowHeight = logical.height;
        m_pixelWidth = pixels.width; m_pixelHeight = pixels.height; m_scale = scale;
        m_width = space.width; m_height = space.height;
        m_pointerScaleX = space.pointerScaleX; m_pointerScaleY = space.pointerScaleY;
        m_input = &window.GetInput();
        m_focused = window.HasFocus();
        m_controls.assign(controls.begin(),controls.end());
        m_pressed.reset();
        m_escapeConsumed = false;
        m_gesture.BeginFrame(m_focused,resized);
        BuildLayout();
        // On the first frame, mouse callbacks may contain no cursor event.
        if(!m_havePointer || resized)
        {
            auto point = m_input->GetCursorPosition();
            m_gesture.SetPosition(ToDp(point));
            m_havePointer = true;
        }
        for(const auto& event : m_input->GetEvents())
        {
            if(event.type == InputEventType::Focus) m_gesture.Focus(event.focused);
            else if(event.type == InputEventType::Cursor) m_gesture.Move(ToDp({event.x,event.y}));
            else if(event.type == InputEventType::MouseButton && event.code == int(MouseButton::Left))
            {
                if(event.action == InputAction::Press) m_gesture.Press();
                else if(event.action == InputAction::Release) m_gesture.Release();
            }
            else if(event.type == InputEventType::MouseButton)
                m_gesture.AuxiliaryButton(event.code,event.action==InputAction::Press);
            else if(event.type == InputEventType::Scroll) m_gesture.Wheel(float(event.y));
        }
        // Event snapshots may have been skipped while an app was suspended.
        // Reconcile releases so a held touch cannot survive backgrounding.
        if(m_gesture.PrimaryPressed() && !m_input->IsDown(MouseButton::Left)) m_gesture.Cancel();
        for(int button=1;button<8;++button)
            if(!m_input->IsDown(MouseButton(button))) m_gesture.AuxiliaryButton(button,false);
        // Consume panels before the caller sees Escape, including a touchscreen
        // Back button. A second Escape/Back reaches the application's exit path.
        if(m_focused && m_input->WasPressed(Key::Escape) && HasPanel())
        {
            ClosePanels();
            m_escapeConsumed = true;
        }
        if(m_focused && m_input->WasPressed(Key::F11)) SetProfilerVisible(!m_profilerVisible);
        const auto actions = m_gesture.activated;
        for(int action : actions) Activate(action);
        m_scroll = std::clamp(m_scroll+m_gesture.scrollDelta,0.f,m_scrollMaximum);
        BuildLayout();
    }

    bool Pressed(dyf::Platform::Key key) const
    {
        if(key == dyf::Platform::Key::F11 || !m_focused) return false; // HUD owns F11.
        if(key == dyf::Platform::Key::Escape && m_escapeConsumed) return false;
        if(!Enabled(key)) return false;
        const int code = int(key);
        return (code >= 0 && code < int(m_pressed.size()) && m_pressed[size_t(code)]) ||
            (m_input && m_input->WasPressed(key));
    }
    bool Down(dyf::Platform::Key key) const
    {
        return m_focused && Enabled(key) && ((m_input && m_input->IsDown(key)) || m_gesture.Held(int(key)));
    }
    dyf::Platform::InputPoint SceneDragDelta() const { return m_gesture.sceneDelta; }
    bool CapturesPointer() const { return m_gesture.CapturesPointer(); }
    bool IsProfilerVisible() const { return m_profilerVisible; }
    void SetProfilerVisible(bool visible)
    {
        if(m_profilerVisible==visible) return;
        m_profilerVisible = visible;
        if(!visible) m_details = false;
        m_gesture.Cancel();
        BuildLayout();
    }
    void ClosePanels()
    {
        m_details = false; m_controlsOpen = false; m_scroll = 0; m_gesture.Cancel(); BuildLayout();
    }
    void ToggleControls()
    {
        m_controlsOpen = !m_controlsOpen; m_details = false; m_scroll = 0; m_gesture.Cancel(); BuildLayout();
    }
    bool IsPanelOpen() const { return HasPanel(); }
    float BottomInset() const
    {
        return (m_height-(m_controlsOpen ? m_panel.y : m_toolbar.y)+8)*m_scale;
    }

    void Draw(dyf::Canvas& canvas, const dyf::Renderer& renderer, std::string_view title,
        std::string_view status = {})
    {
        if(m_width < 96 || m_height < 96) return;
        EnsureFonts();
        canvas.ResetClipRect();
        const auto& timing = renderer.GetProfilerTimingSnapshot();
        const auto& statistics = renderer.GetLastRenderStatistics();
        const auto& applied = renderer.GetAppliedConfig();
        if(!title.empty())
        {
            const float width = std::min(350.f,std::max(1.f,m_card.x-24));
            if(width >= 150 || !m_profilerVisible)
            {
                const float available = m_profilerVisible ? width : std::min(350.f,m_width-24);
                const float height = TextHeight(title,available-24,1)+(status.empty()?0:TextHeight(status,available-24,0))+22;
                const detail::HudRect badge{12,12,available,height};
                Panel(canvas,badge);
                float y = Wrapped(canvas,title,24,22,available-24,1,Paper());
                if(!status.empty()) Wrapped(canvas,status,24,y+2,available-24,0,Muted());
            }
        }
        if(m_profilerVisible && !m_details) DrawSummary(canvas,timing);
        if(m_details) DrawDetails(canvas,timing,statistics,applied);
        if(m_controlsOpen) DrawControls(canvas);
        DrawToolbar(canvas);
        canvas.ResetClipRect();
    }

private:
    enum Action : int
    {
        OpenScene = -1, OpenRender = -2, OpenCamera = -3, ToggleProfiler = -4,
        Back = -5, Details = -6, Close = -7, GpuTab = -8, CpuTab = -9, WorkTab = -10,
        ScrollUp = -11, ScrollDown = -12
    };
    struct Button { detail::HudRect bounds; int action; std::string label; bool active=false, enabled=true, held=false; };
    struct MetricRow { std::string label, value; double fraction=0; };
    std::string m_fontPath;
    std::unique_ptr<dyf::Font> m_small, m_body, m_large;
    float m_fontScale = 0, m_scale = 1, m_width = 0, m_height = 0;
    float m_pointerScaleX = 1, m_pointerScaleY = 1;
    int m_windowWidth = 0, m_windowHeight = 0, m_pixelWidth = 0, m_pixelHeight = 0;
    bool m_focused = true, m_havePointer = false, m_profilerVisible = true;
    bool m_details = false, m_controlsOpen = false, m_escapeConsumed = false;
    ControlGroup m_group = ControlGroup::Scene;
    int m_detailTab = 0;
    float m_scroll = 0, m_scrollMaximum = 0;
    const dyf::Platform::Input* m_input = nullptr;
    std::vector<Control> m_controls;
    std::vector<Button> m_buttons;
    std::vector<detail::HudRect> m_regions;
    detail::HudRect m_card, m_toolbar, m_panel, m_scrollArea;
    detail::HudGesture m_gesture;
    std::bitset<349> m_pressed;

    bool Enabled(dyf::Platform::Key key) const
    {
        const auto control=std::find_if(m_controls.begin(),m_controls.end(),[key](const auto& c) { return c.key==key; });
        return control==m_controls.end() || control->enabled;
    }
    dyf::Platform::InputPoint ToDp(dyf::Platform::InputPoint p) const { return {p.x*m_pointerScaleX,p.y*m_pointerScaleY}; }
    static dyf::Math::float4 Paper() { return {.94f,.96f,.98f,1}; }
    static dyf::Math::float4 Muted() { return {.66f,.74f,.79f,1}; }
    static dyf::Math::float4 Accent() { return {.35f,.84f,.79f,1}; }
    dyf::Rectangle Pixels(detail::HudRect r) const { return {r.x*m_scale,r.y*m_scale,r.width*m_scale,r.height*m_scale}; }
    void Fill(dyf::Canvas& canvas, detail::HudRect r, dyf::Math::float4 color) { canvas.FillRect(Pixels(r),color); }
    void Clip(dyf::Canvas& canvas, detail::HudRect r) { canvas.SetClipRect(Pixels(r)); }
    void Panel(dyf::Canvas& canvas, detail::HudRect r)
    {
        Fill(canvas,r,{.025f,.043f,.058f,.95f});
        canvas.Rect(Pixels(r),{.20f,.29f,.33f,.9f},m_scale);
    }
    void Add(detail::HudRect bounds, int action, std::string label, bool active=false, bool enabled=true, bool held=false)
    {
        m_buttons.push_back({bounds,action,std::move(label),active,enabled,held});
    }
    bool HasPanel() const { return m_details || m_controlsOpen; }

    void BuildLayout()
    {
        m_buttons.clear(); m_regions.clear(); m_scrollArea = {};
        const float margin = 12, gap = 4;
        const auto toolbar=detail::HudToolbarLayout::Make(m_width,m_height);
        m_toolbar=toolbar.bounds;
        m_regions.push_back(m_toolbar);
        const std::array<int,5> actions{OpenScene,OpenRender,OpenCamera,ToggleProfiler,Back};
        const std::array<const char*,5> labels{"Scene","Render","Camera","Profiler","Back"};
        for(size_t i=0;i<actions.size();++i)
            Add(toolbar.buttons[i],actions[i],labels[i],
                i<3 ? m_controlsOpen && int(m_group)==int(i) : i==3 && m_profilerVisible);
        const float cardWidth = std::min(304.f,std::max(0.f,m_width-2*margin));
        m_card = {m_width-margin-cardWidth,margin,cardWidth,140};
        if(m_profilerVisible && !m_details)
        {
            m_regions.push_back(m_card);
            Add({m_card.x+m_card.width-102,m_card.y,102,48},Details,"Details");
        }
        if(m_details)
        {
            const float width = std::min(430.f,m_width-2*margin);
            const float height = std::min(550.f,std::max(0.f,m_toolbar.y-margin*2));
            m_panel = {m_width-margin-width,margin,width,height};
            m_regions.push_back(m_panel);
            Add({m_panel.x+width-60,m_panel.y,60,48},Close,"Close");
            const std::array<int,3> tabs{GpuTab,CpuTab,WorkTab};
            const std::array<const char*,3> names{"GPU passes","CPU","Work"};
            const float tabWidth = (width-16)/3;
            for(size_t i=0;i<tabs.size();++i)
                Add({m_panel.x+8+float(i)*tabWidth,m_panel.y+48,tabWidth,48},tabs[i],names[i],m_detailTab==int(i));
            m_scrollArea = {m_panel.x+12,m_panel.y+105,width-24,std::max(0.f,height-165)};
            Add({m_panel.x+8,m_panel.y+height-52,64,48},ScrollUp,"Up");
            Add({m_panel.x+width-72,m_panel.y+height-52,64,48},ScrollDown,"Down");
        }
        else if(m_controlsOpen)
        {
            std::vector<const Control*> visible;
            for(const auto& control : m_controls)
                if(control.group==m_group) visible.push_back(&control);
            const float width = std::min(650.f,m_width-2*margin);
            const int columns = std::max(1,int((width-20)/125));
            const int rows = (int(visible.size())+columns-1)/columns;
            const float contentHeight = float(rows)*56;
            const float height = std::min(contentHeight+68,std::max(116.f,(m_height-90)*.58f));
            m_panel = {(m_width-width)*.5f,m_toolbar.y-8-height,width,height};
            m_regions.push_back(m_panel);
            Add({m_panel.x+width-60,m_panel.y,60,48},Close,"Close");
            m_scrollArea = {m_panel.x+10,m_panel.y+52,width-20,height-62};
            m_scrollMaximum = std::max(0.f,contentHeight-m_scrollArea.height);
            m_scroll = std::clamp(m_scroll,0.f,m_scrollMaximum);
            const float cellWidth = (width-20-float(columns-1)*gap)/float(columns);
            for(size_t i=0;i<visible.size();++i)
            {
                const auto& c = *visible[i];
                detail::HudRect r{m_scrollArea.x+float(i%size_t(columns))*(cellWidth+gap),
                    m_scrollArea.y+float(i/size_t(columns))*56-m_scroll,cellWidth,48};
                // Partly clipped buttons stay visual only. A complete 48dp target
                // is required to activate; scroll reveals the next row.
                if(r.y>=m_scrollArea.y && r.y+r.height<=m_scrollArea.y+m_scrollArea.height)
                    Add(r,int(c.key),c.label ? c.label : "",c.active,c.enabled,c.held);
            }
        }
        if(!HasPanel()) m_scrollMaximum = m_scroll = 0;
        std::vector<detail::HudTarget> targets;
        targets.reserve(m_buttons.size());
        for(const auto& b:m_buttons) targets.push_back({b.bounds,b.action,b.enabled,b.held});
        m_gesture.SetTargets(targets,m_regions,m_scrollArea);
    }

    void Activate(int action)
    {
        if(action >= 0)
        {
            if(action==int(dyf::Platform::Key::Escape)) { Activate(Back); return; }
            if(action<int(m_pressed.size())) m_pressed.set(size_t(action));
            return;
        }
        switch(action)
        {
        case OpenScene: case OpenRender: case OpenCamera:
        {
            const auto group = ControlGroup(-action-1);
            m_controlsOpen = !m_controlsOpen || m_group!=group;
            m_group = group; m_details = false; m_scroll = 0;
            break;
        }
        case ToggleProfiler: SetProfilerVisible(!m_profilerVisible); break;
        case Back:
            if(HasPanel()) { ClosePanels(); m_escapeConsumed = true; }
            else m_pressed.set(size_t(dyf::Platform::Key::Escape));
            break;
        case Details: m_details = true; m_controlsOpen = false; m_scroll = 0; break;
        case Close: ClosePanels(); break;
        case GpuTab: case CpuTab: case WorkTab: m_detailTab=-action-8; m_scroll=0; break;
        case ScrollUp: m_scroll=std::max(0.f,m_scroll-120); break;
        case ScrollDown: m_scroll=std::min(m_scrollMaximum,m_scroll+120); break;
        default: break;
        }
    }

    void DrawToolbar(dyf::Canvas& canvas)
    {
        Panel(canvas,m_toolbar);
        for(const auto& b : m_buttons)
            if(b.bounds.y>=m_toolbar.y) DrawButton(canvas,b);
    }
    void DrawButton(dyf::Canvas& canvas, const Button& button)
    {
        const bool pressed = m_gesture.TargetPressed(button.action);
        Fill(canvas,button.bounds,pressed ? dyf::Math::float4{.18f,.36f,.38f,1} :
            button.active ? dyf::Math::float4{.10f,.25f,.27f,1} : dyf::Math::float4{.045f,.075f,.09f,1});
        if(button.active) Fill(canvas,{button.bounds.x+8,button.bounds.y+button.bounds.height-3,button.bounds.width-16,2},Accent());
        const float textHeight = TextHeight(button.label,button.bounds.width-12,0);
        Clip(canvas,button.bounds);
        const auto lines = Lines(button.label,button.bounds.width-12,0);
        float y = button.bounds.y+(button.bounds.height-textHeight)*.5f;
        for(const auto& line:lines)
        {
            Text(canvas,line,button.bounds.x+(button.bounds.width-Measure(line,0))*.5f,y,0,
                !button.enabled ? dyf::Math::float4{.36f,.42f,.45f,1} : button.active ? Accent() : Paper());
            y += LineHeight(0);
        }
        canvas.ResetClipRect();
    }
    void DrawSummary(dyf::Canvas& canvas, const dyf::Platform::ProfilerTimingSnapshot& timing)
    {
        Panel(canvas,m_card);
        Text(canvas,"PERFORMANCE",m_card.x+14,m_card.y+15,0,Muted());
        const bool ready = timing.frameCount>0;
        Text(canvas,ready ? Number(timing.fps," FPS",0) : "Pending",m_card.x+14,m_card.y+46,2,Paper());
        Text(canvas,ready ? Number(timing.frameAverageMilliseconds," ms / frame") : "Waiting for completed samples",
            m_card.x+14,m_card.y+80,0,Muted());
        const std::string cpu = ready ? Number(timing.cpuRenderAverageMilliseconds," ms") : "Pending";
        const std::string gpu = !timing.gpuTimingSupported ? "Unsupported" :
            timing.hasGpuFrame ? Number(timing.gpuFrameAverageMilliseconds," ms") : "Pending";
        Text(canvas,"CPU "+cpu,m_card.x+14,m_card.y+110,0,Paper());
        Text(canvas,"GPU "+gpu,m_card.x+150,m_card.y+110,0,Accent());
        for(const auto& b:m_buttons) if(b.action==Details) DrawButton(canvas,b);
    }
    void DrawControls(dyf::Canvas& canvas)
    {
        Panel(canvas,m_panel);
        const char* title = m_group==ControlGroup::Scene ? "SCENE CONTROLS" :
            m_group==ControlGroup::Render ? "RENDER SETTINGS" : "CAMERA CONTROLS";
        Text(canvas,title,m_panel.x+14,m_panel.y+15,0,Muted());
        bool any = false;
        for(const auto& c:m_controls) any |= c.group==m_group;
        if(!any) Wrapped(canvas,"No controls in this scene.",m_scrollArea.x,m_scrollArea.y,m_scrollArea.width,0,Muted());
        for(const auto& b:m_buttons)
            if(b.action>=0) DrawButton(canvas,b);
            else if(b.action==Close) DrawButton(canvas,b);
        if(m_scrollMaximum>0) DrawScrollBar(canvas);
    }
    void DrawDetails(dyf::Canvas& canvas, const dyf::Platform::ProfilerTimingSnapshot& timing,
        const dyf::RendererStatistics& stats, const dyf::RendererConfig& config)
    {
        Panel(canvas,m_panel);
        Text(canvas,"FRAME PROFILER",m_panel.x+14,m_panel.y+15,0,Muted());
        std::vector<MetricRow> rows;
        const auto cpu = [&](double v) { return timing.frameCount ? Number(v," ms") : std::string("Pending"); };
        const auto add = [&](std::string label,std::string value,double fraction=0) { rows.push_back({std::move(label),std::move(value),fraction}); };
        if(m_detailTab==0)
        {
            add("GPU frame span",!timing.gpuTimingSupported ? "Unsupported" : timing.hasGpuFrame ? Number(timing.gpuFrameAverageMilliseconds," ms") : "Pending");
            if(!timing.gpuTimingSupported) add("Timestamp queries","Unsupported on this device");
            else if(timing.gpuPasses.empty()) add("Completed GPU pass samples","Pending");
            double total = 0;
            for(const auto& pass:timing.gpuPasses) if(pass.sampleCount) total += pass.averageMilliseconds;
            if(!timing.gpuPasses.empty()) add("Measured pass sum",Number(total," ms"));
            for(const auto& pass:timing.gpuPasses)
                add(pass.name,pass.sampleCount ? Number(pass.averageMilliseconds," ms") : "Pending",
                    pass.sampleCount ? pass.averageMilliseconds/std::max(0.001,total) : 0);
        }
        else if(m_detailTab==1)
        {
            add("Frame average",cpu(timing.frameAverageMilliseconds));
            add("Frame maximum",cpu(timing.frameMaximumMilliseconds));
            add("Render work",cpu(timing.cpuRenderAverageMilliseconds));
            add("Acquire",cpu(timing.cpuAcquireAverageMilliseconds));
            add("Prepare",cpu(timing.cpuPrepareAverageMilliseconds));
            add("Record",cpu(timing.cpuRecordAverageMilliseconds));
            add("Submit",cpu(timing.cpuSubmitAverageMilliseconds));
            add("Present",cpu(timing.cpuPresentAverageMilliseconds));
            add("Readback",cpu(timing.cpuReadbackAverageMilliseconds));
            add("Completed CPU frames",std::to_string(timing.frameCount));
        }
        else
        {
            add("Main draws",std::to_string(stats.mainDrawCount));
            add("Visible instances",stats.gpuVisibility ? "N/A (GPU)" : std::to_string(stats.mainInstanceCount));
            add("Visible triangles",stats.gpuVisibility ? "N/A (GPU)" : std::to_string(stats.mainTriangleCount));
            add("Indirect batches",std::to_string(stats.indirectBatchCount));
            add("CPU-culled entities",std::to_string(stats.culledEntityCount));
            add("Shadow draws",std::to_string(stats.shadowDrawCount));
            add("Shadow instances",std::to_string(stats.shadowInstanceCount));
            add("Shadow triangles",std::to_string(stats.shadowTriangleCount));
            add("Shadow atlas",stats.reusedShadowAtlas ? "Cached" : "Updated");
            add("Render size",std::to_string(stats.renderWidth)+" x "+std::to_string(stats.renderHeight));
            add("Output size",std::to_string(stats.outputWidth)+" x "+std::to_string(stats.outputHeight));
            add("GPU culling",stats.gpuVisibility ? "On" : "Off");
            add("GPU occlusion",stats.gpuOcclusion ? "On" : "Off");
            add("Clustered lighting",stats.clusteredLighting ? "On" : "Off");
            add("Surface prepass",stats.surfacePrepass ? "On" : "Off");
            add("LOD error",Number(config.lodPixelError," px"));
            for(size_t i=0;i<stats.lodInstanceCounts.size();++i)
                add("LOD "+std::to_string(i)+" instances",stats.gpuVisibility ? "N/A (GPU)" : std::to_string(stats.lodInstanceCounts[i]));
            add("Live buffers",std::to_string(timing.liveResources[0]));
            add("Live textures",std::to_string(timing.liveResources[1]));
            add("Live pipelines",std::to_string(timing.liveResources[2]));
        }
        const float valueWidth = std::min(136.f,m_scrollArea.width*.42f);
        const float labelWidth = std::max(1.f,m_scrollArea.width-valueWidth-12);
        float contentHeight = 0;
        for(const auto& row:rows)
            contentHeight += std::max(TextHeight(row.label,labelWidth,0),TextHeight(row.value,valueWidth,0))+18;
        m_scrollMaximum = std::max(0.f,contentHeight-m_scrollArea.height);
        m_scroll = std::clamp(m_scroll,0.f,m_scrollMaximum);
        Clip(canvas,m_scrollArea);
        float y = m_scrollArea.y-m_scroll;
        for(const auto& row:rows)
        {
            const float height = std::max(TextHeight(row.label,labelWidth,0),TextHeight(row.value,valueWidth,0))+18;
            if(y+height>=m_scrollArea.y && y<=m_scrollArea.y+m_scrollArea.height)
            {
                if(row.fraction>0)
                    Fill(canvas,{m_scrollArea.x,y+height-6,m_scrollArea.width*float(std::clamp(row.fraction,0.0,1.0)),2},{.19f,.49f,.49f,1});
                Wrapped(canvas,row.label,m_scrollArea.x,y+5,labelWidth,0,Paper());
                const auto lines=Lines(row.value,valueWidth,0);
                float valueY=y+5;
                for(const auto& line:lines)
                {
                    Text(canvas,line,m_scrollArea.x+m_scrollArea.width-Measure(line,0),valueY,0,Accent());
                    valueY+=LineHeight(0);
                }
            }
            y+=height;
        }
        canvas.ResetClipRect();
        for(const auto& b:m_buttons)
            if(b.action==Close || b.action==GpuTab || b.action==CpuTab || b.action==WorkTab || b.action==ScrollUp || b.action==ScrollDown)
                DrawButton(canvas,b);
        Text(canvas,m_scrollMaximum>0 ? "Scroll / drag for more" : "Completed samples",m_panel.x+80,m_panel.y+m_panel.height-35,0,Muted());
        if(m_scrollMaximum>0) DrawScrollBar(canvas);
    }
    void DrawScrollBar(dyf::Canvas& canvas)
    {
        const float viewport=m_scrollArea.height, total=viewport+m_scrollMaximum;
        const float thumb=std::max(20.f,viewport*viewport/std::max(1.f,total));
        const float y=m_scrollArea.y+(viewport-thumb)*m_scroll/std::max(1.f,m_scrollMaximum);
        Fill(canvas,{m_scrollArea.x+m_scrollArea.width-2,y,2,thumb},{.35f,.58f,.61f,1});
    }

    void EnsureFonts()
    {
        if(m_fontScale==m_scale) return;
        m_fontScale=m_scale;
        if(!m_fontPath.empty())
        {
            m_small=dyf::Font::Load(m_fontPath,14*m_scale);
            m_body=dyf::Font::Load(m_fontPath,18*m_scale);
            m_large=dyf::Font::Load(m_fontPath,26*m_scale);
        }
    }
    dyf::Font* Typeface(int size) { return size==2 ? m_large.get() : size==1 ? m_body.get() : m_small.get(); }
    static float FontSize(int size) { return size==2 ? 26.f : size==1 ? 18.f : 14.f; }
    float LineHeight(int size)
    {
        auto* font=Typeface(size);
        return std::max(FontSize(size)*1.3f,font ? font->GetLineHeight()/m_scale+2 : 0.f);
    }
    static uint32_t Codepoint(std::string_view text,size_t& cursor)
    {
        const auto first=uint8_t(text[cursor++]);
        if(first<0x80) return first;
        uint32_t value=first&(first<0xe0 ? 0x1fu : first<0xf0 ? 0x0fu : 0x07u);
        const size_t count=first<0xe0 ? 1u : first<0xf0 ? 2u : 3u;
        for(size_t i=0;i<count && cursor<text.size();++i)
        {
            const auto next=uint8_t(text[cursor]);
            if((next&0xc0u)!=0x80u) break;
            value=(value<<6u)|(next&0x3fu); ++cursor;
        }
        return value;
    }
    float Advance(uint32_t cp,uint32_t previous,int size)
    {
        if(auto* font=Typeface(size))
        {
            dyf::FontGlyph glyph;
            if(font->GetGlyph(cp,glyph)) return (glyph.advance+font->GetKerning(previous,cp))/m_scale;
        }
        return 6*FontSize(size)/7;
    }
    float Measure(std::string_view value,int size)
    {
        float width=0; uint32_t previous=0;
        for(size_t at=0;at<value.size();)
        {
            const auto cp=Codepoint(value,at); width+=Advance(cp,previous,size); previous=cp;
        }
        return width;
    }
    std::vector<std::string> Lines(std::string_view text,float width,int size)
    {
        std::vector<std::string> result;
        size_t start=0,cursor=0,space=std::string_view::npos;
        float used=0; uint32_t previous=0;
        while(cursor<text.size())
        {
            const size_t before=cursor;
            const auto cp=Codepoint(text,cursor);
            if(cp=='\n')
            {
                result.emplace_back(text.substr(start,before-start));
                start=cursor; space=std::string_view::npos; used=0; previous=0; continue;
            }
            const float advance=Advance(cp,previous,size);
            if(used+advance>width && before>start)
            {
                const size_t end=space!=std::string_view::npos && space>start ? space : before;
                result.emplace_back(text.substr(start,end-start));
                start=end;
                while(start<text.size() && text[start]==' ') ++start;
                cursor=start; space=std::string_view::npos; used=0; previous=0; continue;
            }
            if(cp==' ') space=before;
            used+=advance; previous=cp;
        }
        if(start<text.size()) result.emplace_back(text.substr(start));
        return result;
    }
    float TextHeight(std::string_view text,float width,int size) { return float(Lines(text,width,size).size())*LineHeight(size); }
    float Wrapped(dyf::Canvas& canvas,std::string_view text,float x,float y,float width,int size,dyf::Math::float4 color)
    {
        for(const auto& line:Lines(text,width,size)) { Text(canvas,line,x,y,size,color); y+=LineHeight(size); }
        return y;
    }
    static std::string Number(double value,const char* suffix,int precision=2)
    {
        if(!std::isfinite(value)) return "N/A";
        char text[80]; std::snprintf(text,sizeof(text),"%.*f%s",precision,value,suffix); return text;
    }
    void Text(dyf::Canvas& canvas,std::string_view value,float x,float y,int size,dyf::Math::float4 color)
    {
        if(auto* font=Typeface(size)) { (void)canvas.Text(*font,value,{x*m_scale,y*m_scale},color); return; }
        // Font files are optional on every platform. Keep labels readable on
        // stripped-down Linux installations and iOS devices without exposed fonts.
        static constexpr std::array<std::array<uint8_t,5>,36> glyphs = {{
            {{0x7e,0x11,0x11,0x11,0x7e}},{{0x7f,0x49,0x49,0x49,0x36}},{{0x3e,0x41,0x41,0x41,0x22}},
            {{0x7f,0x41,0x41,0x22,0x1c}},{{0x7f,0x49,0x49,0x49,0x41}},{{0x7f,0x09,0x09,0x09,0x01}},
            {{0x3e,0x41,0x49,0x49,0x7a}},{{0x7f,0x08,0x08,0x08,0x7f}},{{0x41,0x41,0x7f,0x41,0x41}},
            {{0x20,0x40,0x41,0x3f,0x01}},{{0x7f,0x08,0x14,0x22,0x41}},{{0x7f,0x40,0x40,0x40,0x40}},
            {{0x7f,0x02,0x0c,0x02,0x7f}},{{0x7f,0x04,0x08,0x10,0x7f}},{{0x3e,0x41,0x41,0x41,0x3e}},
            {{0x7f,0x09,0x09,0x09,0x06}},{{0x3e,0x41,0x51,0x21,0x5e}},{{0x7f,0x09,0x19,0x29,0x46}},
            {{0x46,0x49,0x49,0x49,0x31}},{{0x01,0x01,0x7f,0x01,0x01}},{{0x3f,0x40,0x40,0x40,0x3f}},
            {{0x1f,0x20,0x40,0x20,0x1f}},{{0x3f,0x40,0x38,0x40,0x3f}},{{0x63,0x14,0x08,0x14,0x63}},
            {{0x07,0x08,0x70,0x08,0x07}},{{0x61,0x51,0x49,0x45,0x43}},{{0x3e,0x51,0x49,0x45,0x3e}},
            {{0x00,0x42,0x7f,0x40,0x00}},{{0x42,0x61,0x51,0x49,0x46}},{{0x21,0x41,0x45,0x4b,0x31}},
            {{0x18,0x14,0x12,0x7f,0x10}},{{0x27,0x45,0x45,0x45,0x39}},{{0x3c,0x4a,0x49,0x49,0x30}},
            {{0x01,0x71,0x09,0x05,0x03}},{{0x36,0x49,0x49,0x49,0x36}},{{0x06,0x49,0x49,0x29,0x1e}}
        }};
        const float pixel=FontSize(size)/7;
        for(char character:value)
        {
            const char c=character>='a' && character<='z' ? char(character-'a'+'A') : character;
            const int index=c>='A' && c<='Z' ? c-'A' : c>='0' && c<='9' ? 26+c-'0' : -1;
            if(index>=0)
                for(int column=0;column<5;++column) for(int row=0;row<7;++row)
                    if(glyphs[size_t(index)][size_t(column)]&(1u<<row))
                        Fill(canvas,{x+column*pixel,y+row*pixel,pixel,pixel},color);
            if(c=='/' || c=='-') canvas.Line({x*m_scale,(y+4*pixel)*m_scale},{(x+4*pixel)*m_scale,(y+(c=='/'?0:4)*pixel)*m_scale},color,pixel*m_scale);
            if(c=='.' || c==':' || c==',') Fill(canvas,{x+2*pixel,y+6*pixel,pixel,pixel},color);
            if(c==':') Fill(canvas,{x+2*pixel,y+2*pixel,pixel,pixel},color);
            if(c=='+') { Fill(canvas,{x,y+3*pixel,5*pixel,pixel},color); Fill(canvas,{x+2*pixel,y+pixel,pixel,5*pixel},color); }
            x+=6*pixel;
        }
    }
};
}
