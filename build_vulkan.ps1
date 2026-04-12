Set-Location "C:\GitHub\GitHub_Project\dy_engine"
cmake -S . -B build -D B_VULKAN=ON
cmake --build build
& ".\build\Debug\dy_engine_Example.exe"
