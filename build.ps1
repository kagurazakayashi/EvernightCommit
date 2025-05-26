<#
.SYNOPSIS
    构建 EvernightCommit（Windows x64 Git GUI，C++20 / CMake）。
.DESCRIPTION
    使用 CMakePresets 构建，默认 Visual Studio 2026 生成器 + Release，无需开发者命令提示符。
    会自动选择一个真正提供该生成器的 cmake.exe：PATH 上的 `cmake` 可能是 MSYS2 构建，
    它不含任何 Visual Studio 生成器，直接拿去配置会在第一步就报“未知生成器”。
    产物：build\<preset>-x64\bin\<Config>\EvernightCommit.exe
.EXAMPLE
    .\build.ps1                 # vs2026 + Release
    .\build.ps1 -Preset vs2022  # 只有 VS 2022 的机器
    .\build.ps1 -Config Debug   # Debug 构建（含测试程序 gc_tests.exe）
    .\build.ps1 -CMake 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
#>
[CmdletBinding()]
param(
    [ValidateSet('vs2026', 'vs2022')]
    [string]$Preset = 'vs2026',
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [string]$CMake
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# 预设要求的具体生成器：判定候选 CMake 是否可用，看它列不列得出生成器，不看版本号。
$requiredGenerator = if ($Preset -eq 'vs2026') { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }

function Get-CmakeCandidate {
    param([string]$Explicit)
    $paths = @()
    if ($Explicit) { $paths += $Explicit }

    # Visual Studio 自带的 CMake 是 MSVC 构建，含 Visual Studio 生成器；按实例逐个探测，
    # 因为「C++ CMake tools」是可选组件，装没装不能假设。
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        foreach ($install in (& $vswhere -all -products * -property installationPath 2>$null)) {
            if (-not $install) { continue }
            $paths += Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        }
    }

    foreach ($cmd in (Get-Command cmake -All -ErrorAction SilentlyContinue)) { $paths += $cmd.Source }

    $paths | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique
}

function Test-CmakeGenerator {
    param([string]$Exe, [string]$Generator)
    $pattern = '(?m)^\s*\*?\s*' + [regex]::Escape($Generator) + '\s+='
    try {
        $help = & $Exe --help 2>$null | Out-String
    } catch {
        return $false
    }
    return ($help -match $pattern)
}

$candidates = @(Get-CmakeCandidate -Explicit $CMake)
$cmakeExe = $null
foreach ($candidate in $candidates) {
    if (Test-CmakeGenerator -Exe $candidate -Generator $requiredGenerator) { $cmakeExe = $candidate; break }
    Write-Host "跳过不提供 '$requiredGenerator' 生成器的 CMake：$candidate" -ForegroundColor DarkGray
}
if (-not $cmakeExe) {
    $checked = if ($candidates.Count) { "`n已检查：`n  " + ($candidates -join "`n  ") } else { '' }
    throw "找不到提供 '$requiredGenerator' 生成器的 cmake.exe$checked。" +
        '请安装 Visual Studio 的“C++ CMake tools for Windows”组件或独立的 CMake（vs2026 需 4.2 及以上），' +
        '或用 -CMake <cmake.exe 路径> 指定；只有 VS 2022 的机器改用 -Preset vs2022。'
}
Write-Host "使用 CMake：$cmakeExe（$(((& $cmakeExe --version | Select-Object -First 1) -join '').Trim())）" -ForegroundColor DarkCyan

Write-Host "配置：cmake --preset ${Preset}-x64" -ForegroundColor Cyan
& $cmakeExe --preset "${Preset}-x64" -S $root
if ($LASTEXITCODE -ne 0) { throw "cmake 配置失败（退出码 $LASTEXITCODE）。" }

Write-Host "构建：cmake --build --preset ${Preset}-$($Config.ToLower())" -ForegroundColor Cyan
& $cmakeExe --build --preset "${Preset}-$($Config.ToLower())"
if ($LASTEXITCODE -ne 0) { throw "cmake 构建失败（退出码 $LASTEXITCODE）。" }

$exe = Join-Path $root "build\${Preset}-x64\bin\$Config\EvernightCommit.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "构建完成但找不到产物：$exe" }
Write-Host "`n产物: $exe" -ForegroundColor Green
