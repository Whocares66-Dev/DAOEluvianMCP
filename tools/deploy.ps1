<#
.SYNOPSIS
    Copy a build's eluvian.dll and the address library's files into the game, or take
    eluvian.dll out.

.DESCRIPTION
    After `.\tools\build.ps1 -Preset <preset>`, puts the folder build\<preset>\daolib\plugins\
    eluvian\ (eluvian.dll and its .pdb, so a crash log names our frames) at
    bin_ship\daolib\plugins\eluvian\, where a loader of DAOLib #26 on looks for it, and takes
    out a loose eluvian.dll from before. The game must not be running: it holds the file open.

    DAOLib's loader (version.dll) is copied only if the game has none, or with -Loader: the
    turn-based mod deploys one too, and a loader loads every plugin of its own plugin API, so
    Eluvian does not replace another's. A loader of another API is refused by this plugin:
    eluvian.log says so in the game's first second, and the bridge's game_launch repeats it.
    -Loader puts this build's loader in, which the plugins built for the old one then refuse,
    so agree it with whoever else deploys plugins.

    The address library's files (build\<preset>\daolib\addresses\*.bin, one per build of the
    game; DAOLib 8b027c6 on) go under bin_ship\daolib\addresses, where the loader and every
    plugin read the running game's. Without them the loader loads nothing. Of this build's file
    and the game's, the one that knows more IDs stays (another plugin may be on a later DAOLib).

    -Remove deletes the eluvian folder (and a loose eluvian.dll and .pdb); the loader and
    other plugins stay.

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
    [string] $Game = 'C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins\bin_ship',
    [string] $Owner = 'eluvian-agent'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
$repo = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path (Join-Path $Game 'DAOrigins.exe'))) { throw "No DAOrigins.exe in $Game" }
# The game holds its plugins open. Another install of the game has the same
# process name (EA's beside GOG's, 2026-10-01), so a running one is asked
# where it runs from, as DAOLib's deploy.ps1 asks: only this install's
# blocks the copy. One whose path cannot be read is taken for this install's.
$exe = Join-Path $Game 'DAOrigins.exe'
foreach ($running in @(Get-Process -Name DAOrigins -ErrorAction SilentlyContinue)) {
    $from = $running.Path
    if (-not $from -or [string]::Equals($from, $exe, [StringComparison]::OrdinalIgnoreCase)) {
        $where = if ($from) { $from } else { 'a path that cannot be read' }
        throw "DAOrigins.exe is running from $where (process $($running.Id)); close it first"
    }
    Show-Info "another install's game is running, from $from (process $($running.Id)); it does not hold this install's files"
}

# Another agent may have the game and its folder in use between its runs, with a loader and
# plugins of its own in place: a deploy six seconds after a tester's game closed went into
# such a folder (2026-10-01). game.claim.json says who holds it, the one claim every agent's
# scripts honour (README.md, "The claim"); one that stands and is not this caller's stops the
# copy. -Owner says who is deploying.
$claimFolder = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'BioWare\Dragon Age\daolib'
# The old name too, which a bridge started before the shared name still writes (2026-10-02).
foreach ($claimFile in @((Join-Path $claimFolder 'game.claim.json'), (Join-Path $claimFolder 'eluvian.claim.json'))) {
    if (Test-Path $claimFile) {
        try {
            $claim = Get-Content $claimFile -Raw | ConvertFrom-Json
            $left = [double]$claim.until - [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
            if ($left -gt 0 -and $claim.owner -ne $Owner) {
                throw "the game and its folder are claimed by $($claim.owner) for another $([math]::Round($left / 60, 1)) minutes ($($claim.note)); not deployed"
            }
        }
        catch [System.ArgumentException] { Show-Info "$claimFile is not readable; taken as no claim" }
    }
}

# A plugin lives in a folder of its own, bin_ship\daolib\plugins\eluvian\, its files
# beside it (DAOLib #26): a loader of #26 loads no DLL loose in plugins\, and loader.log
# says where each belongs. A loose eluvian.dll and .pdb from before are taken out.
$plugins = Join-Path $Game 'daolib\plugins'
$pluginDir = Join-Path $plugins 'eluvian'
$loose = @((Join-Path $plugins 'eluvian.dll'), (Join-Path $plugins 'eluvian.pdb'))

if ($Remove) {
    if (Test-Path $pluginDir) { Remove-Item -Recurse -Force $pluginDir; Show-Ok "removed $pluginDir" }
    foreach ($f in $loose) {
        if (Test-Path $f) { Remove-Item -Force $f; Show-Ok "removed $f" }
    }
    Show-Info 'the loader (version.dll) stays; DAOLib''s deploy.ps1 -Remove takes it out'
    return
}

$build = Join-Path $repo "build\$Preset"
$builtDir = Join-Path $build 'daolib\plugins\eluvian'
$builtPlugin = Join-Path $builtDir 'eluvian.dll'
$builtLoader = Join-Path $build 'version.dll'
foreach ($f in @($builtPlugin, $builtLoader)) {
    if (-not (Test-Path $f)) { throw "No $f; run .\tools\build.ps1 -Preset $Preset first" }
}
$builtAddresses = Join-Path $build 'daolib\addresses'
if (-not (Test-Path (Join-Path $builtAddresses '*.bin'))) {
    throw "No address files in $builtAddresses; run .\tools\build.ps1 -Preset $Preset first"
}

New-Item -ItemType Directory -Force $plugins | Out-Null
$gameLoader = Join-Path $Game 'version.dll'
$keptLoader = -not $Loader -and (Test-Path $gameLoader)
if ($Loader -or -not (Test-Path $gameLoader)) {
    Copy-Item -Force $builtLoader $gameLoader
    $loaderPdb = Join-Path $build 'version.pdb'
    if (Test-Path $loaderPdb) { Copy-Item -Force $loaderPdb (Join-Path $Game 'version.pdb') }
    Show-Ok "loader  -> $gameLoader"
}
else {
    Show-Info "loader  already at $gameLoader (kept; -Loader replaces it)"
}
# The plugin's folder as built, but the linker's incremental-link file.
New-Item -ItemType Directory -Force $pluginDir | Out-Null
Get-ChildItem -Path $builtDir -File | Where-Object { $_.Extension -ne '.ilk' } |
    ForEach-Object { Copy-Item -Force $_.FullName (Join-Path $pluginDir $_.Name) }
Show-Ok "plugin  -> $pluginDir"
foreach ($f in $loose) {
    if (Test-Path $f) { Remove-Item -Force $f; Show-Ok "removed the loose $f (plugins have folders now)" }
}
# The address files: one per build of the game, read by the loader and every plugin. The
# library only adds IDs (known on 2026-10-02: 103,293 for GOG's build, 103,528 Steam's, 103,567 EA's),
# and the plugins in one folder may be on different DAOLibs: so of this build's file and the
# game's, the one that knows more IDs stays, or a deploy would take IDs from under another
# plugin. A file is a 36-byte header, then a 32-bit word per ID, 0xFFFFFFFF for one the
# library does not know (DAOAddressLibrary's tools/addrlib.py, pack_build).
# A game folder that had none had a loader from before them (plugin API 1), which this
# plugin refuses.
function Get-KnownIdCount([string] $path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $known = 0
    for ($at = 36; $at + 4 -le $bytes.Length; $at += 4) {
        if ([BitConverter]::ToUInt32($bytes, $at) -ne [uint32]::MaxValue) { $known++ }
    }
    return $known
}
$addresses = Join-Path $Game 'daolib\addresses'
$hadAddresses = Test-Path (Join-Path $addresses '*.bin')
New-Item -ItemType Directory -Force $addresses | Out-Null
$kept = 0
foreach ($file in Get-ChildItem -Path $builtAddresses -Filter '*.bin') {
    $there = Join-Path $addresses $file.Name
    if ((Test-Path $there) -and (Get-KnownIdCount $there) -ge (Get-KnownIdCount $file.FullName)) {
        $kept++
        continue
    }
    Copy-Item -Force $file.FullName $there
}
Show-Ok "addresses -> $addresses$(if ($kept) { " ($kept kept as the game had them: they know as many IDs or more)" })"
if ($keptLoader -and -not $hadAddresses) {
    Show-Info 'the game had no address files, so the loader kept there is likely of an older plugin API:'
    Show-Info 'Eluvian will refuse it (eluvian.log says so once the game starts). -Loader replaces it.'
}
$docs = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'BioWare\Dragon Age\daolib'
Show-Info "log:      $(Join-Path $docs 'eluvian.log')"
Show-Info "port:     $(Join-Path $docs 'eluvian.json') once the game runs (8930 unless taken)"
Show-Info 'connect:  claude mcp add --transport http eluvian http://127.0.0.1:8930/mcp'
