if(DY_CI_SANITIZERS)
    if(MSVC)
        string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        target_compile_options(dy_engine_options INTERFACE /fsanitize=address)
        # All C++ dependencies must agree on MSVC STL sanitizer annotations.
        add_compile_options(/fsanitize=address)
        target_link_options(dy_engine_options INTERFACE /INCREMENTAL:NO)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            # CMake invokes lld-link directly, so add the runtime normally selected by the clang-cl driver.
            execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
                OUTPUT_VARIABLE clang_resources OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
            find_library(DY_ASAN_RUNTIME NAMES clang_rt.asan_dynamic-x86_64
                PATHS "${clang_resources}/lib/windows" NO_DEFAULT_PATH REQUIRED)
            find_library(DY_ASAN_THUNK NAMES clang_rt.asan_dynamic_runtime_thunk-x86_64
                PATHS "${clang_resources}/lib/windows" NO_DEFAULT_PATH REQUIRED)
            target_link_libraries(dy_engine_options INTERFACE "${DY_ASAN_RUNTIME}")
            target_link_options(dy_engine_options INTERFACE /INCLUDE:__asan_seh_interceptor "/WHOLEARCHIVE:${DY_ASAN_THUNK}")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(dy_engine_options INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(dy_engine_options INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
        message(FATAL_ERROR "Unsupported sanitizer toolchain")
    endif()
endif()
