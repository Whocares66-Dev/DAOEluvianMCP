<#
.SYNOPSIS
    Copy a build's eluvian.dll into the game, or take it out.

.DESCRIPTION
    After `.\tools\build.ps1 -Preset <preset>`, puts build\<preset>\daolib\plugins\eluvian.dll
    (and its .pdb, so a crash log names our frames) under bin_ship\daolib\plugins. The game
    must not be running: it holds the file open.

    DAOLib's loader (version.dll) is copied only if the game has none, or with -Loader: the
    turn-based mod deploys one too, and any build of it loads every plugin, so Eluvian does
    not replace another's.

    -Remove deletes eluvian.dll and its .pdb; the loader and other plugins stay.

.EXAMPLE
    .\tools\deploy.ps1                 # debug build into the GOG install
    .\tools\deploy.ps1 -Preset release
    .\tools\deploy.ps1 -Loader         # replace the loader with this build's too
    .\tools\deploy.ps1 -Remove
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string] $Preset = 'debug',
    [switch] $Remove,
    [switch] $Loader,
    [string] $Game = 'C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins\bin_ship'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
$repo = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path (Join-Path $Game 'DAOrigins.exe'))) { throw "No DAOrigins.exe in $Game" }
if (Get-Process -Name DAOrigins -ErrorAction SilentlyContinue) { throw 'DAOrigins.exe is running; close it first' }

$plugins = Join-Path $Game 'daolib\plugins'
$plugin = Join-Path $plugins 'eluvian.dll'
$pluginPdb = Join-Path $plugins 'eluvian.pdb'

if ($Remove) {
    foreach ($f in @($plugin, $pluginPdb)) {
        if (Test-Path $f) { Remove-Item -Force $f; Show-Ok "removed $f" }
    }
    Show-Info 'the loader (version.dll) stays; DAOLib''s deploy.ps1 -Remove takes it out'
    return
}

$build = Join-Path $repo "build\$Preset"
$builtPlugin = Join-Path $build 'daolib\plugins\eluvian.dll'
$builtLoader = Join-Path $build 'version.dll'
foreach ($f in @($builtPlugin, $builtLoader)) {
    if (-not (Test-Path $f)) { throw "No $f; run .\tools\build.ps1 -Preset $Preset first" }
}

New-Item -ItemType Directory -Force $plugins | Out-Null
$gameLoader = Join-Path $Game 'version.dll'
if ($Loader -or -not (Test-Path $gameLoader)) {
    Copy-Item -Force $builtLoader $gameLoader
    $loaderPdb = Join-Path $build 'version.pdb'
    if (Test-Path $loaderPdb) { Copy-Item -Force $loaderPdb (Join-Path $Game 'version.pdb') }
    Show-Ok "loader  -> $gameLoader"
}
else {
    Show-Info "loader  already at $gameLoader (kept; -Loader replaces it)"
}
Copy-Item -Force $builtPlugin $plugin
$builtPdb = [IO.Path]::ChangeExtension($builtPlugin, '.pdb')
if (Test-Path $builtPdb) { Copy-Item -Force $builtPdb $pluginPdb }
Show-Ok "plugin  -> $plugin"
$docs = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'BioWare\Dragon Age\daolib'
Show-Info "log:      $(Join-Path $docs 'eluvian.log')"
Show-Info "port:     $(Join-Path $docs 'eluvian.json') once the game runs (8930 unless taken)"
Show-Info 'connect:  claude mcp add --transport http eluvian http://127.0.0.1:8930/mcp'
