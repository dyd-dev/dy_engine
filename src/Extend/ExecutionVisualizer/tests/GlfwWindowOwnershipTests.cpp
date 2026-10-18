#include <windows.h>
#include <GLFW/glfw3.h>
#include <cstdio>

struct _GLFWwindow
{
    _GLFWwindow* next=nullptr;
    char keys[GLFW_KEY_LAST+1]{};
    struct { HWND handle=nullptr; } win32;
};
static struct
{
    _GLFWwindow* windowListHead=nullptr;
    struct { int scancodes[GLFW_KEY_LAST+1]{}; } win32;
} _glfw;
static HWND active=nullptr;
static HANDLE property=nullptr;
static int propertyReads=0,releases=0;
static _GLFWwindow* releasedWindow=nullptr;
static HWND TestActiveWindow() { return active; }
static _GLFWwindow* TestProperty(HWND,LPCWSTR) { ++propertyReads; return static_cast<_GLFWwindow*>(property); }
static SHORT TestKeyState(int) { return 0; }
static int getKeyMods() { return 0; }
static void _glfwInputKey(_GLFWwindow* window,int key,int,int action,int)
{
    window->keys[key]=char(action); ++releases; releasedWindow=window;
}
#define GetActiveWindow TestActiveWindow
#define GetPropW TestProperty
#define GetKeyState TestKeyState
static void CurrentModifierBlock()
{
    HWND handle;
    _GLFWwindow* window;
#include "GlfwCurrentModifiers.inc"
}
static void OriginalModifierBlock()
{
    HWND handle;
    _GLFWwindow* window;
#include "GlfwOriginalModifiers.inc"
}
static DWORD Invoke(void (*block)())
{
    __try { block(); return 0; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
static bool Check(bool condition,const char* reason)
{
    if(!condition) std::fprintf(stderr,"FAIL GLFW ownership: %s\n",reason);
    return condition;
}
int main()
{
    _GLFWwindow first,second,foreign;
    first.next=&second; first.win32.handle=reinterpret_cast<HWND>(1); second.win32.handle=reinterpret_cast<HWND>(2);
    _glfw.windowListHead=&first;
    active=reinterpret_cast<HWND>(3); property=reinterpret_cast<HANDLE>(1);
    if(!Check(Invoke(OriginalModifierBlock)==EXCEPTION_ACCESS_VIOLATION,"Negative control must reproduce original invalid property dereference")) return 1;
    propertyReads=releases=0;
    if(!Check(Invoke(CurrentModifierBlock)==0,"Foreign active HWND with inaccessible GLFW property must be ignored")) return 1;
    if(!Check(!propertyReads && !releases,"Foreign property must never be read or sent key events")) return 1;
    property=&foreign; foreign.keys[GLFW_KEY_LEFT_SHIFT]=GLFW_PRESS;
    if(!Check(Invoke(CurrentModifierBlock)==0 && !releases && foreign.keys[GLFW_KEY_LEFT_SHIFT]==GLFW_PRESS,
        "A separate GLFW instance in the same process must remain untouched")) return 1;
    active=second.win32.handle; property=reinterpret_cast<HANDLE>(1); second.keys[GLFW_KEY_LEFT_SHIFT]=GLFW_PRESS;
    if(!Check(Invoke(CurrentModifierBlock)==0 && releases==1 && releasedWindow==&second && !propertyReads
        && second.keys[GLFW_KEY_LEFT_SHIFT]==GLFW_RELEASE,"Owned HWND must still release its modifier without trusting its property")) return 1;
    active=nullptr;
    if(!Check(Invoke(CurrentModifierBlock)==0 && releases==1,"No active HWND must remain a no-op")) return 1;
    std::puts("PASS actual GLFW modifier block: original AV reproduced under SEH; foreign and separate-instance HWND rejected; owned modifier release preserved; no windows or input created");
    return 0;
}
