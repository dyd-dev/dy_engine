Set-Location "C:\GitHub\GitHub_Project\REngine"
cmake -S . -B build -D B_VULKAN=ON
cmake --build build
& ".\build\Debug\SuRengine_Example.exe"
