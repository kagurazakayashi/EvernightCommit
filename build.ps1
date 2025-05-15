<#
.SYNOPSIS
    构建 EvernightCommit（Windows x64 Git GUI，C++20 / CMake）。
.DESCRIPTION
    使用 CMakePresets 构建，默认 Visual Studio 2026 生成器 + Release，无需开发者命令提示符。
    VS 2026 生成器需要 CMake 4.2+（VS 2026 自带实测可用）；只有 VS 2022 时改用 -Preset vs2022。
    产物：build\<preset>-x64\bin\<Config>\EvernightCommit.exe
.EXAMPLE
    .\build.ps1                 # vs2026 + Release
    .\build.ps1 -Preset vs2022  # 只有 VS 2022 的机器
    .\build.ps1 -Config Debug   # Debug 构建（含测试程序 gc_tests.exe）
#>
[CmdletBinding()]
param(
    [ValidateSet('vs2026', 'vs2022')]
    [string]$Preset = 'vs2026',
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw '找不到 cmake.exe：请安装 CMake（或 Visual Studio 的 C++ CMake 组件）并加入 PATH。'
}

Write-Host "配置：cmake --preset ${Preset}-x64" -ForegroundColor Cyan
& cmake --preset "${Preset}-x64"
if ($LASTEXITCODE -ne 0) { throw "cmake 配置失败（退出码 $LASTEXITCODE）。" }

Write-Host "构建：cmake --build --preset ${Preset}-$($Config.ToLower())" -ForegroundColor Cyan
& cmake --build --preset "${Preset}-$($Config.ToLower())"
if ($LASTEXITCODE -ne 0) { throw "cmake 构建失败（退出码 $LASTEXITCODE）。" }

$exe = Join-Path $root "build\${Preset}-x64\bin\$Config\EvernightCommit.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "构建完成但找不到产物：$exe" }
Write-Host "`n产物: $exe" -ForegroundColor Green
