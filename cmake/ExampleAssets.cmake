include_guard(GLOBAL)

function(dy_example_assets target source destination)
    if(NOT destination MATCHES "^[A-Za-z0-9_-]+$")
        message(FATAL_ERROR "Asset destination must be a simple directory name")
    endif()
    get_filename_component(input_directory "${source}" ABSOLUTE)
    set_property(TARGET "${target}" APPEND PROPERTY DY_CI_INPUTS "${input_directory}")
    add_custom_target("ci_assets_${target}_${destination}" ALL
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${source}"
            "-DDESTINATION=$<TARGET_FILE_DIR:${target}>/${destination}"
            "-DBUILD_ROOT=${CMAKE_BINARY_DIR}" -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/SyncAssets.cmake"
        VERBATIM)
    add_dependencies(${target} "ci_assets_${target}_${destination}")
endfunction()

# Register assets independently of CI so asset-only edits also reach local builds.
function(dy_sync_example_assets)
    set(directories "${CMAKE_CURRENT_SOURCE_DIR}")
    while(directories)
        list(POP_FRONT directories directory)
        get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
        list(APPEND directories ${children})
        get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            get_target_property(type "${target}" TYPE)
            if(NOT type STREQUAL "EXECUTABLE")
                continue()
            endif()
            get_target_property(source_directory "${target}" SOURCE_DIR)
            foreach(assets Assets Models Textures)
                if(IS_DIRECTORY "${source_directory}/${assets}" AND NOT TARGET "ci_assets_${target}_${assets}")
                    dy_example_assets("${target}" "${source_directory}/${assets}" "${assets}")
                endif()
            endforeach()
        endforeach()
    endwhile()
endfunction()
