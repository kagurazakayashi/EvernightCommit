<#
.SYNOPSIS
    清理 EvernightCommit 的构建产物（删除 build 目录）。
.DESCRIPTION
    删除整个 build 目录（CMake 缓存、编译产物、测试程序等），并保留 .evernight-local 不动。
    不会触碰源码、docs、resources、tests 目录。
.EXAMPLE
    .\clean.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$build = Join-Path $root 'build'
if (Test-Path -LiteralPath $build) {
    Remove-Item -LiteralPath $build -Recurse -Force
    Write-Host "已删除 $build" -ForegroundColor Green
} else {
    Write-Host '没有 build 目录，无需清理。' -ForegroundColor DarkGray
}
