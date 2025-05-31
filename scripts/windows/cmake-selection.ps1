# Windows 构建入口共用的「挑一个真正提供目标生成器的 cmake.exe」逻辑。
# 被 build.ps1 与 test.ps1 dot-source，不单独执行。
#
# 为什么不能直接用 PATH 上的 cmake：MSYS2 等自带构建不含 Visual Studio 生成器，
# 而且它的 link.exe 会遮蔽 MSVC 链接器；判定标准是「列不列得出生成器」，不看版本号。
# 本文件不安装任何东西，找不到时只说明该装什么、怎么指定。

function Get-GcCmakeCandidate {
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

function Test-GcCmakeGenerator {
    param([string]$Exe, [string]$Generator)
    $pattern = '(?m)^\s*\*?\s*' + [regex]::Escape($Generator) + '\s+='
    try {
        $help = & $Exe --help 2>$null | Out-String
    } catch {
        return $false
    }
    return ($help -match $pattern)
}

# 返回 @{ CMake = <cmake.exe 路径>; Ctest = <同目录 ctest.exe> }；找不到提供目标生成器的那一个就抛。
function Resolve-GcCmake {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Generator,
        [string]$Explicit
    )
    $candidates = @(Get-GcCmakeCandidate -Explicit $Explicit)
    foreach ($candidate in $candidates) {
        if (Test-GcCmakeGenerator -Exe $candidate -Generator $Generator) {
            $ctest = Join-Path (Split-Path -Parent $candidate) 'ctest.exe'
            return @{ CMake = $candidate; Ctest = $ctest }
        }
        Write-Host "跳过不提供 '$Generator' 生成器的 CMake：$candidate" -ForegroundColor DarkGray
    }
    $checked = if ($candidates.Count) { "`n已检查：`n  " + ($candidates -join "`n  ") } else { '' }
    throw "找不到提供 '$Generator' 生成器的 cmake.exe$checked。" +
        '请安装 Visual Studio 的“C++ CMake tools for Windows”组件或独立的 CMake（vs2026 需 4.2 及以上），' +
        '或用 -CMake <cmake.exe 路径> 指定；只有 VS 2022 的机器改用 -Preset vs2022。'
}

# 预设名 → 该预设要求的具体生成器。判定候选 CMake 能不能用，看它列不列得出生成器。
function Get-GcRequiredGenerator {
    param([ValidateSet('vs2026', 'vs2022', 'ninja')][string]$Preset)
    switch ($Preset) {
        'vs2026' { 'Visual Studio 18 2026' }
        'vs2022' { 'Visual Studio 17 2022' }
        'ninja'  { 'Ninja Multi-Config' }
    }
}
