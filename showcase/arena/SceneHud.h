#pragma once

#include "dyf/Canvas.h"
#include "dyf/Font.h"
#include "dyf/Platform/Profiler.h"
#include "dyf/Renderer.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace showcase::arena
{
enum class SceneAction : uint8_t
{
    None, Pause, Restart, Tour, Overview, Profiler, Menu,
    Frustum, Instancing, Workers, GpuCulling, Lod, VSync, CaptureA, CaptureB, Clear,
    Count
};

struct SceneControlState
{
    bool paused = false, tour = true, overview = false, profiler = false, menu = false;
};

class SceneHud
{
public:
    explicit SceneHud(const std::string& fontPath = {})
    {
        if(!fontPath.empty()) m_fontPath = fontPath;
        else
        {
            const std::array<const char*, 14> paths = {
                "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf",
                "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                "C:/Windows/Fonts/segoeuib.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
                "/system/fonts/Roboto-Bold.ttf", "/system/fonts/NotoSans-Bold.ttf",
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
                "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                "C:/Windows/Fonts/segoeui.ttf", "/System/Library/Fonts/Helvetica.ttc",
                "/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf"};
            for(const auto* path : paths)
                if(std::filesystem::is_regular_file(path)) { m_fontPath = path; break; }
        }
    }

    void Draw(dyf::Canvas& canvas, uint32_t width, uint32_t height, double seconds,
        uint32_t actors, bool paused, bool orbit, bool help, float contentScale = 1, float bottomInset = 0)
    {
        if(width < 64 || float(height)-bottomInset < 64) return;
        EnsureFonts(height, contentScale);
        const float w = float(width), h = float(height)-bottomInset, s = m_scale;
        const float margin = std::min(24*s, w*.04f), padding = 16*s;
        const float panelWidth = std::min(410*s, w-2*margin), textWidth = std::max(1.f, panelWidth-2*padding);
        const dyf::Math::float4 paper{.97f,.97f,.93f,1}, muted{.78f,.84f,.87f,1}, gold{1,.78f,.42f,1};
        canvas.SetClipRect({0,0,w,h});
        const float titleHeight = WrappedHeight("EMBERKEEP", textWidth, 2) +
            WrappedHeight("THE SUNKEN COURTYARD", textWidth, 0) +
            WrappedHeight("A REAL-TIME DUNGEON SCENE", textWidth, 0) + 2*padding;
        canvas.FillRect({margin,margin,panelWidth,titleHeight},{.012f,.023f,.035f,.92f});
        canvas.FillRect({margin,margin,4*s,titleHeight},gold);
        float y = margin+padding;
        y = Wrapped(canvas,"EMBERKEEP",margin+padding,y,textWidth,2,paper);
        y = Wrapped(canvas,"THE SUNKEN COURTYARD",margin+padding,y,textWidth,0,muted);
        Wrapped(canvas,"A REAL-TIME DUNGEON SCENE",margin+padding,y,textWidth,0,muted);
        char line[120];
        std::snprintf(line,sizeof(line),"%02u:%02u   %u ACTORS   %s",
            uint32_t(seconds)/60,uint32_t(seconds)%60,actors,paused?"PAUSED":orbit?"CAMERA TOUR":"FIXED CAMERA");
        const std::string status = line;
        const std::string shortcuts = bottomInset > 0 ? "Use the buttons below. Menu: render settings and A/B." :
            "SPACE pause   TAB camera   O wide   R restart   F1 controls";
        const float footerWidth = w-2*margin, footerTextWidth = std::max(1.f, footerWidth-2*padding);
        const float footerHeight = WrappedHeight(status,footerTextWidth,1)+WrappedHeight(shortcuts,footerTextWidth,0)+2*padding;
        const float footerTop = std::max(margin,h-margin-footerHeight);
        canvas.FillRect({margin,footerTop,footerWidth,footerHeight},{.012f,.023f,.035f,.94f});
        y = Wrapped(canvas,status,margin+padding,footerTop+padding,footerTextWidth,1,paper);
        Wrapped(canvas,shortcuts,margin+padding,y,footerTextWidth,0,muted);
        if(help)
        {
            const std::array<const char*,7> lines = {
                "SPACE / R: Pause / restart the scene",
                "TAB / O: Camera tour / overview",
                "WASD / ARROWS: Pan. Scroll: Zoom",
                "F5 / F6 / F7: Instancing A / B / clear",
                "4 / 5 / 6 / U: Culling / instances / workers / GPU",
                "L / V / H: LOD / VSync / labels",
                "F1 / F11 / F12: Controls / profiler / RenderDoc"};
            const float helpWidth = std::min(660*s,w-2*margin), available = std::max(1.f,helpWidth-2*padding);
            float helpHeight = 2*padding+WrappedHeight("SCENE CONTROLS",available,1)+8*s;
            for(const auto* text : lines) helpHeight += WrappedHeight(text,available,1)+6*s;
            helpHeight = std::min(helpHeight,h-2*margin);
            const float x = (w-helpWidth)*.5f, top = std::max(margin,(h-helpHeight)*.5f);
            canvas.FillRect({x,top,helpWidth,helpHeight},{.012f,.025f,.04f,.98f});
            canvas.Rect({x,top,helpWidth,helpHeight},{.3f,.62f,.66f,1},s);
            canvas.SetClipRect({x+padding,top+padding,available,std::max(1.f,helpHeight-2*padding)});
            y = Wrapped(canvas,"SCENE CONTROLS",x+padding,top+padding,available,1,gold)+8*s;
            for(const auto* text : lines) y = Wrapped(canvas,text,x+padding,y,available,1,paper)+6*s;
        }
        canvas.ResetClipRect();
    }

    void DrawProfiler(dyf::Canvas& canvas, uint32_t width, uint32_t height,
        const dyf::Platform::ProfilerTimingSnapshot& timing, const dyf::RendererStatistics& statistics,
        bool gpuSupported, float contentScale = 1, float bottomInset = 0)
    {
        if(width < 64 || float(height)-bottomInset < 64) return;
        EnsureFonts(height,contentScale);
        const float w = float(width), h = float(height)-bottomInset, s = m_scale;
        const float margin = std::min(22*s,w*.035f), padding = 16*s;
        const float panelWidth = std::min(1000*s,w-2*margin), x = margin+padding;
        const float available = std::max(1.f,panelWidth-2*padding), bottom = h-margin-padding;
        const dyf::Math::float4 paper{.97f,.98f,1,1}, muted{.76f,.84f,.9f,1}, gold{1,.79f,.42f,1};
        canvas.FillRect({margin,margin,panelWidth,h-2*margin},{.008f,.017f,.03f,.97f});
        canvas.Rect({margin,margin,panelWidth,h-2*margin},{.23f,.42f,.52f,1},s);
        canvas.SetClipRect({x,margin+padding,available,std::max(1.f,bottom-margin-padding)});
        float y = Wrapped(canvas,"FRAME PROFILER",x,margin+padding,available,2,paper);
        y = Wrapped(canvas,bottomInset > 0 ? "Profiler button closes this view. Completed samples." :
            "F11 close   |   Completed timing samples",x,y,available,0,muted)+8*s;
        const bool hasCpu = timing.frameCount > 0;
        const auto ms = [&](double value) { return hasCpu ? Number(value," ms") : std::string("Pending"); };
        const auto row = [&](std::string_view label, const std::string& value) {
            return Metric(canvas,label,value,x,y,available,bottom,paper,muted);
        };
        const auto section = [&](const char* name) {
            const float needed = LineHeight(0)+12*s;
            if(y+needed > bottom) return false;
            y += 8*s;
            canvas.Line({x,y},{x+available,y},{.24f,.36f,.44f,1},s);
            y = Wrapped(canvas,name,x,y+4*s,available,0,gold);
            return true;
        };
        bool fits = row("FPS",hasCpu ? Number(timing.fps,"") : "Pending") &&
            row("Frame average",ms(timing.frameAverageMilliseconds)) &&
            row("Frame maximum",ms(timing.frameMaximumMilliseconds));
        if(fits && section("CPU WALL TIME"))
        {
            fits = row("Work",ms(timing.cpuRenderAverageMilliseconds)) &&
                row("Prepare",ms(timing.cpuPrepareAverageMilliseconds)) &&
                row("Record",ms(timing.cpuRecordAverageMilliseconds)) &&
                row("Submit",ms(timing.cpuSubmitAverageMilliseconds)) &&
                row("Acquire",ms(timing.cpuAcquireAverageMilliseconds)) &&
                row("Present",ms(timing.cpuPresentAverageMilliseconds));
        }
        if(fits && section("GPU"))
        {
            const std::string span = !gpuSupported ? "Unsupported" : timing.hasGpuFrame ?
                Number(timing.gpuFrameAverageMilliseconds," ms") : "Pending";
            fits = row("Frame span",span);
        }
        if(fits && section("SUBMITTED WORK"))
        {
            if(statistics.gpuVisibility)
                fits = row("Indirect batches",std::to_string(statistics.indirectBatchCount)) &&
                    row("Visible instances","N/A (GPU)") && row("Visible triangles","N/A (GPU)");
            else
                fits = row("Main draws",std::to_string(statistics.mainDrawCount)) &&
                    row("Instances",std::to_string(statistics.mainInstanceCount)) &&
                    row("Triangles",std::to_string(statistics.mainTriangleCount));
        }
        if(fits && section("GPU PASSES / MS"))
        {
            if(!gpuSupported || timing.gpuPasses.empty())
                Wrapped(canvas,!gpuSupported ? "GPU timestamp queries are unsupported." :
                    "Pending completed GPU pass samples.",x,y,available,0,muted);
            else
            {
                size_t shown = 0;
                const float footer = LineHeight(0)*2;
                for(const auto& pass : timing.gpuPasses)
                {
                    if(!Metric(canvas,pass.name,Number(pass.averageMilliseconds,""),x,y,available,
                        bottom-footer,paper,muted)) break;
                    ++shown;
                }
                if(shown < timing.gpuPasses.size())
                {
                    const auto more = std::to_string(timing.gpuPasses.size()-shown)+" more passes. Enlarge the window to see all.";
                    Wrapped(canvas,more,x,y+4*s,available,0,gold);
                }
            }
        }
        canvas.ResetClipRect();
    }

    float TouchInset(uint32_t width, uint32_t height, bool menu, float contentScale = 1) const
    {
        return MakeTouchLayout(width,height,menu,contentScale).inset;
    }

    SceneAction TouchAction(uint32_t width, uint32_t height, float x, float y,
        bool menu, float contentScale = 1) const
    {
        for(const auto& button : MakeTouchLayout(width,height,menu,contentScale).buttons)
        {
            const auto& bounds = button.bounds;
            if(x >= bounds.x && x < bounds.x+bounds.width && y >= bounds.y && y < bounds.y+bounds.height)
                return button.action;
        }
        return SceneAction::None;
    }

    void DrawTouchControls(dyf::Canvas& canvas, uint32_t width, uint32_t height,
        const SceneControlState& state, const dyf::RendererConfig& config, float contentScale = 1)
    {
        if(width < 64 || height < 64) return;
        EnsureFonts(height,contentScale);
        const auto layout = MakeTouchLayout(width,height,state.menu,contentScale);
        const dyf::Math::float4 paper{.97f,.98f,1,1}, gold{1,.79f,.42f,1};
        canvas.ResetClipRect();
        canvas.FillRect(layout.toolbar,{.008f,.017f,.03f,.98f});
        if(state.menu)
        {
            canvas.FillRect(layout.menu,{.008f,.017f,.03f,.98f});
            canvas.Rect(layout.menu,{.28f,.47f,.56f,1},m_scale);
            Text(canvas,"RENDER OPTIONS",layout.menu.x+12*m_scale,layout.menu.y+6*m_scale,0,gold);
        }
        for(const auto& button : layout.buttons)
        {
            bool active = false;
            const char* label = button.label;
            switch(button.action)
            {
            case SceneAction::Pause: active=state.paused; if(state.paused) label="Play"; break;
            case SceneAction::Tour: active=state.tour; break;
            case SceneAction::Overview: active=state.overview; break;
            case SceneAction::Profiler: active=state.profiler; break;
            case SceneAction::Menu: active=state.menu; if(state.menu) label="Close menu"; break;
            case SceneAction::Frustum: active=config.enableFrustumCulling; break;
            case SceneAction::Instancing: active=config.enableInstancing; break;
            case SceneAction::Workers: active=config.enableParallelRenderGraph; break;
            case SceneAction::GpuCulling: active=config.enableGpuCulling; break;
            case SceneAction::Lod: active=config.enableLod; break;
            case SceneAction::VSync: active=config.vsync; break;
            default: break;
            }
            canvas.FillRect(button.bounds,active ? dyf::Math::float4{.17f,.27f,.31f,1} : dyf::Math::float4{.045f,.085f,.115f,1});
            canvas.Rect(button.bounds,active ? gold : dyf::Math::float4{.3f,.46f,.54f,1},active ? 2*m_scale : m_scale);
            const float available = std::max(1.f,button.bounds.width-12*m_scale);
            const auto lines = Lines(label,available,0);
            float y = button.bounds.y+std::max(0.f,(button.bounds.height-float(lines.size())*LineHeight(0))*.5f);
            canvas.SetClipRect(button.bounds);
            for(const auto& line : lines)
            {
                Text(canvas,line,button.bounds.x+(button.bounds.width-Measure(line,0))*.5f,y,0,active ? gold : paper);
                y += LineHeight(0);
            }
            canvas.ResetClipRect();
        }
    }

private:
    std::string m_fontPath;
    float m_scale = 0;
    std::unique_ptr<dyf::Font> m_small, m_body, m_title;

    struct TouchButton
    {
        dyf::Rectangle bounds;
        SceneAction action;
        const char* label;
    };
    struct TouchLayout
    {
        dyf::Rectangle toolbar, menu;
        std::vector<TouchButton> buttons;
        float inset = 0;
    };

    static float UiScale(uint32_t height, float contentScale)
    {
        if(!std::isfinite(contentScale)) contentScale = 1;
        // Keep the desktop height-based limit while preserving native Retina DPI.
        const float scale = std::max(std::max(1.f,contentScale),std::clamp(float(height)/900.f,1.f,2.f));
        return std::ceil(scale*20.f)/20.f;
    }

    static TouchLayout MakeTouchLayout(uint32_t width, uint32_t height, bool menu, float contentScale)
    {
        TouchLayout result;
        if(width < 64 || height < 64) return result;
        const float s = UiScale(height,contentScale), w = float(width), h = float(height);
        const float margin = 8*s, gap = 8*s, buttonHeight = 56*s;
        const float available = std::max(1.f,w-2*margin);
        const uint32_t columns = available >= 520*s ? 6u : 3u, rows = (6u+columns-1u)/columns;
        const float buttonWidth = std::max(1.f,(available-float(columns-1)*gap)/float(columns));
        const float toolbarHeight = float(rows)*buttonHeight+float(rows-1)*gap;
        const float toolbarTop = h-margin-toolbarHeight;
        result.toolbar = {0,toolbarTop-margin,w,toolbarHeight+2*margin};
        const std::array<SceneAction,6> primary = {SceneAction::Pause,SceneAction::Restart,SceneAction::Tour,
            SceneAction::Overview,SceneAction::Profiler,SceneAction::Menu};
        const std::array<const char*,6> primaryLabels = {"Pause","Restart","Tour","Overview","Profiler","Menu"};
        for(uint32_t i=0; i<primary.size(); ++i)
            result.buttons.push_back({{margin+float(i%columns)*(buttonWidth+gap),
                toolbarTop+float(i/columns)*(buttonHeight+gap),buttonWidth,buttonHeight},primary[i],primaryLabels[i]});
        result.inset = toolbarHeight+2*margin;
        if(menu)
        {
            const float panelWidth = std::min(660*s,available), panelX = (w-panelWidth)*.5f;
            const uint32_t menuColumns = panelWidth >= 290*s ? 3u : 2u, menuRows = (9u+menuColumns-1u)/menuColumns;
            const float menuWidth = std::max(1.f,(panelWidth-2*margin-float(menuColumns-1)*gap)/float(menuColumns));
            const float header = 28*s, panelHeight = header+2*margin+float(menuRows)*buttonHeight+float(menuRows-1)*gap;
            const float panelTop = std::max(0.f,result.toolbar.y-gap-panelHeight);
            result.menu = {panelX,panelTop,panelWidth,panelHeight};
            const std::array<SceneAction,9> actions = {SceneAction::Frustum,SceneAction::Instancing,SceneAction::Workers,
                SceneAction::GpuCulling,SceneAction::Lod,SceneAction::VSync,SceneAction::CaptureA,SceneAction::CaptureB,SceneAction::Clear};
            const std::array<const char*,9> labels = {"Culling","Instancing","Workers","GPU cull","LOD","VSync","Capture A","Capture B","Clear A/B"};
            for(uint32_t i=0; i<actions.size(); ++i)
                result.buttons.push_back({{panelX+margin+float(i%menuColumns)*(menuWidth+gap),
                    panelTop+header+margin+float(i/menuColumns)*(buttonHeight+gap),menuWidth,buttonHeight},actions[i],labels[i]});
            result.inset = h-panelTop+margin;
        }
        return result;
    }

    void EnsureFonts(uint32_t height, float contentScale)
    {
        // Width changes layout only. Text never falls below 16 / 18 / 30 pixels.
        const float rounded = UiScale(height,contentScale);
        if(m_scale == rounded) return;
        m_scale = rounded;
        if(!m_fontPath.empty())
        {
            m_small = dyf::Font::Load(m_fontPath,16*m_scale);
            m_body = dyf::Font::Load(m_fontPath,18*m_scale);
            m_title = dyf::Font::Load(m_fontPath,30*m_scale);
        }
    }

    dyf::Font* Typeface(int size)
    {
        return size == 2 ? m_title.get() : size == 1 ? m_body.get() : m_small.get();
    }

    float LineHeight(int size)
    {
        const float pixels = (size == 2 ? 30.f : size == 1 ? 18.f : 16.f)*m_scale;
        auto* font = Typeface(size);
        return std::max(pixels*1.35f,font ? font->GetLineHeight()+3*m_scale : pixels*1.35f);
    }

    static uint32_t Codepoint(std::string_view text, size_t& cursor)
    {
        const auto first = uint8_t(text[cursor++]);
        if(first < 0x80) return first;
        uint32_t value = first & (first < 0xe0 ? 0x1fu : first < 0xf0 ? 0x0fu : 0x07u);
        const size_t count = first < 0xe0 ? 1u : first < 0xf0 ? 2u : 3u;
        for(size_t i=0; i<count && cursor<text.size(); ++i)
        {
            const auto next = uint8_t(text[cursor]);
            if((next & 0xc0u) != 0x80u) break;
            value = (value<<6u) | (next & 0x3fu);
            ++cursor;
        }
        return value;
    }

    float Advance(uint32_t codepoint, uint32_t previous, int size)
    {
        if(auto* font = Typeface(size))
        {
            dyf::FontGlyph glyph;
            if(font->GetGlyph(codepoint,glyph)) return glyph.advance+font->GetKerning(previous,codepoint);
        }
        return 6.f*(size == 2 ? 30.f : size == 1 ? 18.f : 16.f)*m_scale/7.f;
    }

    float Measure(std::string_view text, int size)
    {
        float result = 0;
        uint32_t previous = 0;
        for(size_t i=0; i<text.size();)
        {
            const auto cp = Codepoint(text,i);
            result += Advance(cp,previous,size);
            previous = cp;
        }
        return result;
    }

    std::vector<std::string> Lines(std::string_view text, float width, int size)
    {
        std::vector<std::string> result;
        size_t start = 0, cursor = 0, space = std::string_view::npos;
        float used = 0;
        uint32_t previous = 0;
        while(cursor < text.size())
        {
            const size_t before = cursor;
            const auto cp = Codepoint(text,cursor);
            if(cp == '\n')
            {
                result.emplace_back(text.substr(start,before-start));
                start = cursor; space = std::string_view::npos; used = 0; previous = 0;
                continue;
            }
            const float advance = Advance(cp,previous,size);
            if(used+advance > width && before > start)
            {
                const size_t end = space != std::string_view::npos && space > start ? space : before;
                result.emplace_back(text.substr(start,end-start));
                start = end;
                while(start < text.size() && text[start] == ' ') ++start;
                cursor = start; space = std::string_view::npos; used = 0; previous = 0;
                continue;
            }
            if(cp == ' ') space = before;
            used += advance;
            previous = cp;
        }
        if(start < text.size()) result.emplace_back(text.substr(start));
        return result;
    }

    float WrappedHeight(std::string_view value, float width, int size)
    {
        return float(Lines(value,width,size).size())*LineHeight(size);
    }

    float Wrapped(dyf::Canvas& canvas, std::string_view value, float x, float y, float width,
        int size, dyf::Math::float4 color)
    {
        for(const auto& line : Lines(value,width,size))
        {
            Text(canvas,line,x,y,size,color);
            y += LineHeight(size);
        }
        return y;
    }

    bool Metric(dyf::Canvas& canvas, std::string_view label, const std::string& value,
        float x, float& y, float width, float bottom, dyf::Math::float4 paper, dyf::Math::float4 muted)
    {
        const float valueWidth = Measure(value,1), labelWidth = width-valueWidth-18*m_scale;
        const bool stacked = labelWidth < std::min(110*m_scale,width*.4f);
        const float labelHeight = WrappedHeight(label,stacked ? width : labelWidth,1);
        const float valueHeight = WrappedHeight(value,width,1);
        const float rowHeight = (stacked ? labelHeight+valueHeight : std::max(labelHeight,valueHeight))+3*m_scale;
        if(y+rowHeight > bottom) return false;
        Wrapped(canvas,label,x,y,stacked ? width : labelWidth,1,muted);
        if(stacked) Wrapped(canvas,value,x,y+labelHeight,width,1,paper);
        else Text(canvas,value,x+width-valueWidth,y,1,paper);
        y += rowHeight;
        return true;
    }

    static std::string Number(double value, const char* suffix)
    {
        if(!std::isfinite(value)) return "N/A";
        char text[64];
        std::snprintf(text,sizeof(text),"%.2f%s",value,suffix);
        return text;
    }

    void Text(dyf::Canvas& canvas, std::string_view value, float x, float y, int size, dyf::Math::float4 color)
    {
        auto* font = Typeface(size);
        if(font) { (void)canvas.Text(*font, value, {x,y}, color); return; }
        static constexpr std::array<std::array<uint8_t,5>,36> glyphs = {{
            {{0x7e,0x11,0x11,0x11,0x7e}},{{0x7f,0x49,0x49,0x49,0x36}},{{0x3e,0x41,0x41,0x41,0x22}},
            {{0x7f,0x41,0x41,0x22,0x1c}},{{0x7f,0x49,0x49,0x49,0x41}},{{0x7f,0x09,0x09,0x09,0x01}},
            {{0x3e,0x41,0x49,0x49,0x7a}},{{0x7f,0x08,0x08,0x08,0x7f}},{{0x41,0x41,0x7f,0x41,0x41}},
            {{0x20,0x40,0x41,0x3f,0x01}},{{0x7f,0x08,0x14,0x22,0x41}},{{0x7f,0x40,0x40,0x40,0x40}},
            {{0x7f,0x02,0x04,0x02,0x7f}},{{0x7f,0x04,0x08,0x10,0x7f}},{{0x3e,0x41,0x41,0x41,0x3e}},
            {{0x7f,0x09,0x09,0x09,0x06}},{{0x3e,0x41,0x51,0x21,0x5e}},{{0x7f,0x09,0x19,0x29,0x46}},
            {{0x46,0x49,0x49,0x49,0x31}},{{0x01,0x01,0x7f,0x01,0x01}},{{0x3f,0x40,0x40,0x40,0x3f}},
            {{0x1f,0x20,0x40,0x20,0x1f}},{{0x3f,0x40,0x38,0x40,0x3f}},{{0x63,0x14,0x08,0x14,0x63}},
            {{0x07,0x08,0x70,0x08,0x07}},{{0x61,0x51,0x49,0x45,0x43}},{{0x3e,0x51,0x49,0x45,0x3e}},
            {{0x00,0x42,0x7f,0x40,0x00}},{{0x42,0x61,0x51,0x49,0x46}},{{0x21,0x41,0x45,0x4b,0x31}},
            {{0x18,0x14,0x12,0x7f,0x10}},{{0x27,0x45,0x45,0x45,0x39}},{{0x3c,0x4a,0x49,0x49,0x30}},
            {{0x01,0x71,0x09,0x05,0x03}},{{0x36,0x49,0x49,0x49,0x36}},{{0x06,0x49,0x49,0x29,0x1e}}
        }};
        const float pixel = m_scale * (size == 2 ? 30.f : size == 1 ? 18.f : 16.f)/7.f;
        for(char character : value)
        {
            const char c = character >= 'a' && character <= 'z' ? char(character-'a'+'A') : character;
            const int index = c >= 'A' && c <= 'Z' ? c-'A' : c >= '0' && c <= '9' ? 26+c-'0' : -1;
            if(index >= 0)
                for(int column=0; column<5; ++column) for(int row=0; row<7; ++row)
                    if(glyphs[size_t(index)][size_t(column)] & (1u<<row))
                        canvas.FillRect({x+column*pixel,y+row*pixel,pixel,pixel}, color);
            if(c == '/' || c == '-') canvas.Line({x,y+4*pixel},{x+4*pixel,y+(c=='/'?0:4)*pixel},color,pixel);
            if(c == '.' || c == ':' || c == ',') canvas.FillRect({x+2*pixel,y+6*pixel,pixel,pixel},color);
            if(c == ':') canvas.FillRect({x+2*pixel,y+2*pixel,pixel,pixel},color);
            x += 6*pixel;
        }
    }
};
}
