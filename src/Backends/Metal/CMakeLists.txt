# ===== ===== Requirements ===== =====
# message(STATUS "[${PROJECT_NAME}] Find Metal Package...")

# ===== ===== Backend Target ===== =====
set(BACKEND_NAME "dy_backend_metal")

add_library(${BACKEND_NAME} OBJECT
    MetalDevice.mm
    MetalBuffer.mm
    MetalTexture.mm
    MetalShader.mm
    MetalPipeline.mm
    MetalResourceSet.mm
    MetalCommandList.mm
)

set_source_files_properties(
    MetalDevice.mm
    MetalBuffer.mm
    MetalTexture.mm
    MetalShader.mm
    MetalPipeline.mm
    MetalResourceSet.mm
    MetalCommandList.mm
    PROPERTIES COMPILE_FLAGS "-x objective-c++"
)

target_link_libraries(${BACKEND_NAME} PRIVATE
    dy_engine_options
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    "-framework AppKit"
    "-framework Metal"
    "-framework Foundation"
    "-framework QuartzCore"
)

target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:${BACKEND_NAME}>)
message(STATUS "[${PROJECT_NAME}] Ready to Use ${BACKEND_NAME}")
