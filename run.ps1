<#
.SYNOPSIS
    启动 EvernightCommit（GUI 程序，无需命令行参数）。
.DESCRIPTION
    启动 build\<preset>-x64\bin\<Config>\EvernightCommit.exe。
    产物不存在时提示先运行对应的 .\build.ps1。
.EXAMPLE
    .\run.ps1                       # vs2026 + Release
    .\run.ps1 -Preset vs2022 -Config Debug
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

$exe = Join-Path $root "build\${Preset}-x64\bin\$Config\EvernightCommit.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "找不到产物 $exe —— 请先运行 .\build.ps1 -Preset $Preset -Config $Config。"
}

Write-Host "启动 $exe" -ForegroundColor Green
Start-Process -FilePath $exe
