#pragma once

#include "ArenaActors.h"
#include "dyf/Extends/Model/ModelScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace showcase::arena
{
namespace model_detail
{
inline constexpr float pi = 3.14159265358979323846f;

inline void Require(bool success, const char* message)
{
    if(!success) throw std::runtime_error(message);
}

inline uint32_t FindClip(const dyf::ModelAsset& asset, std::initializer_list<const char*> names)
{
    for(const auto* name : names)
        for(uint32_t i = 0; i < asset.animations.size(); ++i)
            if(asset.animations[i].name == name && asset.animations[i].duration > 0) return i;
    throw std::runtime_error("KayKit character is missing a required named animation.");
}

inline dyf::MaterialDesc PropMaterial(const dyf::ModelData& model, uint32_t index)
{
    if(index >= model.materials.size()) throw std::runtime_error("KayKit prop has no material.");
    const auto& source = model.materials[index];
    auto material = source.material;
    const auto texture = source.textureIndices[dyf::ToIndex(dyf::MaterialTextureKind::BaseColor)];
    if(texture >= model.textures.size() || !model.textures[texture].IsValid())
        throw std::runtime_error("KayKit prop is missing its embedded color atlas.");
    material.baseColorTexture = model.textures[texture];
    material.baseColorTexture.SetColorSpace(dyf::ColorSpace::Srgb);
    return material;
}

struct PropPlacement
{
    dyf::Math::float3 position;
    float yaw = 0;
    float scale = 1;
};

// Static decorations use ordinary Scene instances, so they add no animation work.
inline void AddProp(dyf::ModelScene& scene, const std::filesystem::path& path,
    const std::vector<PropPlacement>& placements)
{
    using namespace dyf;
    ModelData model;
    if(!LoadModel(path.string(), model))
        throw std::runtime_error("Cannot load KayKit prop: " + path.string());
    std::vector<NodeTransform> localPose;
    std::vector<Math::float4x4> globalPose;
    for(const auto& node : model.nodes) localPose.push_back(node.bindTransform);
    Require(BuildGlobalNodeMatrices(model.nodes, localPose, globalPose), "Invalid KayKit prop hierarchy.");
    for(const auto& part : model.meshes)
    {
        const auto geometry = part.geometryIndex == UINT32_MAX ? &part :
            part.geometryIndex < model.meshes.size() ? &model.meshes[part.geometryIndex] : nullptr;
        if(!geometry || geometry->mesh.vertices.empty() || geometry->mesh.indices.empty()) continue;
        auto local = model.assetTransform;
        if(part.nodeIndex < globalPose.size()) local = local * globalPose[part.nodeIndex];
        const auto material = PropMaterial(model, part.materialIndex);
        EntityHandle source;
        for(const auto& placement : placements)
        {
            const auto transform = Math::Translation(placement.position) * Math::RotationZ(placement.yaw) *
                Math::Scaling(placement.scale) * Math::RotationX(pi * .5f) * local;
            auto entity = source ? scene.AddInstance(source, transform) : scene.Add(geometry->mesh, material, transform);
            Require(bool(entity), "Cannot instantiate KayKit dungeon decoration.");
            if(!source) source = entity;
        }
    }
}
}

inline void AddKayKitProps(dyf::ModelScene& scene, const std::filesystem::path& assets,
    std::vector<ArenaObstacle>* obstacles = nullptr)
{
    using namespace model_detail;
    std::vector<PropPlacement> barrels, chests, columns;
    for(int x : {-1, 1})
        for(int y : {-1, 1})
        {
            barrels.push_back({{x * 13.8f, y * 12.8f, .035f}, x * y * .3f, .64f});
            chests.push_back({{x * 11.2f, y * 14.3f, .035f}, y > 0 ? pi : 0.f, .8f});
            if(obstacles)
            {
                obstacles->push_back({barrels.back().position, .68f});
                obstacles->push_back({chests.back().position, .86f});
            }
            // The short columns sit beyond the walkable boundary, flanking each gate.
            columns.push_back({{x * 4.1f, y * 17.1f, .035f}, 0, 1.45f});
            columns.push_back({{x * 17.1f, y * 4.1f, .035f}, 0, 1.45f});
        }
    AddProp(scene, assets / "barrel_large.gltf.glb", barrels);
    AddProp(scene, assets / "chest.glb", chests);
    AddProp(scene, assets / "column.gltf.glb", columns);
}

class ArenaModels
{
public:
    ArenaModels(dyf::ModelScene& scene, const std::filesystem::path& assets, uint32_t capacity)
        : m_scene(scene), m_enemies(capacity)
    {
        m_assets[0] = LoadAsset(assets / "Knight.glb", 1.9f, false);
        m_assets[1] = LoadAsset(assets / "Skeleton_Minion.glb", 1.8f, true);
        m_assets[2] = LoadAsset(assets / "Skeleton_Warrior.glb", 2.5f, true);
        m_player = CreateActor(m_assets[0], true);
    }

    void Update(const ArenaActors& game, float dt)
    {
        if(!std::isfinite(dt) || dt < 0) throw std::runtime_error("Invalid arena animation delta.");
        const auto& poses = game.EnemyPoses();
        if(poses.size() > m_enemies.size()) throw std::runtime_error("Arena model capacity exceeded.");
        bool changed = UpdateActor(m_player, game.PlayerPose(), m_assets[0]);
        for(size_t i = 0; i < m_enemies.size(); ++i)
        {
            const ArenaActorPose* pose = i < poses.size() ? &poses[i] : nullptr;
            auto& slot = m_enemies[i];
            const uint32_t variant = pose && pose->kind == 1 ? 1u : 0u;
            if(pose && pose->alive && !dyf::IsValid(slot[variant].instance))
            {
                slot[variant] = CreateActor(m_assets[variant + 1], false);
                changed = true;
            }
            for(uint32_t type = 0; type < slot.size(); ++type)
            {
                auto& actor = slot[type];
                if(!dyf::IsValid(actor.instance)) continue;
                if(pose && variant == type) changed = UpdateActor(actor, *pose, m_assets[type + 1]) || changed;
                else changed = HideActor(actor) || changed;
            }
        }
        // Frozen A/B comparisons retain both object transforms and the sampled pose.
        if(dt > 0 || changed || m_dirty)
        {
            model_detail::Require(m_scene.UpdateAnimations(dt), "Cannot update KayKit character animations.");
            m_dirty = false;
        }
    }

    void Reset()
    {
        const auto reset = [&](Actor& actor) {
            if(!dyf::IsValid(actor.instance)) return;
            auto& instance = m_scene.GetModelInstance(actor.instance);
            instance.playback.time = 0;
            actor.clip = UINT32_MAX;
            actor.phase = 0;
            actor.hasPose = false;
        };
        reset(m_player);
        for(auto& slot : m_enemies)
            for(auto& actor : slot) reset(actor);
        m_dirty = true;
    }

private:
    struct Asset
    {
        dyf::ModelAssetID id = dyf::ModelAssetID::Invalid;
        float size = 1;
        uint32_t idle = UINT32_MAX, run = UINT32_MAX, attack = UINT32_MAX, spell = UINT32_MAX;
    };

    struct Actor
    {
        dyf::ModelInstanceID instance = dyf::ModelInstanceID::Invalid;
        dyf::Math::float4x4 base = dyf::Math::float4x4::Identity();
        ArenaActorPose pose{};
        uint32_t clip = UINT32_MAX;
        float phase = 0;
        bool hasPose = false;
    };

    Asset LoadAsset(const std::filesystem::path& path, float size, bool skeleton)
    {
        Asset result;
        if(!dyf::LoadModelAsset(m_scene, path.string(), {}, &result.id))
            throw std::runtime_error("Cannot load KayKit character: " + path.string());
        result.size = size;
        const auto& asset = m_scene.GetModelAsset(result.id);
        result.idle = model_detail::FindClip(asset, {"Idle_Combat", "Idle", "Unarmed_Idle"});
        result.run = model_detail::FindClip(asset, {"Running_A", "Walking_A", "Walking_D_Skeletons"});
        result.attack = skeleton ? model_detail::FindClip(asset, {"Unarmed_Melee_Attack_Punch_A", "1H_Melee_Attack_Chop"}) :
            model_detail::FindClip(asset, {"1H_Melee_Attack_Slice_Horizontal", "1H_Melee_Attack_Chop"});
        result.spell = model_detail::FindClip(asset, {"Spellcasting", "Spellcast_Shoot", "Spellcast_Long"});
        return result;
    }

    Actor CreateActor(const Asset& asset, bool hero)
    {
        using namespace dyf;
        using namespace model_detail;
        Actor actor;
        ModelSceneDesc desc;
        desc.normalizedSize = asset.size;
        desc.transform = Math::RotationX(pi * .5f);
        Require(InstantiateModel(m_scene, asset.id, desc, &actor.instance), "Cannot instantiate KayKit character.");
        auto& instance = m_scene.GetModelInstance(actor.instance);
        const auto& source = m_scene.GetModelAsset(asset.id);
        if(hero)
        {
            // The source includes mutually exclusive equipment meshes. Keep one sword
            // and one shield; detached bindings remain hidden without per-frame edits.
            auto& bindings = instance.bindings;
            bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [&](const ModelEntityBinding& binding) {
                if(binding.nodeIndex >= source.nodes.size()) return false;
                const auto& name = source.nodes[binding.nodeIndex].name;
                const bool alternate = name == "1H_Sword_Offhand" || name == "2H_Sword" ||
                    name == "Badge_Shield" || name == "Rectangle_Shield" || name == "Spike_Shield";
                if(alternate)
                {
                    Require(m_scene.GetEntity(binding.entity).SetTransform(Math::Translation({0, 0, -10000}) *
                        m_scene.GetTransform(binding.entity)), "Cannot hide alternate Knight equipment.");
                    Require(m_scene.GetEntity(binding.entity).SetLighting({false, false}),
                        "Cannot exclude unused Knight equipment from shadows.");
                }
                return alternate;
            }), bindings.end());
        }
        // KayKit faces +Z in its Y-up GLB. X rotation makes that -Y, so turn
        // it once by pi to match ArenaActors' local +Y forward convention.
        actor.base = Math::RotationZ(pi) * instance.rootTransform;
        float minZ = std::numeric_limits<float>::infinity();
        for(const auto& binding : instance.bindings)
        {
            const auto mesh = ToIndex(m_scene.GetEntityMesh(binding.entity));
            if(mesh >= m_scene.Meshes().size() || !m_scene.Meshes()[mesh] || binding.nodeIndex >= instance.globalPose.size()) continue;
            const auto transform = actor.base * instance.globalPose[binding.nodeIndex];
            for(const auto& vertex : m_scene.Meshes()[mesh]->vertices)
                minZ = std::min(minZ, Math::TransformPoint(transform, vertex.position).z);
        }
        if(!std::isfinite(minZ)) throw std::runtime_error("KayKit character has no finite foot bounds.");
        actor.base = Math::Translation({0, 0, -minZ + .035f}) * actor.base;
        instance.rootTransform = Math::Translation({0, 0, -10000}) * actor.base;
        actor.phase = 0;
        return actor;
    }

    bool HideActor(Actor& actor)
    {
        if(actor.hasPose && !actor.pose.alive) return false;
        auto& instance = m_scene.GetModelInstance(actor.instance);
        instance.rootTransform = dyf::Math::Translation({0, 0, -10000}) * actor.base;
        instance.playback.playing = false;
        actor.pose.alive = false;
        actor.hasPose = true;
        return true;
    }

    bool UpdateActor(Actor& actor, const ArenaActorPose& pose, const Asset& asset)
    {
        using namespace dyf;
        if(!pose.alive) return HideActor(actor);
        auto& instance = m_scene.GetModelInstance(actor.instance);
        const bool spawned = !actor.hasPose || !actor.pose.alive;
        const bool moved = spawned || actor.pose.position.x != pose.position.x || actor.pose.position.y != pose.position.y ||
            actor.pose.position.z != pose.position.z || actor.pose.yaw != pose.yaw;
        if(moved) instance.rootTransform = Math::Translation(pose.position) * Math::RotationZ(pose.yaw) * actor.base;
        const auto clip = pose.attacking ? (pose.kind == 2 ? asset.spell : asset.attack) :
            pose.moving ? asset.run : asset.idle;
        const bool switched = spawned || actor.clip != clip;
        if(switched)
        {
            model_detail::Require(m_scene.PlayAnimation(actor.instance, clip, true), "Cannot play KayKit animation.");
            actor.clip = clip;
            if(!pose.attacking)
                instance.playback.time = actor.phase * m_scene.GetModelAsset(asset.id).animations[clip].duration;
        }
        instance.playback.playing = true;
        actor.pose = pose;
        actor.hasPose = true;
        return moved || switched;
    }

    dyf::ModelScene& m_scene;
    std::array<Asset, 3> m_assets;
    Actor m_player;
    std::vector<std::array<Actor, 2>> m_enemies;
    bool m_dirty = true;
};
}
