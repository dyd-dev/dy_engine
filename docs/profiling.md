# Profiling and frame capture

The renderer publishes CPU/frame and completed GPU samples every 200 ms. The HUD
shows the average and the maximum frame time, excludes pauses of at least one
second, and appends one frame/CPU/GPU history point per published sample. GPU history
leaves gaps where samples are unavailable; the graphs retain the 16.67 ms reference
and adapt their scale. Applications that call `Diagnostics::UpdateProfilerControls`
after polling input use F11 to toggle their renderer's expanded HUD. Window and
Renderer do not interpret diagnostic shortcuts themselves. GPU timing distinguishes pending samples from
an unsupported backend. D3D12 and Vulkan implement the public timestamp query API;
Metal currently reports timestamp queries as unsupported.

Resource counters include buffers, textures and pipelines retained by recorded or
submitted GPU work. Destroying a public handle does not count as physical destruction
until the final reference is released. Swapchain-owned images are excluded.

`DY_ENABLE_TRACY=ON` enables CPU zones, frame marks and 5 Hz timing/resource plots.
`DY_TRACY_RAW_PLOTS=ON` additionally publishes per-frame timing plots. Timing and
Tracy publication do not depend on whether the HUD is visible.

Internal measurement and display continue when Tracy, RenderDoc, PIX and the Live
connection are all disabled. `Renderer::GetProfilerTimingSnapshot()` is the same
published sample used by the HUD, so another UI can read it without a tool SDK.
The SDK-free `dyf/Diagnostics/Profiler.h` exports scopes, frame marks and values;
the old `dyf/Platform/Profiler.h` forwards to it. SDK headers and build definitions
are private to the `dyf::ExternalProfilers` adapter target. `dy_engine` links that
target; instrumentation-only consumers can link it directly.

`dyf::LiveProfiling` contains collection, controls and immutable snapshots only.
`ReadLiveSnapshot()` needs no transport. `dyf::LiveClient` separately contains wire
encoding and the Windows named-pipe server/client; include
`dyf/Diagnostics/LiveConnection.h` to use `LiveClient` or own a connection.
Registering objects or enabling collection never starts a server.

`DY_ENABLE_LIVE_CONNECTION=ON` links that optional target into applications and
requires `DY_ENABLE_LIVE_PROFILING=ON`. It defaults to OFF independently of collection.
The app still owns startup and shutdown, for example with one `LiveConnectionScope`
in its main function. Showcase, the ImGui editor and AdvancedProfiling/Instancing
create that scope only when the connection option is enabled. Custom consumers
can instead link `dyf::LiveClient` and call `StartLiveConnection()` and
`StopLiveConnection()` explicitly. Disabling collection stops serving data;
re-enabling it requires another explicit start. Connections on other platforms
remain unsupported. No separate service process is launched.

The optional `dyf/Diagnostics/ProfilerControls.h` helper is called once after
`Window::PollEvents()` and before rendering:

```cpp
if(!dyf::Diagnostics::UpdateProfilerControls(window.GetInput(), renderer.get()))
    return 1;
```

Omit the renderer argument in a raw-RHI application to handle F12 alone. The helper
is installed in Showcase, the ImGui editor and AdvancedProfiling/Instancing;
other applications choose their own bindings. `Window` only queues native input.

`DY_ENABLE_RENDERDOC=ON` requires `renderdoc_app.h`; set `DY_RENDERDOC_ROOT` to its
installation/source directory. Launch the application through RenderDoc, then press
F12 in an application using the helper to request the next frame.
`dyf::Platform::RenderDocCapture` only accesses an
already loaded RenderDoc API: it does not load RenderDoc or start a service. The
bridge is unavailable on Metal/macOS; use Xcode GPU Capture there.

`DY_ENABLE_PIX=ON` enables D3D12 events and markers using WinPixEventRuntime
1.0.240308001 and requires MSVC. It defaults to ON with MSVC; other compilers can
build D3D12 rendering and GPU timing with PIX disabled. Project executables receive
the runtime DLL beside their executable, including parent-project executables
declared after `add_subdirectory(dy_engine)`. Installed or packaged applications
must also deploy `$<TARGET_FILE:WinPixEventRuntime>` with the application.
Public `ICommandList` events copy their labels at record time and preserve them
during deferred native replay. Vulkan maps the same events to optional debug-utils
labels. The shader/backend/Retina/depth-state fixes formerly tied to the old Metal
examples are provided by the current standalone Metal shaders and backend; no old
Graphics or Common example tree is restored.

`AdvancedProfiling --frames 120 --validation` exercises CPU statistics, frame marks,
GPU timestamps, events and fence-delayed readback. `--capture path.ppm` writes an
image readback; F12 requests a RenderDoc capture instead. These are separate outputs.
Use `DY_BUILD_PROFILING_TESTS=ON` and run `ctest --output-on-failure` for sampling,
resource lifetime, deferred events, supported native timestamp queries and the
RenderDoc bridge. RenderDoc-enabled tests inject a local fake API to check the bridge;
an actual RenderDoc/PIX/Xcode capture still requires the corresponding capture tool.
