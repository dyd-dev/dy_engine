#include <dyf/Extends/Model/Model.h>
#include <dyf/Extends/Model/ModelScene.h>
#include <dyf/Platform/Log.h>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace {
void Check(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
void Write(const fs::path& path, const std::string& text) { std::ofstream(path, std::ios::binary) << text; }
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
void Replace(std::string& text, const std::string& before, const std::string& after) {
    const auto pos = text.find(before); Check(pos != std::string::npos, "fixture replacement missing");
    text.replace(pos, before.size(), after);
}
void Near(float actual, float expected) { Check(std::fabs(actual - expected) < 1e-5f, "morph coefficient differs from pinned ufbx semantics"); }
void U32(std::string& data, uint32_t value) { for(unsigned i = 0; i < 4; ++i) data.push_back(char(value >> (i * 8))); }

// These are bounded ordinary assets. No malformed binary or unchecked-cast reproduction is generated.
void Paths(const fs::path& directory) {
    const std::string json = R"({"asset":{"version":"2.0"},"buffers":[{"uri":"triangle.bin","byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    const float triangle[] = {0,0,0,1,0,0,0,1,0};
    Write(directory / "triangle.bin", std::string(reinterpret_cast<const char*>(triangle), sizeof(triangle)));
    Write(directory / "triangle.gltf", json);
    std::string padded = json;
    while(padded.size() % 4) padded += ' ';
    std::string glb; U32(glb, 0x46546c67); U32(glb, 2); U32(glb, uint32_t(20 + padded.size()));
    U32(glb, uint32_t(padded.size())); U32(glb, 0x4e4f534a); glb += padded;
    Write(directory / "triangle.glb", glb);
    const auto previous = fs::current_path();
    struct Restore { fs::path path; ~Restore() { fs::current_path(path); } } restore{previous};
    fs::current_path(directory);
    for(const char* extension : {".gltf", ".glb"}) {
        const std::string name = std::string("triangle") + extension;
        for(const auto& path : {fs::path(name), fs::path(".") / name, directory / name}) {
            dyf::ModelData model;
            std::string diagnostic;
            dyf::Platform::Log::SetCallback([](const auto& record, void* context) {
                *static_cast<std::string*>(context) += record.message + '\n';
            }, &diagnostic);
            const bool loaded = dyf::LoadModel(path.string(), model);
            dyf::Platform::Log::SetCallback(nullptr);
            if(!loaded) throw std::runtime_error(path.string() + ": " + diagnostic);
            Check(model.meshes.size() == 1 && model.meshes[0].mesh.vertices.size() == 3, "path changed geometry");
            Near(model.meshes[0].mesh.vertices[1].position.x, 1.f);
        }
    }
}

void Cache(const fs::path& directory, const std::string& base) {
    auto tiny = base; Replace(tiny, "a: 0,50,100", "a: 0,0.000015,0.00003");
    const auto path = directory / "tiny.fbx"; Write(path, tiny);
    for(bool reverse : {false, true}) {
        dyf::ModelScene scene;
        dyf::ModelLoadOptions exact, tolerant; exact.fbxBakeRate = tolerant.fbxBakeRate = 2.f;
        exact.fbxConstantTrackTolerance = 0.f; tolerant.fbxConstantTrackTolerance = 4e-7f;
        dyf::ModelAssetID first, second, repeat;
        Check(dyf::LoadModelAsset(scene, path.string(), reverse ? tolerant : exact, &first), "first asset rejected");
        Check(dyf::LoadModelAsset(scene, path.string(), reverse ? exact : tolerant, &second), "second asset rejected");
        Check(first != second, "distinct float options collided in asset cache");
        Check(scene.GetModelAsset(reverse ? second : first).animations[0].morphTracks[0].weights.size() == 3, "exact track was collapsed");
        Check(scene.GetModelAsset(reverse ? first : second).animations[0].morphTracks[0].weights.size() == 1, "tolerant track was not collapsed");
        Check(dyf::LoadModelAsset(scene, path.string(), reverse ? tolerant : exact, &repeat) && first == repeat, "identical options lost cache hit");
        exact.fbxBakeRate = 0.5f; dyf::ModelAssetID rateA, rateB;
        Check(dyf::LoadModelAsset(scene, path.string(), exact, &rateA), "rate A rejected");
        exact.fbxBakeRate = std::nextafter(0.5f, 1.f);
        Check(dyf::LoadModelAsset(scene, path.string(), exact, &rateB) && rateA != rateB, "adjacent bake rates collided");
        exact.fbxConstantTrackTolerance = 0.f; dyf::ModelAssetID positiveZero, negativeZero;
        Check(dyf::LoadModelAsset(scene, path.string(), exact, &positiveZero), "positive zero rejected");
        exact.fbxConstantTrackTolerance = -0.f;
        Check(dyf::LoadModelAsset(scene, path.string(), exact, &negativeZero) && positiveZero != negativeZero,
            "bit identity policy lost signed zero");
    }
}

void Morph(const fs::path& directory, const std::string& base) {
    struct Case { const char* targets; float input, first, second; };
    // Hand-calculated coefficients from ufbx's zero anchor and two-endpoint extrapolation.
    const Case cases[] = {{"-100,100",-1,1,0},{"-100,100",-.5f,.5f,0},{"-100,100",0,0,0},
        {"-100,100",.5f,0,.5f},{"-100,100",1,0,1},{"50,100",0,0,0},{"50,100",.25f,.5f,0},
        {"50,100",.75f,.5f,.5f},{"50,100",1,0,1},{"50,100",1.5f,-1,2},
        {"-100,-50",-1.5f,2,-1},{"50,100",-.5f,-1,0},{"0,100",0,0,0},{"100,100",1,0,0}};
    for(const auto& test : cases) {
        auto text = base;
        Replace(text, "FullWeights: *1", "FullWeights: *2"); Replace(text, "a: 100", std::string("a: ") + test.targets);
        Replace(text, "    Deformer: 4,", "    Geometry: 10, \"Geometry::Second\", \"Shape\" {\n        Indexes: *3 { a: 0,1,2 }\n        Vertices: *9 { a: 0,2,0,0,2,0,0,2,0 }\n    }\n    Deformer: 4,");
        Replace(text, "    C: \"OO\",3,5", "    C: \"OO\",3,5\n    C: \"OO\",10,5");
        const auto percent = std::to_string(test.input * 100.f);
        Replace(text, "\"A\",50", "\"A\"," + percent);
        Replace(text, "DeformPercent: 50", "DeformPercent: " + percent);
        Replace(text, "a: 0,50,100", "a: " + percent + ',' + percent + ',' + percent);
        const auto path = directory / "progressive.fbx"; Write(path, text);
        dyf::ModelData model; dyf::ModelLoadOptions options; options.fbxBakeRate = 2.f;
        Check(dyf::LoadModel(path.string(), model, options), "progressive morph rejected");
        const auto& mesh = model.meshes.at(0);
        Check(mesh.defaultMorphWeights.size() == 2, "progressive shapes missing");
        Near(mesh.defaultMorphWeights[0], test.first); Near(mesh.defaultMorphWeights[1], test.second);
        const auto& tracks = model.animations.at(0).morphTracks;
        Check(tracks.size() == 2, "progressive animated targets missing");
        Near(tracks[0].weights.at(0).value, test.first); Near(tracks[1].weights.at(0).value, test.second);
    }
}

// Run only against the guarded implementation: tiny finite targets and ordinary bounded
// channel inputs exercise rejection before any out-of-range double-to-float conversion.
void MorphRange(const fs::path& directory, const std::string& base) {
    struct Case { const char* defaultPercent; const char* animatedPercent; bool accepted; };
    for(const auto& test : {Case{"0.1", "0,0.05,0.1", true}, Case{"50", "0,0,0", false},
        Case{"0", "0,25,50", false}}) {
        auto text = base;
        Replace(text, "FullWeights: *1", "FullWeights: *2");
        Replace(text, "a: 100", "a: 1e-38,2e-38");
        Replace(text, "    Deformer: 4,", "    Geometry: 10, \"Geometry::Second\", \"Shape\" {\n        Indexes: *3 { a: 0,1,2 }\n        Vertices: *9 { a: 0,2,0,0,2,0,0,2,0 }\n    }\n    Deformer: 4,");
        Replace(text, "    C: \"OO\",3,5", "    C: \"OO\",3,5\n    C: \"OO\",10,5");
        Replace(text, "\"A\",50", std::string("\"A\",") + test.defaultPercent);
        Replace(text, "DeformPercent: 50", std::string("DeformPercent: ") + test.defaultPercent);
        Replace(text, "a: 0,50,100", std::string("a: ") + test.animatedPercent);
        const auto path = directory / "morph-range.fbx"; Write(path, text);
        dyf::ModelData model; dyf::ModelLoadOptions options; options.fbxBakeRate = 2.f;
        std::string diagnostic;
        dyf::Platform::Log::SetCallback([](const auto& record, void* context) {
            *static_cast<std::string*>(context) += record.message + '\n';
        }, &diagnostic);
        const bool loaded = dyf::LoadModel(path.string(), model, options);
        dyf::Platform::Log::SetCallback(nullptr);
        Check(loaded == test.accepted, "morph coefficient range boundary was not enforced");
        if(loaded) {
            const auto& mesh = model.meshes.at(0);
            Check(mesh.defaultMorphWeights.size() == 2 && mesh.defaultMorphWeights[0] < -1e36f
                && mesh.defaultMorphWeights[1] > 1e36f, "valid finite extrapolation was clamped");
            for(float weight : mesh.defaultMorphWeights) Check(std::isfinite(weight), "non-finite default coefficient escaped");
            for(const auto& track : model.animations.at(0).morphTracks)
                for(const auto& key : track.weights) Check(std::isfinite(key.value), "non-finite animated coefficient escaped");
        } else {
            Check(model.meshes.empty() && model.nodes.empty() && model.animations.empty(), "range rejection left partial output");
            Check(diagnostic.find("coefficients") != std::string::npos, "range rejection did not report coefficient error");
        }
    }
}

void Samples(const fs::path& directory, const std::string& base) {
    const auto path = directory / "samples.fbx"; Write(path, base);
    dyf::ModelData model; dyf::ModelLoadOptions options;
    options.fbxBakeRate = 99999.f;
    Check(dyf::LoadModel(path.string(), model, options), "100000-sample boundary rejected");
    Check(model.animations.at(0).morphTracks.at(0).weights.size() == 100000, "bake boundary sample count changed");
    Near(model.animations[0].morphTracks[0].weights.back().time, 1.f);
    options.fbxBakeRate = 100000.f;
    Check(!dyf::LoadModel(path.string(), model, options) && model.meshes.empty(), "over-limit bake accepted");
    for(float rate : {0.f, -1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        options.fbxBakeRate = rate; Check(!dyf::LoadModel(path.string(), model, options), "invalid rate accepted");
    }
    auto still = base;
    Replace(still, "\"LocalStop\", \"KTime\", \"Time\", \"\",46186158000", "\"LocalStop\", \"KTime\", \"Time\", \"\",0");
    Replace(still, "\"ReferenceStop\", \"KTime\", \"Time\", \"\",46186158000", "\"ReferenceStop\", \"KTime\", \"Time\", \"\",0");
    Write(path, still); options.fbxBakeRate = 2.f;
    Check(dyf::LoadModel(path.string(), model, options), "zero-duration animation rejected");
    Check(model.animations.at(0).morphTracks.at(0).weights.size() == 1, "zero duration did not produce one sample");
}

uint64_t MinimumBudget(const fs::path& path) {
    uint64_t low = 0, high = 16384;
    dyf::ModelLoadOptions options; dyf::ModelData model;
    options.maxDecodedBytes = high; Check(dyf::LoadModel(path.string(), model, options), "budget search fixture rejected");
    while(low < high) { const auto mid = (low + high) / 2; options.maxDecodedBytes = mid;
        if(dyf::LoadModel(path.string(), model, options)) high = mid; else low = mid + 1;
    }
    return low;
}
void Textures(const fs::path& directory) {
    const auto path = directory / "textured.obj";
    Write(path, "mtllib texture.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl first\nf 1 2 3\n");
    Write(directory / "texture.mtl", "newmtl first\nKd 1 1 1\nmap_Kd pixel.bmp\n");
    Write(directory / "pixel.bmp", "unsupported image");
    const auto geometryBytes = MinimumBudget(path);
    std::string bmp = "BM"; U32(bmp, 118); U32(bmp, 0); U32(bmp, 54); U32(bmp, 40); U32(bmp, 4); U32(bmp, 4);
    bmp.append("\1\0\40\0", 4); U32(bmp, 0); U32(bmp, 64); bmp.append(16, '\0'); bmp.append(64, char(255));
    Write(directory / "pixel.bmp", bmp);
    Check(MinimumBudget(path) == geometryBytes + 64, "external pixels excluded from combined decoded budget");
    dyf::ModelData model; dyf::ModelLoadOptions options; options.maxDecodedBytes = geometryBytes + 64;
    Check(dyf::LoadModel(path.string(), model, options), "exact combined budget rejected");
    Check(model.textures.size() == 1 && model.textures[0].IsValid(), "bounded external pixels not retained");
    options.maxDecodedBytes--; Check(!dyf::LoadModel(path.string(), model, options), "combined budget overage accepted");
    Write(directory / "pixel.bmp", "unsupported image");
    Check(dyf::LoadModel(path.string(), model), "unsupported texture changed LoadModel contract");
    dyf::ModelScene scene; Check(!dyf::LoadModelAsset(scene, path.string(), {}, nullptr), "unsupported texture accepted by asset loader");

    // Distinct references to one canonical file share immutable pixels and one budget charge.
    Write(directory / "texture.mtl", "newmtl first\nKd 1 1 1\nmap_Kd pixel.bmp\nnewmtl second\nKd 1 1 1\nmap_Kd ./pixel.bmp\n");
    Write(path, "mtllib texture.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl first\nf 1 2 3\nusemtl second\nf 1 2 3\n");
    const auto sharedGeometry = MinimumBudget(path);
    Write(directory / "pixel.bmp", bmp);
    Check(MinimumBudget(path) == sharedGeometry + 64, "shared image charged more than once");
    Check(dyf::LoadModel(path.string(), model), "shared image load failed");
    Check(model.textures.size() == 2 && &model.textures[0].GetPixels() == &model.textures[1].GetPixels(), "canonical images do not share pixels");
    Write(directory / "texture.mtl", "newmtl first\nKd 1 1 1\nmap_Kd pixel.bmp\nnewmtl second\nKd 1 1 1\nmap_Kd other.bmp\n");
    Write(directory / "other.bmp", bmp);
    Check(MinimumBudget(path) == sharedGeometry + 128, "distinct image pixels were not summed");
}
}

int main(int argc, char** argv) {
    try {
        Check(argc >= 3, "usage: P2ModelLoadTests FBX_BASE OUTPUT_DIRECTORY [case]");
        dyf::Platform::Log::SetDefaultOutputEnabled(false);
        const auto base = Read(argv[1]); const auto directory = fs::absolute(argv[2]); fs::create_directories(directory);
        const std::string selected = argc > 3 ? argv[3] : "all";
        if(selected == "all" || selected == "paths") Paths(directory);
        if(selected == "all" || selected == "cache") Cache(directory, base);
        if(selected == "all" || selected == "morph") Morph(directory, base);
        if(selected == "all" || selected == "morph-range") MorphRange(directory, base);
        if(selected == "all" || selected == "samples") Samples(directory, base);
        if(selected == "all" || selected == "textures") Textures(directory);
        std::cout << "PASS " << selected << '\n'; return 0;
    } catch(const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
