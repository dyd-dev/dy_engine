#pragma once

#include "dyf/Platform/Input.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace showcase::detail
{
// Hit testing and gestures use density-independent coordinates. Rendering converts
// these rectangles to framebuffer pixels only at the Canvas boundary.
struct HudRect
{
    float x = 0, y = 0, width = 0, height = 0;
    bool Contains(dyf::Platform::InputPoint point) const
    {
        return point.x >= x && point.y >= y && point.x < x+width && point.y < y+height;
    }
};

struct HudTarget
{
    HudRect bounds;
    int action = 0;
    bool enabled = true, held = false;
};

struct HudCoordinateSpace
{
    float scale = 1, width = 0, height = 0, pointerScaleX = 1, pointerScaleY = 1;
    static HudCoordinateSpace Make(int windowWidth,int windowHeight,int pixelWidth,int pixelHeight,
        float densityX,float densityY)
    {
        const auto valid=[](float value) { return std::isfinite(value) && value>0 ? value : 1.f; };
        HudCoordinateSpace result;
        result.scale=std::max({1.f,valid(densityX),valid(densityY)});
        result.width=float(std::max(0,pixelWidth))/result.scale;
        result.height=float(std::max(0,pixelHeight))/result.scale;
        result.pointerScaleX=windowWidth>0 ? result.width/float(windowWidth) : 1.f;
        result.pointerScaleY=windowHeight>0 ? result.height/float(windowHeight) : 1.f;
        return result;
    }
    dyf::Platform::InputPoint ToDp(dyf::Platform::InputPoint point) const
    {
        return {point.x*pointerScaleX,point.y*pointerScaleY};
    }
};

struct HudToolbarLayout
{
    HudRect bounds;
    std::array<HudRect,5> buttons;
    static HudToolbarLayout Make(float width,float height)
    {
        HudToolbarLayout result;
        const float available=std::min(476.f,std::max(0.f,width-16));
        const int columns=std::clamp(int((available-8)/48),1,5);
        const int rows=(5+columns-1)/columns;
        const float cell=(available-8)/float(columns), barHeight=float(rows)*48+4;
        result.bounds={(width-available)*.5f,height-barHeight-8,available,barHeight};
        for(size_t i=0;i<result.buttons.size();++i)
            result.buttons[i]={result.bounds.x+4+float(i%size_t(columns))*cell,
                result.bounds.y+2+float(i/size_t(columns))*48,cell,48};
        return result;
    }
};

// Replays the ordered platform events rather than the final button snapshot:
// a press, motion and release can all happen between two rendered frames.
class HudGesture
{
public:
    void BeginFrame(bool focused, bool resized)
    {
        activated.clear();
        sceneDelta = {};
        scrollDelta = 0;
        capturedThisFrame = false;
        if(!focused || resized) Cancel();
        m_focused = focused;
    }

    void SetTargets(const std::vector<HudTarget>& targets, const std::vector<HudRect>& regions,
        HudRect scrollArea = {})
    {
        m_targets = targets;
        m_regions = regions;
        m_scrollArea = scrollArea;
    }

    void SetPosition(dyf::Platform::InputPoint position) { m_position = position; }
    dyf::Platform::InputPoint Position() const { return m_position; }

    void Move(dyf::Platform::InputPoint position)
    {
        const auto old = m_position;
        m_position = position;
        if(!m_focused || !m_pressed) return;
        if(m_scene)
        {
            sceneDelta.x += position.x-old.x;
            sceneDelta.y += position.y-old.y;
            return;
        }
        capturedThisFrame = true;
        if(std::hypot(position.x-m_origin.x,position.y-m_origin.y) > 8.0) m_dragged = true;
        if(m_dragged)
        {
            m_heldAction = 0;
            if(m_scrollGesture) scrollDelta += float(old.y-position.y);
        }
        if(m_heldAction && !m_pressedBounds.Contains(position)) m_heldAction = 0;
    }

    void Press()
    {
        if(!m_focused || m_pressed) return;
        m_pressed = true;
        m_origin = m_position;
        m_dragged = false;
        m_scene = !OverUi();
        m_scrollGesture = !m_scene && m_scrollArea.Contains(m_position);
        m_pressedAction = m_heldAction = 0;
        if(m_scene) return;
        capturedThisFrame = true;
        // Later targets are drawn on top.
        for(auto it=m_targets.rbegin(); it!=m_targets.rend(); ++it)
            if(it->bounds.Contains(m_position))
            {
                if(it->enabled)
                {
                    m_pressedAction = it->action;
                    m_pressedBounds = it->bounds;
                    if(it->held) m_heldAction = it->action;
                }
                break;
            }
    }

    void Release()
    {
        if(!m_pressed) return;
        if(!m_scene)
        {
            capturedThisFrame = true;
            if(!m_dragged && m_pressedAction && m_pressedBounds.Contains(m_position))
                activated.push_back(m_pressedAction);
        }
        m_pressed = m_scene = m_dragged = m_scrollGesture = false;
        m_pressedAction = m_heldAction = 0;
    }

    void Wheel(float amount)
    {
        if(m_scrollArea.Contains(m_position)) { scrollDelta -= amount*42; capturedThisFrame = true; }
    }

    void AuxiliaryButton(int button,bool down)
    {
        if(button<0 || button>=int(m_auxiliaryCaptured.size())) return;
        if(!down) m_auxiliaryCaptured[size_t(button)]=false;
        else if(m_focused && OverUi()) m_auxiliaryCaptured[size_t(button)]=true;
    }

    void Focus(bool focused)
    {
        m_focused = focused;
        if(!focused) { Cancel(); activated.clear(); scrollDelta = 0; }
    }

    void Cancel()
    {
        m_pressed = m_scene = m_dragged = m_scrollGesture = false;
        m_pressedAction = m_heldAction = 0;
        m_auxiliaryCaptured.fill(false);
        sceneDelta = {};
    }

    bool OverUi() const
    {
        return std::any_of(m_regions.begin(),m_regions.end(),[&](const auto& r) { return r.Contains(m_position); });
    }
    bool CapturesPointer() const
    {
        return capturedThisFrame || (m_pressed && !m_scene) || OverUi() ||
            std::any_of(m_auxiliaryCaptured.begin(),m_auxiliaryCaptured.end(),[](bool captured) { return captured; });
    }
    bool PrimaryPressed() const { return m_pressed; }
    bool Held(int action) const { return action != 0 && m_heldAction == action; }
    bool TargetPressed(int action) const { return m_pressed && !m_dragged && m_pressedAction == action; }

    std::vector<int> activated;
    dyf::Platform::InputPoint sceneDelta;
    float scrollDelta = 0;
    bool capturedThisFrame = false;

private:
    std::vector<HudTarget> m_targets;
    std::vector<HudRect> m_regions;
    HudRect m_scrollArea, m_pressedBounds;
    dyf::Platform::InputPoint m_position{}, m_origin{};
    bool m_pressed = false, m_scene = false, m_dragged = false, m_focused = true, m_scrollGesture = false;
    int m_pressedAction = 0, m_heldAction = 0;
    std::array<bool,8> m_auxiliaryCaptured{};
};
}
