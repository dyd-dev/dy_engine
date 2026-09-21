# Profiling and frame capture

The renderer publishes CPU/frame and completed GPU samples every 200 ms. The HUD
shows the average and the maximum frame time, excludes pauses of at least one
second, and appends one frame/CPU/GPU history point per published sample. GPU history
leaves gaps where samples are unavailable; the graphs retain the 16.67 ms reference
and adapt their scale. F11 toggles the expanded
HUD in the window receiving the key. GPU timing distinguishes pending samples from
an unsupported backend. D3D12 and Vulkan implement the public timestamp query API;
Metal currently reports timestamp queries as unsupported.

Resource counters include buffers, textures and pipelines retained by recorded or
submitted GPU work. Destroying a public handle does not count as physical destruction
until the final reference is released. Swapchain-owned images are excluded.

`DY_ENABLE_TRACY=ON` enables CPU zones, frame marks and 5 Hz timing/resource plots.
`DY_TRACY_RAW_PLOTS=ON` additionally publishes per-frame timing plots. Timing and
Tracy publication do not depend on whether the HUD is visible.

`DY_ENABLE_RENDERDOC=ON` requires `renderdoc_app.h`; set `DY_RENDERDOC_ROOT` to its
installation/source directory. Launch the application through RenderDoc, then press
F12 to request the next frame. `dyf::Platform::RenderDocCapture` only accesses an
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
