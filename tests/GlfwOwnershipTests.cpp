// All windows belong to this short-lived test process and stay hidden.
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include "dyf/Platform/Window.h"
#include <cstdio>
#include <stdexcept>

void Check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
int main()
{
    try {
        Check(glfwInit()==GLFW_TRUE,"host GLFW initialize");
        glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);
        auto* host=glfwCreateWindow(32,32,"audit-owned hidden host",nullptr,nullptr);
        Check(host!=nullptr,"hidden host create");
        const HWND hostHandle=glfwGetWin32Window(host);
        Check(IsWindow(hostHandle)&&!IsWindowVisible(hostHandle),"host is hidden");
        auto* peer=glfwCreateWindow(32,32,"audit-owned hidden peer",nullptr,nullptr);
        Check(peer!=nullptr,"hidden peer create");
        glfwDestroyWindow(peer);
        Check(IsWindow(hostHandle),"ordinary peer destruction must preserve host");
        {
            dyf::Platform::Window engine(32,32,"audit-owned hidden dyf window");
            const auto native=engine.GetNativeHandle();
            Check(native.kind==dyf::Platform::NativeWindowKind::Win32&&native.value,"dyf window create");
            Check(!IsWindowVisible(static_cast<HWND>(native.value)),"dyf window stayed hidden");
            Check(IsWindow(hostHandle),"host survived dyf create");
        }
        // IsWindow safely queries the saved OS handle; never dereference a GLFW handle after termination.
        const bool hostStillExists=IsWindow(hostHandle)!=FALSE;
        std::printf("OBSERVED ordinary_peer_destroy_host_alive=1 dyf_last_window_destroy_host_alive=%d\n",hostStillExists);
        if(hostStillExists)glfwDestroyWindow(host);
        glfwTerminate();
        Check(hostStillExists,"dyf destruction terminated the host GLFW window");
        std::puts("PASS independent hidden host window survives dyf window destruction; host owns GLFW termination.");
    }catch(const std::exception& e){std::fprintf(stderr,"GLFW ownership probe: %s\n",e.what());glfwTerminate();return 1;}
}
