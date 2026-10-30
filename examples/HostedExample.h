#pragma once

#include "dyf/Platform/Window.h"
#include <memory>

namespace dy_examples
{
#ifdef DY_EXAMPLE_HOSTED
dyf::Platform::Window* ActiveExampleWindow() noexcept;
bool ExampleRunning(const dyf::Platform::Window&) noexcept;
bool BeginExampleFrame(dyf::Platform::Window&);
void RequestExampleClose(dyf::Platform::Window&);
bool CancelExampleClose(dyf::Platform::Window&);
void CancelExampleNavigation();
void DrawExampleControls();
bool ExampleAction(dyf::Platform::Key, unsigned kind);
bool ExampleLooking();
bool ExampleKey(const dyf::Platform::Window&, dyf::Platform::Key, unsigned kind);
bool ExampleButton(const dyf::Platform::Window&, dyf::Platform::MouseButton, unsigned kind);
dyf::Platform::InputPoint ExamplePointer(const dyf::Platform::Window&, unsigned kind);
const std::vector<dyf::Platform::InputEvent>& ExampleEvents(const dyf::Platform::Window&);
#else
inline dyf::Platform::Window* ActiveExampleWindow() noexcept { return nullptr; }
inline bool ExampleRunning(const dyf::Platform::Window& window) noexcept { return window.IsRunning(); }
inline bool BeginExampleFrame(dyf::Platform::Window& window) { return window.IsRunning(); }
inline void RequestExampleClose(dyf::Platform::Window& window) { window.RequestClose(); }
inline bool CancelExampleClose(dyf::Platform::Window& window) { return window.CancelClose(); }
inline void CancelExampleNavigation() {}
inline void DrawExampleControls() {}
inline bool ExampleAction(dyf::Platform::Key, unsigned) { return false; }
inline bool ExampleLooking() { return false; }
inline bool ExampleKey(const dyf::Platform::Window& window, dyf::Platform::Key key, unsigned kind)
{ const auto& input=window.GetInput(); return kind==0?input.IsDown(key):kind==1?input.WasPressed(key):input.WasReleased(key); }
inline bool ExampleButton(const dyf::Platform::Window& window, dyf::Platform::MouseButton key, unsigned kind)
{ const auto& input=window.GetInput(); return kind==0?input.IsDown(key):kind==1?input.WasPressed(key):input.WasReleased(key); }
inline dyf::Platform::InputPoint ExamplePointer(const dyf::Platform::Window& window, unsigned kind)
{ const auto& input=window.GetInput(); return kind==0?input.GetCursorPosition():kind==1?input.GetCursorDelta():input.GetScrollDelta(); }
inline const std::vector<dyf::Platform::InputEvent>& ExampleEvents(const dyf::Platform::Window& window)
{ return window.GetInput().GetEvents(); }
#endif

class ExampleWindowLease
{
public:
    ExampleWindowLease(unsigned width,unsigned height,const char* title)
    {
        window=ActiveExampleWindow();
        if(!window) { owned=std::make_unique<dyf::Platform::Window>(width,height,title); window=owned.get(); }
        else window->SetTitle(title);
    }
    dyf::Platform::Window& Get() const { return *window; }
private:
    std::unique_ptr<dyf::Platform::Window> owned;
    dyf::Platform::Window* window=nullptr;
};

// A module reads GUI actions and physical input through the same local view.
// No key events are sent to the compositor or to another application.
class ExampleInputView
{
public:
    explicit ExampleInputView(const dyf::Platform::Window& value):window(value) {}
    bool IsDown(dyf::Platform::Key key) const { return ExampleKey(window,key,0); }
    bool WasPressed(dyf::Platform::Key key) const { return ExampleKey(window,key,1); }
    bool WasReleased(dyf::Platform::Key key) const { return ExampleKey(window,key,2); }
    bool IsDown(dyf::Platform::MouseButton key) const { return ExampleButton(window,key,0); }
    bool WasPressed(dyf::Platform::MouseButton key) const { return ExampleButton(window,key,1); }
    bool WasReleased(dyf::Platform::MouseButton key) const { return ExampleButton(window,key,2); }
    dyf::Platform::InputPoint GetCursorPosition() const { return ExamplePointer(window,0); }
    dyf::Platform::InputPoint GetCursorDelta() const { return ExamplePointer(window,1); }
    dyf::Platform::InputPoint GetScrollDelta() const { return ExamplePointer(window,2); }
    const std::vector<dyf::Platform::InputEvent>& GetEvents() const { return ExampleEvents(window); }
    const std::u32string& GetTextInput() const { return window.GetInput().GetTextInput(); }
private:
    const dyf::Platform::Window& window;
};
inline ExampleInputView ExampleInput(const dyf::Platform::Window& window) { return ExampleInputView(window); }
}

#define DY_EXAMPLE_WINDOW(name,width,height,title) \
    dy_examples::ExampleWindowLease name##Lease(width,height,title); \
    auto& name=name##Lease.Get()
