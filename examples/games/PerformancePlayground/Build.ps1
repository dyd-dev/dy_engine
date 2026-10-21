param([string]$BuildDirectory = (Join-Path $PSScriptRoot '../../../build/performance-playground-native'))
$ErrorActionPreference = 'Stop'
$engine = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$arguments = @('-S',$PSScriptRoot,'-B',$BuildDirectory,'-G','Visual Studio 17 2022','-A','x64')
# Reuse this checkout's already downloaded sources when available; normal CMake FetchContent is the fallback.
$dependencies = @{
    GLFW='build/demo-viewer-d3d12/_deps/glfw-src'
    STB='build/demo-viewer-d3d12/_deps/stb-src'
    FASTGLTF='out/model-cleanup-20261003/build/_deps/fastgltf-src'
    UFBX='out/model-cleanup-20261003/build/_deps/ufbx-src'
    BCDEC='build/merge-prep-d3d12/_deps/bcdec-src'
    MESHOPTIMIZER='build/merge-prep-d3d12/_deps/meshoptimizer-src'
    TRACY='build/tool-integration-deps/tracy'
}
foreach($name in $dependencies.Keys) {
    $source = Join-Path $engine $dependencies[$name]
    if(Test-Path -LiteralPath $source) { $arguments += '-DFETCHCONTENT_SOURCE_DIR_'+$name+'='+$source }
}
$dxc = Get-Command dxc -ErrorAction SilentlyContinue
$compiler = if($dxc) {$dxc.Source} elseif($env:VULKAN_SDK) {Join-Path $env:VULKAN_SDK 'Bin/dxc.exe'}
if(-not $compiler) {
    $compiler = Get-ChildItem -LiteralPath 'C:/VulkanSDK' -Filter dxc.exe -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
}
if($compiler) { $arguments += '-DDY_DXC='+$compiler; $arguments += '-DDY_MODEL_DXC='+$compiler }
& cmake @arguments
if($LASTEXITCODE -ne 0) {throw 'CMake configure failed'}
& cmake --build $BuildDirectory --config Release --parallel 4 -- /nodeReuse:false
if($LASTEXITCODE -ne 0) {throw 'Release build failed'}
& ctest --test-dir $BuildDirectory -C Release --output-on-failure
if($LASTEXITCODE -ne 0) {throw 'Verification failed'}
Write-Output ('Executable: '+[IO.Path]::GetFullPath((Join-Path $BuildDirectory 'Release/PerformancePlayground.exe')))
