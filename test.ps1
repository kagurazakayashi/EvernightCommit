<#
.SYNOPSIS
    跑 EvernightCommit 的测试（Windows）—— 默认只跑「安全分组」，不会间接执行 git push。
.DESCRIPTION
    默认走 ctest：tests\CMakeLists.txt 只注册 pure / windows / git-readonly / git-commit
    四组，外加一条「push 闸门必须拒绝」的自证用例；会执行真实 git push 的那一组
    根本没有注册入口，所以这条命令不可能把提交推到远端。

    -Group 直接跑 gc_tests.exe 的某一组。点到 git-push / all 时，运行器要求
    同时设 GC_ALLOW_PUSH=1，否则拒绝启动并给出原因与数量（退出码 4）。
    本脚本自己不设这个变量：那是使用者本人的授权开关。

    Linux 侧的对应入口是根目录的 test.sh。
.EXAMPLE
    .\test.ps1                      # vs2026 + Debug，ctest 安全分组
    .\test.ps1 -Preset ninja        # Ninja Multi-Config 构建目录
    .\test.ps1 -Groups              # 打印各组账目与运行授权
    .\test.ps1 -Group git-commit    # 只跑隔离仓库里真实提交那一组
    .\test.ps1 -List -Group pure    # 列名单，不执行任何 Git 命令
#>
[CmdletBinding()]
param(
    [ValidateSet('vs2026', 'vs2022', 'ninja')]
    [string]$Preset = 'vs2026',
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [string]$Group,
    [switch]$Groups,
    [switch]$List,
    [string]$NameFilter,
    [string]$CMake
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
. (Join-Path $root 'scripts\windows\cmake-selection.ps1')

$requiredGenerator = Get-GcRequiredGenerator -Preset $Preset
$tool = Resolve-GcCmake -Generator $requiredGenerator -Explicit $CMake
$cmakeExe = $tool.CMake
$ctestExe = $tool.Ctest
$configPreset = "${Preset}-x64"
$buildPreset = "${Preset}-$($Config.ToLower())"

if (-not (Test-Path -LiteralPath $ctestExe)) {
    throw "找不到与 cmake 同目录的 ctest.exe：$ctestExe"
}

# 先确保测试程序是最新的：ctest 只跑已注册的用例，不负责重建。
& $cmakeExe --build --preset $buildPreset --target gc_tests
if ($LASTEXITCODE -ne 0) { throw "构建 gc_tests 失败（退出码 $LASTEXITCODE）。" }

if ($Groups -or $List -or $Group) {
    $exe = Join-Path $root "build\$configPreset\bin\$Config\gc_tests.exe"
    if (-not (Test-Path -LiteralPath $exe)) { throw "找不到测试程序：$exe" }
    $runnerArgs = @()
    if ($Groups) { $runnerArgs += '--groups' }
    if ($List) { $runnerArgs += '--list' }
    if ($Group) { $runnerArgs += @('--group', $Group) }
    if ($NameFilter) { $runnerArgs += $NameFilter }
    Write-Host "运行：$exe $($runnerArgs -join ' ')" -ForegroundColor Cyan
    & $exe @runnerArgs
    exit $LASTEXITCODE
}

if (-not (Test-Path -LiteralPath (Join-Path $root "build\$configPreset\CMakeCache.txt"))) {
    throw "构建目录尚未配置。先执行：$cmakeExe --preset $configPreset -S $root"
}

Write-Host "安全测试入口：ctest --preset $buildPreset" -ForegroundColor Cyan
& $ctestExe --preset $buildPreset
exit $LASTEXITCODE
