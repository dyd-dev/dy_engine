#include "../WindowBackend.h"
#import <UIKit/UIKit.h>
#include <stdexcept>

namespace dyf::Platform
{
class IOSWindow final : public WindowBackend
{
public:
    IOSWindow(Window& owner,const WindowDescription& desc):WindowBackend(owner)
    {
        if(desc.borrowed.kind!=NativeWindowKind::UIKit || !desc.borrowed.value)
            throw std::invalid_argument("iOS Window wraps an existing UIKit UIView with a CAMetalLayer.");
        view=(__bridge UIView*)desc.borrowed.value;
        InputEvent event;event.type=InputEventType::Focus;event.focused=HasFocus();owner.HandleInputEvent(event);
    }
    bool IsRunning() const override { return view&&!closed; }
    void RequestClose() const override { closed=true; }
    void Resize(unsigned int,unsigned int) const override {}
    WindowSize GetSize() const override
    {return view?WindowSize{static_cast<int>(view.bounds.size.width),static_cast<int>(view.bounds.size.height)}:WindowSize{};}
    WindowSize GetFramebufferSize() const override
    {
        if(!view)return {};
        const auto scale=view.contentScaleFactor;
        return {static_cast<int>(view.bounds.size.width*scale),static_cast<int>(view.bounds.size.height*scale)};
    }
    ContentScale GetContentScale() const override
    {const auto scale=view?static_cast<float>(view.contentScaleFactor):1.f;return {scale,scale};}
    bool HasFocus() const override { return view.window&&UIApplication.sharedApplication.applicationState==UIApplicationStateActive; }
    bool IsMinimized() const override { return !view.window||view.window.hidden; }
    void SetCursorMode(CursorMode) override {}
    CursorMode GetCursorMode() const override { return CursorMode::Normal; }
    bool IsRawMouseMotionSupported() const override { return false; }
    bool SetRawMouseMotion(bool value) override { return !value; }
    bool IsRawMouseMotionEnabled() const override { return false; }
    NativeWindowHandle GetNativeHandle() const override { return {NativeWindowKind::UIKit,(__bridge void*)view}; }
private:
    __unsafe_unretained UIView* view=nil;
    mutable bool closed=false;
};
std::unique_ptr<WindowBackend> CreateWindowBackend(Window& owner,const WindowDescription& desc)
{ return std::make_unique<IOSWindow>(owner,desc); }
// UIKit owns dispatch. CADisplayLink invokes the application's frame callback.
void PollPlatformEvents() {}
void WaitPlatformEvents(double)
{ throw std::logic_error("UIKit owns iOS event dispatch; use the application lifecycle and CADisplayLink."); }
}
