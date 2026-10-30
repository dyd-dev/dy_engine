#include "../ShowcaseHudInteraction.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
using showcase::detail::HudGesture;
using showcase::detail::HudRect;
using showcase::detail::HudTarget;

void Require(bool condition, const char* message)
{
    if(!condition) throw std::runtime_error(message);
}

HudGesture MakeGesture()
{
    HudGesture gesture;
    gesture.SetTargets({HudTarget{{100,100,100,48},32,true,false},
        HudTarget{{100,160,100,48},65,true,true},HudTarget{{100,220,100,48},66,false,false}},
        {HudRect{90,90,130,200}},HudRect{90,90,130,200});
    gesture.BeginFrame(true,false);
    return gesture;
}

void TapUsesReleaseAndOrderedMotion()
{
    auto g=MakeGesture();
    g.Move({120,120}); g.Press();
    Require(g.activated.empty(),"A press must not toggle a scene setting");
    g.Move({122,121}); g.Release();
    Require(g.activated==std::vector<int>{32},"A complete tap between rendered frames must activate once");
    Require(g.CapturesPointer(),"The release frame must still capture scene input");
    Require(g.sceneDelta.x==0 && g.sceneDelta.y==0,"A UI tap must not rotate the camera");
    g.BeginFrame(true,false);
    Require(g.activated.empty(),"The action must not repeat on the next frame");
}

void DragCancelsSettingAndScrolls()
{
    auto g=MakeGesture();
    g.Move({120,120}); g.Press(); g.Move({121,80}); g.Release();
    Require(g.activated.empty(),"Scrolling from a button must not toggle its setting");
    Require(g.scrollDelta==40,"A drawer drag must scroll by its logical-coordinate distance");
    Require(g.sceneDelta.x==0 && g.sceneDelta.y==0,"A drag begun over UI must never become a scene drag");
}

void SceneDragRetainsOwnership()
{
    auto g=MakeGesture();
    g.Move({20,20}); g.Press(); g.Move({120,120}); g.Release();
    Require(g.activated.empty(),"Dragging the scene onto a button must not activate it");
    Require(g.sceneDelta.x==100 && g.sceneDelta.y==100,"Scene movement must retain its content-coordinate delta");
}

void HeldCameraControlCannotStick()
{
    auto g=MakeGesture();
    g.Move({120,180}); g.Press();
    Require(g.Held(65),"Camera buttons must support continuous movement");
    g.BeginFrame(true,false);
    Require(g.Held(65),"A held camera button must survive a frame boundary");
    g.Focus(false);
    Require(!g.Held(65),"Losing focus must release a held camera control");
    g.Release();
    Require(g.activated.empty(),"Focus loss must not turn into a delayed tap");
    g.Focus(true); g.Move({120,180}); g.Press(); g.Move({300,180});
    Require(!g.Held(65),"Dragging out of a held control must stop movement");
}

void ResizeAndDisabledControlsCancelActivation()
{
    auto g=MakeGesture();
    g.Move({120,120}); g.Press(); g.BeginFrame(true,true); g.Release();
    Require(g.activated.empty(),"Resizing must invalidate old hit rectangles");
    g.Move({120,240}); g.Press(); g.Release();
    Require(g.activated.empty(),"A disabled renderer option must never activate");
    Require(g.CapturesPointer(),"A disabled button still shields the scene");
}

void WheelOnlyScrollsDrawer()
{
    auto g=MakeGesture();
    g.Move({120,120}); g.Wheel(-1);
    Require(g.scrollDelta==42,"Wheel input should scroll the visible drawer");
    g.BeginFrame(true,false); g.Move({20,20}); g.Wheel(-1);
    Require(g.scrollDelta==0,"Wheel input outside the drawer belongs to the scene");
}

void AuxiliaryDragCannotLeakIntoScene()
{
    auto g=MakeGesture();
    g.Move({120,120}); g.AuxiliaryButton(1,true); g.Move({400,400});
    Require(g.CapturesPointer(),"A right-button gesture begun over UI must stay captured outside the panel");
    g.AuxiliaryButton(1,false);
    Require(!g.CapturesPointer(),"A released right-button gesture must not leave capture stuck");
}

void DensityAndPortraitLayoutsUseSameTargets()
{
    using showcase::detail::HudCoordinateSpace;
    using showcase::detail::HudToolbarLayout;
    const auto desktop=HudCoordinateSpace::Make(1280,720,1280,720,1,1);
    const auto retina=HudCoordinateSpace::Make(1280,720,2560,1440,2,2);
    const auto android=HudCoordinateSpace::Make(1080,2340,1080,2340,3,3);
    const auto scaledWindows=HudCoordinateSpace::Make(2560,1440,2560,1440,2,2);
    Require(desktop.ToDp({100,100}).x==100 && retina.ToDp({100,100}).x==100,
        "Retina window coordinates must not be multiplied twice");
    Require(std::abs(android.ToDp({300,300}).x-100)<.001 && scaledWindows.ToDp({200,200}).x==100,
        "Pixel-coordinate input must be converted to density-independent coordinates");
    Require(android.width==360 && android.height==780,"Android portrait layout must use dp dimensions");
    for(const auto space : {desktop,retina,android,scaledWindows,HudCoordinateSpace::Make(240,640,240,640,1,1)})
    {
        const auto toolbar=HudToolbarLayout::Make(space.width,space.height);
        for(const auto button:toolbar.buttons)
        {
            Require(button.width>=48 && button.height>=48,"Every toolbar action must retain a 48dp touch target");
            Require(button.x>=toolbar.bounds.x && button.y>=toolbar.bounds.y &&
                button.x+button.width<=toolbar.bounds.x+toolbar.bounds.width+.001f &&
                button.y+button.height<=toolbar.bounds.y+toolbar.bounds.height+.001f,
                "Portrait and narrow-window controls must remain inside the toolbar");
        }
    }
}
}

int main()
{
    try
    {
        TapUsesReleaseAndOrderedMotion(); DragCancelsSettingAndScrolls(); SceneDragRetainsOwnership();
        HeldCameraControlCannotStick(); ResizeAndDisabledControlsCancelActivation(); WheelOnlyScrollsDrawer();
        AuxiliaryDragCannotLeakIntoScene(); DensityAndPortraitLayoutsUseSameTargets();
        std::cout << "Showcase HUD interaction checks passed\n";
        return EXIT_SUCCESS;
    }
    catch(const std::exception& error)
    {
        std::cerr << error.what() << '\n'; return EXIT_FAILURE;
    }
}
