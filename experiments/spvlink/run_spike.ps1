param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

& (Join-Path $root "scripts\spvlink_spike.ps1") -Root $root

if (-not $SkipBuild) {
    cmake -S $root -B (Join-Path $root "build")
    cmake --build (Join-Path $root "build") --config Debug

    $exe = Join-Path $root "build\Debug\spvlink_verify.exe"
    if (-not (Test-Path $exe)) {
        $exe = Join-Path $root "build\spvlink_verify.exe"
    }
    if (-not (Test-Path $exe)) {
        throw "spvlink_verify executable was not produced"
    }

    & $exe `
        (Join-Path $root "out\fullscreen.vert.spv") `
        (Join-Path $root "out\glsl_warm.final.spv") `
        (Join-Path $root "out\glsl_cool.final.spv") `
        (Join-Path $root "out\slang_warm.final.spv")
}
