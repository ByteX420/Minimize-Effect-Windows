param(
    [Parameter(Mandatory = $true)]
    [string]$Source,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,
    [Parameter(Mandatory = $true)]
    [string]$WindowsSdkBinPath
)

$compilerCandidates = @(
    (Join-Path $WindowsSdkBinPath 'fxc.exe'),
    (Join-Path $WindowsSdkBinPath 'x64\fxc.exe')
)
$compiler = $compilerCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
    Select-Object -First 1
if (-not $compiler) {
    throw "The selected Windows SDK does not provide x64 fxc.exe. WindowsSdkBinPath=$WindowsSdkBinPath"
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
& $compiler /nologo /Ges /O3 /T vs_5_0 /E VertexMain /Fh (Join-Path $OutputDirectory 'genie_vertex_shader.hpp') /Vn g_genie_vertex_shader $Source
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $compiler /nologo /Ges /O3 /T ps_5_0 /E PixelMain /Fh (Join-Path $OutputDirectory 'genie_pixel_shader.hpp') /Vn g_genie_pixel_shader $Source
exit $LASTEXITCODE
