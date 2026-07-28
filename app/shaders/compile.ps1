param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

$sdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$compiler = Get-ChildItem -Path $sdkBin -Filter fxc.exe -Recurse |
    Where-Object { $_.DirectoryName -like '*\x64' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $compiler) {
    throw "The Windows SDK HLSL compiler was not found under $sdkBin."
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
& $compiler /nologo /Ges /O3 /T vs_5_0 /E VertexMain /Fh (Join-Path $OutputDirectory 'genie_vertex_shader.hpp') /Vn g_genie_vertex_shader $Source
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $compiler /nologo /Ges /O3 /T ps_5_0 /E PixelMain /Fh (Join-Path $OutputDirectory 'genie_pixel_shader.hpp') /Vn g_genie_pixel_shader $Source
exit $LASTEXITCODE
