#pragma once
#include "dyf/Scene.h"
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace editor
{
enum class Primitive { Cube, Sphere, Floor };
const char* PrimitiveName(Primitive);
struct Object
{
    Primitive primitive = Primitive::Cube;
    std::string name = "Cube";
    std::array<float,3> position = {0,0,.5f}, rotation = {0,0,0}, scale = {1,1,1};
    std::array<float,4> color = {.65f,.65f,.7f,1};
    float roughness = .35f, metallic = .05f;
};
dyf::Math::float4x4 Transform(const Object&);

class Document
{
public:
    std::vector<Object> objects;
    int selected = -1;
    std::filesystem::path path;
    bool playing = false, paused = false;
    std::unique_ptr<dyf::Scene> scene;

    Document();
    void New(bool examples = false);
    void Add(Primitive);
    void Duplicate();
    void Delete();
    bool Commit(const std::vector<Object>& before);
    bool Undo();
    bool Redo();
    bool CanUndo() const { return !undo.empty(); }
    bool CanRedo() const { return !redo.empty(); }
    bool Dirty() const;
    bool Save(const std::filesystem::path&, std::string& error);
    bool Load(const std::filesystem::path&, std::string& error);
    void Sync();
    void Play();
    void Tick(float seconds);
    void Stop();
    int Pick(dyf::Math::float3 origin, dyf::Math::float3 direction) const;
    std::string Contents() const;
private:
    // ponytail: at most 64 whole-scene snapshots; use per-object commands for large scenes.
    std::vector<std::vector<Object>> undo, redo;
    std::string saved;
    std::vector<Primitive> built;
    float previewTime = 0;
};
bool SelfTest(const std::filesystem::path& directory, std::string& error);
}
