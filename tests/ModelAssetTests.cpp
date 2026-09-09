#include "Graphics/Scene.h"
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
    using namespace dy::Graphics;
    try
    {
        if(argc != 4) throw std::runtime_error("Pass glTF, FBX and image paths");
        for(int i = 1; i <= 2; ++i)
        {
            ModelData model;
            const auto result = LoadModelDetailed(argv[i], model);
            if(!result.success)
            {
                for(const auto& diagnostic : result.diagnostics)
                    std::cerr << diagnostic.path << ": " << diagnostic.message << '\n';
                throw std::runtime_error("Bundled model import failed");
            }
            if(model.meshes.empty()) throw std::runtime_error("Imported model has no meshes");
            if(i == 1 && (model.skins.empty() || model.animations.empty()))
                throw std::runtime_error("Animated glTF lost its skin or clips");
            for(const auto& texture : model.textures)
                if(texture.rgba8.empty() && !texture.sourcePath.empty())
                {
                    TextureAsset pixels;
                    if(!LoadImage(texture.sourcePath, pixels)) throw std::runtime_error("Model texture is not self-contained");
                }
            std::cout << argv[i] << ": " << model.meshes.size() << " meshes, "
                << model.animations.size() << " animation clips\n";
        }
        TextureAsset image;
        if(!LoadImage(argv[3], image) || image.rgba8.size() != static_cast<size_t>(image.width) * image.height * 4)
            throw std::runtime_error("Bundled image import failed");
        return 0;
    }
    catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
