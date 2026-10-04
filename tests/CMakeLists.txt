if(DY_BUILD_PROFILING_TESTS)
    add_executable(ProfilerSamplerTests ProfilerSamplerTest.cpp ../src/dyf/ProfilerSampler.cpp)
    target_link_libraries(ProfilerSamplerTests PRIVATE dy_engine_options)
    add_test(NAME ProfilingSampler COMMAND ProfilerSamplerTests)

    add_executable(ProfilingTests ProfilingTests.cpp)
    target_link_libraries(ProfilingTests PRIVATE dy_engine)
    if(USE_D3D12)
        target_compile_definitions(ProfilingTests PRIVATE DY_TEST_D3D12)
    endif()
    add_test(NAME ProfilingContracts COMMAND ProfilingTests)
    set_tests_properties(ProfilingContracts PROPERTIES TIMEOUT 30)

    add_executable(RenderDocCaptureTests RenderDocCaptureTests.cpp)
    target_link_libraries(RenderDocCaptureTests PRIVATE dy_engine ${CMAKE_DL_LIBS})
    add_test(NAME RenderDocAbsent COMMAND RenderDocCaptureTests --absent)
    if(DY_ENABLE_RENDERDOC)
        add_library(RenderDocTestProvider SHARED RenderDocTestProvider.cpp)
        target_include_directories(RenderDocTestProvider PRIVATE "${DY_RENDERDOC_INCLUDE_DIR}")
        set_target_properties(RenderDocTestProvider PROPERTIES OUTPUT_NAME renderdoc
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/renderdoc-stub"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/renderdoc-stub")
        if(WIN32)
            set_target_properties(RenderDocTestProvider PROPERTIES PREFIX "")
        endif()
        add_dependencies(RenderDocCaptureTests RenderDocTestProvider)
        add_test(NAME RenderDocInjected COMMAND RenderDocCaptureTests "$<TARGET_FILE:RenderDocTestProvider>")
    endif()
endif()

if(DY_BUILD_TESTS)
    add_executable(P2InputTests P2InputTests.cpp)
    target_link_libraries(P2InputTests PRIVATE dy_engine_options)
    add_test(NAME P2InputContracts COMMAND P2InputTests)

    add_executable(P2WindowTests P2WindowTests.cpp
        ../src/Platform/Window.cpp ../src/Platform/Input.cpp)
    target_link_libraries(P2WindowTests PRIVATE dy_engine_options)
    target_include_directories(P2WindowTests PRIVATE $<TARGET_PROPERTY:glfw,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(P2WindowTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN GLFW_INCLUDE_NONE)
    foreach(scenario IN ITEMS constructor constructor-shared release focus capture)
        add_test(NAME P2Window_${scenario} COMMAND P2WindowTests "${scenario}")
        set_tests_properties(P2Window_${scenario} PROPERTIES TIMEOUT 30)
    endforeach()

    # CPU contract tests use a Null native boundary and the production common RHI.
    if(NOT USE_D3D12 AND NOT USE_VULKAN AND NOT USE_METAL)
        add_executable(P2RhiTests P2RhiTests.cpp ../src/RHI/Resources.cpp)
        target_link_libraries(P2RhiTests PRIVATE dy_engine)
        target_include_directories(P2RhiTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
        add_test(NAME P2RhiContracts COMMAND P2RhiTests)
        set_tests_properties(P2RhiContracts PROPERTIES TIMEOUT 30 ENVIRONMENT "DY_LOG_AUTO=0")

        add_executable(P2FrontendTests P2FrontendTests.cpp
            ../src/dyf/Renderer.cpp ../src/dyf/CanvasRenderer.cpp ../src/dyf/DrawData.cpp
            ../src/dyf/TextureCache.cpp ../src/dyf/Lighting.cpp ../src/dyf/Profiler.cpp
            ../src/dyf/Scene.cpp ../src/dyf/Shaders.cpp ../src/dyf/PostProcess.cpp)
        target_link_libraries(P2FrontendTests PRIVATE dy_engine)
        target_include_directories(P2FrontendTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
        if(MSVC)
            target_compile_options(P2FrontendTests PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/P2FrontendAccess.h")
        else()
            target_compile_options(P2FrontendTests PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/P2FrontendAccess.h")
        endif()
        foreach(scenario IN ITEMS shader-views canvas-retry canvas-exception canvas-recovery
                default-texture-retry frame-upload-rollback profiler-cancel scene-append
                texture-exception draw-data-exception shadow-angles spot-angles)
            add_test(NAME P2Frontend_${scenario} COMMAND P2FrontendTests "${scenario}")
            set_tests_properties(P2Frontend_${scenario} PROPERTIES TIMEOUT 30 ENVIRONMENT "DY_LOG_AUTO=0")
        endforeach()
    endif()

    find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_executable(P2LogSessionTests P2LogSessionTests.cpp ../src/Platform/LogSession.cpp)
        target_link_libraries(P2LogSessionTests PRIVATE dy_engine_options Threads::Threads)
        add_test(NAME P2LogSessionPaths COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/P2LogSessionTests.py"
            --binary "$<TARGET_FILE:P2LogSessionTests>"
            --output "${CMAKE_CURRENT_BINARY_DIR}/p2-log-session")
        set_tests_properties(P2LogSessionPaths PROPERTIES TIMEOUT 60)

        get_property(p2_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        if(WIN32 AND MSVC AND p2_multi_config)
            add_test(NAME P2LogCapturePolicy COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/P2LogCaptureTests.py"
                --cmake "${CMAKE_COMMAND}" --repo "${PROJECT_SOURCE_DIR}"
                --generator "${CMAKE_GENERATOR}" --make-program "${CMAKE_MAKE_PROGRAM}"
                --work-dir "${CMAKE_CURRENT_BINARY_DIR}/p2-log-capture")
            set_tests_properties(P2LogCapturePolicy PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
        endif()
    endif()

    if(DY_EXTEND_MODEL)
        add_executable(GltfValidationTests GltfValidationTests.cpp)
        target_link_libraries(GltfValidationTests PRIVATE dyf::Model)
        add_test(NAME GltfValidationContracts COMMAND GltfValidationTests
            --fixture-dir "${CMAKE_CURRENT_BINARY_DIR}/gltf-fixtures")
        set_tests_properties(GltfValidationContracts PROPERTIES TIMEOUT 60)

        add_executable(FbxMorphWeightTests FbxMorphWeightTests.cpp)
        target_link_libraries(FbxMorphWeightTests PRIVATE dyf::Model)
        add_test(NAME FbxMorphWeights COMMAND FbxMorphWeightTests
            "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/FbxMorph")

        add_executable(P2ModelLoadTests P2ModelLoadTests.cpp)
        target_link_libraries(P2ModelLoadTests PRIVATE dyf::Model)
        add_test(NAME P2ModelLoadContracts COMMAND P2ModelLoadTests
            "${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/FbxMorph/animated.fbx"
            "${CMAKE_CURRENT_BINARY_DIR}/p2-model-fixtures")
        set_tests_properties(P2ModelLoadContracts PROPERTIES TIMEOUT 60 ENVIRONMENT "DY_LOG_AUTO=0")

        if(NOT USE_D3D12 AND NOT USE_VULKAN AND NOT USE_METAL)
            add_executable(P2ModelRendererTests P2ModelRendererTests.cpp)
            target_link_libraries(P2ModelRendererTests PRIVATE dyf::Model)
            target_include_directories(P2ModelRendererTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
            add_test(NAME P2ModelRendererContracts COMMAND P2ModelRendererTests)
            set_tests_properties(P2ModelRendererContracts PROPERTIES TIMEOUT 60 ENVIRONMENT "DY_LOG_AUTO=0")
        endif()
    endif()

    if(WIN32 AND USE_D3D12)
        add_executable(D3D12ReadbackLifetimeTests D3D12ReadbackLifetimeTests.cpp)
        target_include_directories(D3D12ReadbackLifetimeTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_compile_definitions(D3D12ReadbackLifetimeTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(D3D12ReadbackLifetimeTests PRIVATE dy_engine d3d12 dxgi dxguid)
        add_test(NAME D3D12ReadbackLifetime COMMAND D3D12ReadbackLifetimeTests)
        set_tests_properties(D3D12ReadbackLifetime PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

        add_executable(P2D3D12BackendTests P2D3D12BackendTests.cpp)
        target_include_directories(P2D3D12BackendTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_compile_definitions(P2D3D12BackendTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(P2D3D12BackendTests PRIVATE dy_engine d3d12 dxgi dxguid)
        add_test(NAME P2D3D12BackendContracts COMMAND P2D3D12BackendTests)
        set_tests_properties(P2D3D12BackendContracts PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE ENVIRONMENT "DY_LOG_AUTO=0")
    endif()

    if(WIN32 AND USE_VULKAN)
        find_package(Vulkan REQUIRED)
        add_executable(VulkanReadbackLifetimeTests VulkanReadbackLifetimeTests.cpp)
        target_include_directories(VulkanReadbackLifetimeTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_compile_definitions(VulkanReadbackLifetimeTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN GLFW_INCLUDE_NONE)
        target_link_libraries(VulkanReadbackLifetimeTests PRIVATE dy_engine Vulkan::Vulkan)
        foreach(scenario IN ITEMS success pre-submit-failure submit-oom wait-host-oom wait-device-oom wait-device-lost shutdown-unknown wait-callback-reentry)
            add_test(NAME VulkanReadback_${scenario} COMMAND VulkanReadbackLifetimeTests "${scenario}")
            set_tests_properties(VulkanReadback_${scenario} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
        endforeach()

        add_executable(P2VulkanBackendTests P2VulkanBackendTests.cpp)
        target_include_directories(P2VulkanBackendTests PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_compile_definitions(P2VulkanBackendTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN GLFW_INCLUDE_NONE)
        target_link_libraries(P2VulkanBackendTests PRIVATE dy_engine Vulkan::Vulkan glfw)
        add_test(NAME P2VulkanBackendContracts COMMAND P2VulkanBackendTests)
        set_tests_properties(P2VulkanBackendContracts PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE ENVIRONMENT "DY_LOG_AUTO=0")
    endif()

    add_executable(RenderGraphTests RenderGraphTests.cpp)
    target_link_libraries(RenderGraphTests PRIVATE dy_engine)
    add_test(NAME RenderGraphContracts COMMAND RenderGraphTests)
    set_tests_properties(RenderGraphContracts PROPERTIES TIMEOUT 60)

    add_executable(TessellationTests TessellationTests.cpp)
    target_link_libraries(TessellationTests PRIVATE dy_engine)
    if(USE_D3D12 OR USE_METAL OR USE_VULKAN)
        include(CompileShaders)
        set(tessellation_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Tessellation")
        foreach(shader IN ITEMS vertex hull domain fragment)
            if(shader STREQUAL "vertex")
                set(stage vert)
                set(metal_entry vertexMain)
            elseif(shader STREQUAL "fragment")
                set(stage frag)
                set(metal_entry fragmentMain)
            else()
                set(stage "${shader}")
                if(shader STREQUAL "hull")
                    set(metal_entry hullMain)
                else()
                    set(metal_entry domainMain)
                endif()
            endif()
            dy_compile_shader(TessellationTests "tess_${shader}" "${stage}"
                GLSL "${tessellation_shader_dir}/${shader}.glsl"
                HLSL "${tessellation_shader_dir}/${shader}.hlsl"
                METAL "${tessellation_shader_dir}/${shader}.metal"
                METAL_ENTRY "${metal_entry}"
                INCLUDE_DIRS "${tessellation_shader_dir}")
        endforeach()
        target_compile_definitions(TessellationTests PRIVATE DY_TEST_NATIVE_SHADERS=1)
    endif()
    add_test(NAME TessellationContracts COMMAND TessellationTests)
    set_tests_properties(TessellationContracts PROPERTIES TIMEOUT 30)
endif()
