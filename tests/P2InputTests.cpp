#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

// Only the event producer is replaced; assertions use the public input/action API.
#define private public
#include <dyf/Platform/Input.h>
#include <dyf/Platform/ActionMap.h>
#undef private
#include "../src/Platform/Input.cpp"
#include "../src/Platform/ActionMap.cpp"

static bool failNextAllocation = false;
void* operator new(std::size_t size)
{
    if(failNextAllocation) { failNextAllocation = false; throw std::bad_alloc(); }
    if(void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace dyf::Platform;
static int failures = 0;
void Check(bool condition, const char* message)
{
    if(!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void KeyEvent(Input& input, Key key, InputAction action)
{
    InputEvent event; event.type = InputEventType::Key;
    event.code = static_cast<int>(key); event.action = action;
    input.OnEvent(event);
}
void MouseEvent(Input& input, InputAction action)
{
    InputEvent event; event.type = InputEventType::MouseButton;
    event.code = static_cast<int>(MouseButton::Left); event.action = action;
    input.OnEvent(event);
}

void CaptureReleaseSurvivesOtherCapture()
{
    Input input;
    ActionMap map(input);
    map.BindButton(1, Key::A);
    KeyEvent(input, Key::A, InputAction::Press);
    input.PublishFrame();
    map.Update();
    Check(map.WasPressed(1) && map.IsDown(1), "key press must be visible before capture");
    map.Update({true, false});
    Check(map.WasReleased(1) && !map.IsDown(1), "keyboard capture must cancel held action");
    map.Update({true, true});
    Check(map.WasReleased(1) && !map.IsDown(1), "later mouse capture must preserve keyboard cancellation");
    input.PublishFrame();
    map.Update();
    Check(!map.WasReleased(1) && !map.IsDown(1), "next frame clears cancellation edge but preserves suppression");
    KeyEvent(input, Key::A, InputAction::Release);
    input.PublishFrame(); map.Update();
    KeyEvent(input, Key::A, InputAction::Press);
    input.PublishFrame(); map.Update();
    Check(map.IsDown(1) && map.WasPressed(1), "physical release must reactivate a captured key");
}

void NewlyCapturedTapIsHidden()
{
    Input input;
    ActionMap map(input);
    map.BindButton(1, Key::A);
    KeyEvent(input, Key::A, InputAction::Press);
    KeyEvent(input, Key::A, InputAction::Release);
    input.PublishFrame(); map.Update();
    Check(map.WasPressed(1) && map.WasReleased(1), "short tap exposes both edges before capture");
    map.Update({true, false});
    Check(!map.WasPressed(1) && !map.WasReleased(1), "newly captured raw tap must not retain raw release");
    map.Update({true, true});
    Check(!map.WasReleased(1), "unrelated later capture must not resurrect a captured tap");
}

void MouseAndMixedActions()
{
    Input input;
    ActionMap map(input);
    map.BindButton(1, MouseButton::Left);
    map.BindButton(2, Key::A); map.BindButton(2, MouseButton::Left);
    map.BindAxis(3, Key::A, Key::D);
    KeyEvent(input, Key::D, InputAction::Press);
    MouseEvent(input, InputAction::Press);
    input.PublishFrame(); map.Update();
    map.Update({false, true});
    Check(map.WasReleased(1) && map.WasReleased(2), "mouse capture cancels mouse and mouse-held mixed actions");
    Check(map.GetAxis(3) == 1.f, "mouse capture preserves a keyboard axis");
    map.Update({true, true});
    Check(map.WasReleased(1) && map.WasReleased(2), "keyboard capture preserves prior mouse cancellations");
    Check(map.GetAxis(3) == 0.f && map.WasReleased(3), "keyboard capture cancels the remaining axis");
}

void EventAllocationFailureKeepsReleaseState()
{
    Input input;
    KeyEvent(input, Key::A, InputAction::Press);
    input.PublishFrame();
    // Publish swaps storage; the pending event vector is empty with no allocation.
    bool threw = false;
    failNextAllocation = true;
    try { KeyEvent(input, Key::A, InputAction::Release); }
    catch(const std::bad_alloc&) { threw = true; }
    failNextAllocation = false;
    input.PublishFrame();
    Check(threw, "failed event storage remains observable at a C++ boundary");
    Check(!input.IsDown(Key::A) && input.WasReleased(Key::A), "release state must survive event storage failure");
}

void EventAllocationFailureKeepsFocusState()
{
    Input input;
    KeyEvent(input, Key::A, InputAction::Press);
    input.PublishFrame();
    InputEvent lost; lost.type = InputEventType::Focus; lost.focused = false;
    bool threw = false;
    failNextAllocation = true;
    try { input.OnEvent(lost); } catch(const std::bad_alloc&) { threw = true; }
    failNextAllocation = false;
    input.PublishFrame();
    Check(threw, "focus event storage failure remains observable");
    Check(!input.IsDown(Key::A) && input.WasReleased(Key::A), "focus loss cannot leave held keys after storage failure");
}

int main()
{
    CaptureReleaseSurvivesOtherCapture();
    NewlyCapturedTapIsHidden();
    MouseAndMixedActions();
    EventAllocationFailureKeepsReleaseState();
    EventAllocationFailureKeepsFocusState();
    std::printf("P2 input checks: %d failures\n", failures);
    return failures ? 1 : 0;
}
