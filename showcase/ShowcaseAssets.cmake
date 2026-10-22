include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/AssetDownload.cmake")

function(dyf_prepare_arena_assets destination)
    # Pin the official source revisions; the files are shared by every build.
    set(actors "https://raw.githubusercontent.com/KayKit-Game-Assets/KayKit-Character-Pack-Adventures-1.0/672074b73ba276876a19e8816ecdc5241817ab47")
    set(skeletons "https://raw.githubusercontent.com/KayKit-Game-Assets/KayKit-Character-Pack-Skeletons-1.0/15b62b9bad122f72926c10fb14d622c73819fa54")
    set(dungeon "https://raw.githubusercontent.com/KayKit-Game-Assets/KayKit-Dungeon-Remastered-1.0/b0ca9bd96a8072ab36a3a5464f00ed1e06a16d07")
    set(actor_models "${actors}/addons/kaykit_character_pack_adventures/Characters/gltf")
    set(skeleton_models "${skeletons}/addons/kaykit_character_pack_skeletons/Characters/gltf")
    set(dungeon_files "${dungeon}/addons/kaykit_dungeon_remastered/Assets")
    dyf_download_asset_file("${destination}/Knight.glb" "${actor_models}/Knight.glb")
    dyf_download_asset_file("${destination}/knight_texture.png" "${actor_models}/knight_texture.png")
    dyf_download_asset_file("${destination}/Adventurers-LICENSE.txt" "${actors}/LICENSE.txt")
    foreach(name Skeleton_Minion.glb Skeleton_Warrior.glb skeleton_texture.png)
        dyf_download_asset_file("${destination}/${name}" "${skeleton_models}/${name}")
    endforeach()
    dyf_download_asset_file("${destination}/Skeletons-LICENSE.txt" "${skeletons}/LICENSE.txt")
    foreach(name barrel_large.gltf.glb column.gltf.glb chest.glb)
        dyf_download_asset_file("${destination}/${name}" "${dungeon_files}/gltf/${name}")
    endforeach()
    dyf_download_asset_file("${destination}/dungeon_texture.png" "${dungeon_files}/texture/dungeon_texture.png")
    dyf_download_asset_file("${destination}/Dungeon-LICENSE.txt" "${dungeon}/LICENSE.txt")
endfunction()

function(dyf_prepare_cinematic_assets)
    # Download built-in bundles only when the corresponding path is selected.
    # Custom model/environment paths remain owned and supplied by the caller.
    set(bistro "${DY_ASSET_ROOT}/Bistro_v5_2")
    set(need_bistro FALSE)
    foreach(name BistroExterior.fbx BistroInterior.fbx BistroInterior_Wine.fbx)
        if(DY_SHOWCASE_SCENE STREQUAL "${bistro}/${name}")
            set(need_bistro TRUE)
        endif()
    endforeach()
    if(DY_SHOWCASE_ENVIRONMENT STREQUAL "${bistro}/san_giuseppe_bridge_4k.hdr")
        set(need_bistro TRUE)
    endif()
    if(need_bistro)
        dyf_download_asset_archive("${bistro}"
            "https://developer.nvidia.com/bistro" Bistro_v5_2.zip
            ROOT Bistro_v5_2 REQUIRED_FILES
                BistroExterior.fbx BistroInterior.fbx BistroInterior_Wine.fbx
                san_giuseppe_bridge_4k.hdr LICENSE.txt
                Textures/Antenna_Metal_BaseColor.dds)
    endif()

    set(emerald "${DY_ASSET_ROOT}/EmeraldSquare_v4_1")
    if(DY_SHOWCASE_EMERALD_SCENE STREQUAL "${emerald}/EmeraldSquare_Day.fbx" OR
            DY_SHOWCASE_EMERALD_ENVIRONMENT STREQUAL "${emerald}/symmetrical_garden_4k.hdr")
        dyf_download_asset_archive("${emerald}"
            "https://developer.nvidia.com/emerald-square" EmeraldSquare_v4_1.zip
            ROOT EmeraldSquare_v4_1 REQUIRED_FILES
                EmeraldSquare_Day.fbx symmetrical_garden_4k.hdr LICENSE.txt
                Textures/511_Awning_BlueBlack_BaseColor.dds)
    endif()

    set(knight "${DY_ASSET_ROOT}/Intel_Animated_Knight")
    if(DY_SHOWCASE_KNIGHT_ACTOR STREQUAL "${knight}/pkg_e_knight_anim/Exports/FBX/Knight_USD_002.fbx")
        dyf_download_asset_archive("${knight}"
            "https://cdrdv2.intel.com/v1/dl/getContent/763175" Intel_Animated_Knight.zip
            REQUIRED_FILES pkg_e_knight_anim/Exports/FBX/Knight_USD_002.fbx
                pkg_e_knight_anim/Textures/cloth_diffuse_D.png
                pkg_e_knight_anim/credits_license.txt)
    endif()

endfunction()
