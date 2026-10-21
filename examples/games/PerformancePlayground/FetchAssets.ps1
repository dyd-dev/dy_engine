param([string]$Cache = (Join-Path $PSScriptRoot '../../../build/performance-playground-assets'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$packs = @(
    @{id='commercial'; url='https://kenney.nl/media/pages/assets/city-kit-commercial/a742d900eb-1753115042/kenney_city-kit-commercial_2.1.zip'; hash='F8B09B081C2BB88BCC126E2DEC1CB40FD0DAD7E7E591B6C26AAEFE96FB35276B'; models=@('building-a','building-b','building-c','building-d','building-f','building-k','building-skyscraper-a','detail-awning','detail-awning-wide','detail-overhang','detail-parasol-a')},
    @{id='industrial'; url='https://kenney.nl/media/pages/assets/city-kit-industrial/0ec35b139d-1788171848/kenney_city-kit-industrial_2.0.zip'; hash='5B381164E5760F3830A2DBEE43B972DEEE38B2A695D091B56E238AB2910C96D2'; models=@('building-a','building-b','building-c','chimney-small','chimney-medium','detail-tank','shipping-container-a','solar-panel-landscape','water-tower','windmill-low')},
    @{id='building'; url='https://kenney.nl/media/pages/assets/building-kit/0de7aaa492-1743244741/kenney_building-kit.zip'; hash='2740EF5772FB5FB3D7AAB881DB22D129F6B68AFE711B1A79E6D5E9E19CF3EEC6'; models=@('wall','wall-window-square','wall-doorway-wide-square','floor','roof-flat-center','column','detail-pipe','gutter-vertical','plating-detailed','border')},
    @{id='characters'; url='https://kenney.nl/media/pages/assets/animated-characters-protagonists/608191acc4-1774773108/kenney_animated-characters-protagonists.zip'; hash='EC3787DE70FA2200256848D74201B10F6B6C3126594E9857BF989753312C2B84'; models=@()}
)
New-Item -ItemType Directory -Force -Path $Cache | Out-Null
$manifest = @()
foreach($pack in $packs) {
    $archive = Join-Path $Cache ($pack.id+'.zip')
    if(-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $pack.url -OutFile $archive }
    if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $pack.hash) { throw ('Archive hash mismatch: '+$archive) }
    $destination = Join-Path $PSScriptRoot ('Assets/Kenney/'+$pack.id)
    $files = @(@{source='License.txt'; target='License.txt'})
    foreach($model in $pack.models) { $files += @{source=('Models/GLB format/'+$model+'.glb'); target=($model+'.glb')} }
    if($pack.id -eq 'characters') {
        foreach($name in 'Model/characterMedium.fbx','Animations/idle.fbx','Animations/run.fbx','Skins/skaterMaleA.png') { $files += @{source=$name; target=$name} }
    } else { $files += @{source='Models/GLB format/Textures/colormap.png'; target='Textures/colormap.png'} }
    $zip = [System.IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($archive))
    try {
        foreach($file in $files) {
            $entry = $zip.GetEntry($file.source)
            if($null -eq $entry) { throw ('Missing asset: '+$pack.id+'/'+$file.source) }
            $output = Join-Path $destination $file.target
            New-Item -ItemType Directory -Force -Path (Split-Path $output) | Out-Null
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$output,$true)
            $manifest += [ordered]@{file=('Kenney/'+$pack.id+'/'+$file.target); archiveEntry=$file.source; bytes=$entry.Length; sha256=(Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash}
        }
    } finally { $zip.Dispose() }
    Write-Output ($pack.id+': checksum and '+$files.Count+' selected files OK')
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'Assets/manifest.json') -Encoding utf8
