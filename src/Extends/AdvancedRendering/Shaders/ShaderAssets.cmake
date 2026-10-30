set(DY_ADVANCED_SHADER_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(DY_ADVANCED_SHADER_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND DY_ADVANCED_SHADER_GENERATED_DIR "/$<CONFIG>")
endif()
set(DY_ADVANCED_SHADER_SHARED_SOURCES
    "${DY_ADVANCED_SHADER_SOURCE_DIR}/probe_visibility.glsl"
    "${DY_ADVANCED_SHADER_SOURCE_DIR}/probe_visibility.hlsl"
    "${DY_ADVANCED_SHADER_SOURCE_DIR}/probe_visibility.metal")
# 같은 디렉터리의 include를 재컴파일 의존성에 넣고 기본 타깃에는 전파하지 않는다.
list(APPEND DY_STOCK_SHADER_SHARED_SOURCES ${DY_ADVANCED_SHADER_SHARED_SOURCES})
set(DY_ADVANCED_SHADER_NATIVE_SOURCES)
foreach(language IN ITEMS glsl hlsl metal)
    foreach(program IN ITEMS surface_vs surface_ps surface_debug screen_space_vs screen_space_ps
        environment_vs environment_diffuse_ps environment_specular_ps environment_background_ps
        clustered_lighting_cs
        transparent_vs transparent_ps transparent_copy_vs transparent_copy_ps transparent_depth_copy_ps
        temporal_vertex temporal_resolve bloom_down bloom_up temporal_filmic exposure_meter
        volumetric_fog screen_denoise)
        set(source "${DY_ADVANCED_SHADER_SOURCE_DIR}/${program}.${language}")
        if(NOT EXISTS "${source}")
            message(FATAL_ERROR "Missing ${language} AdvancedRendering shader: ${source}")
        endif()
        list(APPEND DY_ADVANCED_SHADER_NATIVE_SOURCES "${source}")
    endforeach()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${DY_ADVANCED_SHADER_NATIVE_SOURCES} ${DY_ADVANCED_SHADER_SHARED_SOURCES})

# 선택 확장의 셰이더 생성은 기본 타깃의 셰이더 목록·출력 경로와 분리한다.
if(DY_BACKEND_NORMALIZED STREQUAL "NULL")
    return()
endif()
# 공용 컴파일·임베드 함수를 이 하위 디렉터리의 소스·출력 경로로 재사용한다.
set(DY_STOCK_SHADER_SOURCE_DIR "${DY_ADVANCED_SHADER_SOURCE_DIR}")
set(DY_STOCK_SHADER_GENERATED_DIR "${DY_ADVANCED_SHADER_GENERATED_DIR}")
set(DY_STOCK_SHADER_HEADERS)
if(DY_BACKEND_NORMALIZED STREQUAL "METAL")
    dy_embed_stock_metal_library(EnvironmentLibrary kEnvironmentLibrary
        environment_vs environment_diffuse_ps environment_specular_ps environment_background_ps)
    dy_embed_stock_metal_library(ClusteredLightingCompute kClusteredLightingCompute clustered_lighting_cs)
    dy_embed_stock_metal_library(ScreenSpaceMetalLibrary kScreenSpaceMetalLibrary
        surface_vs surface_ps surface_debug screen_space_vs screen_space_ps
        INVARIANT_PROGRAMS surface_vs)
    dy_embed_stock_metal_library(TransparentLibrary kTransparentLibrary
        transparent_vs transparent_ps transparent_copy_vs transparent_copy_ps transparent_depth_copy_ps
        INVARIANT_PROGRAMS transparent_vs)
    dy_embed_stock_metal_library(TransparentNoAreaFragment kTransparentNoAreaFragment transparent_ps
        -DRENDERER_ENABLE_AREA_LIGHTS=0 -DTRANSPARENT_FRAGMENT_ENTRY=transparentNoAreaFragment)
    dy_embed_stock_metal_library(TransparentDepthFragment kTransparentDepthFragment transparent_ps
        -DTRANSPARENT_DEPTH_ONLY=1)
    dy_embed_stock_metal_library(TemporalVertex kTemporalVertex temporal_vertex)
    dy_embed_stock_metal_library(TemporalResolve kTemporalResolve temporal_resolve)
    dy_embed_stock_metal_library(BloomDown kBloomDown bloom_down)
    dy_embed_stock_metal_library(BloomUp kBloomUp bloom_up)
    dy_embed_stock_metal_library(TemporalFilmic kTemporalFilmic temporal_filmic)
    dy_embed_stock_metal_library(ExposureMeterFragment kExposureMeterFragment exposure_meter)
    dy_embed_stock_metal_library(VolumetricFogFragment kVolumetricFogFragment volumetric_fog)
    dy_embed_stock_metal_library(ScreenDenoiseTemporal kScreenDenoiseTemporal screen_denoise)
    dy_embed_stock_metal_library(ScreenDenoiseSpatial kScreenDenoiseSpatial screen_denoise)
else()
    if(DY_BACKEND_NORMALIZED STREQUAL "D3D12")
        set(shader_extension hlsl)
    elseif(DY_BACKEND_NORMALIZED STREQUAL "VULKAN")
        set(shader_extension glsl)
    else()
        message(FATAL_ERROR "Unsupported AdvancedRendering shader backend: ${DY_BACKEND_NORMALIZED}")
    endif()
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/environment_vs.${shader_extension}" vert EnvironmentVertex kEnvironmentVertex)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/environment_diffuse_ps.${shader_extension}" frag EnvironmentDiffuseFragment kEnvironmentDiffuseFragment)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/environment_specular_ps.${shader_extension}" frag EnvironmentSpecularFragment kEnvironmentSpecularFragment)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/environment_background_ps.${shader_extension}" frag EnvironmentBackgroundFragment kEnvironmentBackgroundFragment)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/clustered_lighting_cs.${shader_extension}" comp ClusteredLightingCompute kClusteredLightingCompute)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/surface_vs.${shader_extension}" vert SurfaceVertex kSurfaceVertex)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/surface_ps.${shader_extension}" frag SurfaceFragment kSurfaceFragment)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/surface_ps.${shader_extension}" frag SurfaceFullFragment kSurfaceFullFragment -DSURFACE_FULL_OUTPUTS=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/surface_debug.${shader_extension}" frag SurfaceDebugFragment kSurfaceDebugFragment)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/transparent_vs.${shader_extension}" vert TransparentVertex kTransparentVertex)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/transparent_ps.${shader_extension}" frag TransparentFragment kTransparentFragment)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/transparent_ps.${shader_extension}" frag TransparentNoAreaFragment kTransparentNoAreaFragment -DRENDERER_ENABLE_AREA_LIGHTS=0)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/transparent_ps.${shader_extension}" frag TransparentDepthFragment kTransparentDepthFragment -DTRANSPARENT_DEPTH_ONLY=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/transparent_copy_vs.${shader_extension}" vert TransparentCopyVertex kTransparentCopyVertex)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/transparent_copy_ps.${shader_extension}" frag TransparentCopyFragment kTransparentCopyFragment)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/transparent_depth_copy_ps.${shader_extension}" frag TransparentDepthCopyFragment kTransparentDepthCopyFragment)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/screen_space_vs.${shader_extension}" vert ScreenSpaceVertex kScreenSpaceVertex)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/screen_space_ps.${shader_extension}" frag ScreenSpaceFragment kScreenSpaceFragment)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/screen_space_ps.${shader_extension}" frag ScreenSpaceLightingFragment kScreenSpaceLightingFragment -DSCREEN_SPACE_LIGHTING=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/screen_space_ps.${shader_extension}" frag ScreenDepthCopyFragment kScreenDepthCopyFragment -DSCREEN_DEPTH_COPY=1)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/temporal_vertex.${shader_extension}" vert TemporalVertex kTemporalVertex)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/temporal_resolve.${shader_extension}" frag TemporalResolve kTemporalResolve)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/bloom_down.${shader_extension}" frag BloomDown kBloomDown)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/bloom_up.${shader_extension}" frag BloomUp kBloomUp)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/temporal_filmic.${shader_extension}" frag TemporalFilmic kTemporalFilmic)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/exposure_meter.${shader_extension}" frag ExposureMeterFragment kExposureMeterFragment)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/volumetric_fog.${shader_extension}" frag VolumetricFogFragment kVolumetricFogFragment)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/screen_denoise.${shader_extension}" frag ScreenDenoiseTemporal kScreenDenoiseTemporal -DSCREEN_DENOISE_SPATIAL=0)
    dy_embed_stock_shader("${DY_STOCK_SHADER_SOURCE_DIR}/screen_denoise.${shader_extension}" frag ScreenDenoiseSpatial kScreenDenoiseSpatial -DSCREEN_DENOISE_SPATIAL=1)
endif()
add_custom_target(dy_advanced_rendering_shaders DEPENDS ${DY_STOCK_SHADER_HEADERS})
add_dependencies(dy_extend_advanced_rendering dy_advanced_rendering_shaders)
target_include_directories(dy_extend_advanced_rendering PRIVATE "${DY_ADVANCED_SHADER_GENERATED_DIR}")
