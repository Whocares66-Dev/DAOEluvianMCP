<#
.SYNOPSIS
    Make the release zip Eluvian is installed from.

.DESCRIPTION
    After `.\tools\build.ps1 -Preset release`, writes build\package\DAOEluvianMCP-<version>.zip,
    laid out as the game's install folder, so it is extracted there (beside bin_ship) as
    DAOLib's own zip is. It holds the plugin's folder and nothing else:

        bin_ship\daolib\plugins\eluvian\eluvian.dll
        bin_ship\daolib\plugins\eluvian\eluvian.pdb         so a crash log names Eluvian's frames
        bin_ship\daolib\plugins\eluvian\eluvian_bridge.py   what an agent's client starts
        bin_ship\daolib\plugins\eluvian\README.md, LICENSE.txt, VERSION.txt

    No loader and no address files: those are DAOLib's zip, which a game has first. The
    texts are in the plugin's folder and not at the zip's root, which is the game's folder
    and where DAOLib's zip has a README.txt of its own.

    README.md is the repository's, the same file, as DAOAddressLibrary's zip has its own:
    what it says of installing and connecting is written once, there. Its links to other
    files of the repository lead nowhere from a game folder. VERSION.txt is what the
    README cannot say: the release, and the DAOLib it was built on, which is the tag
    extern\DAOLib is at, where a release holds it (dev/RELEASING.md).

    The version is the project's (CMakeLists.txt). Nothing is copied into the game.

.EXAMPLE
    .\tools\package.ps1                 # from build\release
    .\tools\package.ps1 -Preset debug   # a debug build's, for a trial
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release')]
    [string] $Preset = 'release'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
. (Join-Path $PSScriptRoot 'daolib_release.ps1')
$repo = Split-Path -Parent $PSScriptRoot

$built = Join-Path $repo "build\$Preset\daolib\plugins\eluvian"
$builtPlugin = Join-Path $built 'eluvian.dll'
if (-not (Test-Path $builtPlugin)) { throw "No $builtPlugin; run .\tools\build.ps1 -Preset $Preset first" }
$builtPdb = Join-Path $built 'eluvian.pdb'
if (-not (Test-Path $builtPdb)) { throw "No $builtPdb; a crash log names none of Eluvian's frames without it" }
# What is given out does not say who built it. The linker writes the folder of its
# temporary files into a PDB, which tools\build.ps1 keeps in the build tree for that
# reason (#115, as DAOLib #55); a plugin linked any other way is caught here.
$profileFolder = [Environment]::GetFolderPath('UserProfile')
foreach ($file in $builtPlugin, $builtPdb) {
    $bytes = [Text.Encoding]::Latin1.GetString([IO.File]::ReadAllBytes($file))
    foreach ($written in $profileFolder, ($profileFolder.ToCharArray() -join "`0")) {
        if ($bytes.IndexOf($written, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "$file holds the path of this user's folder; build it with .\tools\build.ps1 -Preset $Preset"
        }
    }
}
# The bridge beside the DLL is the build's copy of tools\eluvian_bridge.py; one edited
# since the build is not the one the build's tests ran.
$builtBridge = Join-Path $built 'eluvian_bridge.py'
$bridge = Join-Path $repo 'tools\eluvian_bridge.py'
if (-not (Test-Path $builtBridge) -or (Get-FileHash $builtBridge).Hash -ne (Get-FileHash $bridge).Hash) {
    throw "$builtBridge is not tools\eluvian_bridge.py as it is now; run .\tools\build.ps1 -Preset $Preset again"
}

$cmake = Get-Content -Raw (Join-Path $repo 'CMakeLists.txt')
if ($cmake -notmatch 'project\((\w+) VERSION (\d+\.\d+\.\d+)') { throw 'No version in CMakeLists.txt' }
$version = $Matches[2]
$name = "$($Matches[1])-$version"

# The loader and the address files a game has are a DAOLib release's, so VERSION.txt
# says which this was built on. At no tag it is a commit's, which a trial may be and a
# release is not.
$daolib = Join-Path $repo 'extern\DAOLib'
$daolibVersion = Get-DAOLibRelease $daolib
$pluginApi = Get-DAOLibPluginApi $daolib
if ($daolibVersion) {
    $builtOn = "DAOLib $daolibVersion"
}
else {
    $builtOn = "DAOLib at its commit $(git -C $daolib rev-parse --short HEAD), between releases"
    Show-Warn 'extern\DAOLib is at no release''s tag; an Eluvian release holds it at one'
}
$header = Get-Content -Raw (Join-Path $repo 'src\api\EluvianAPI.h')
if ($header -notmatch '#define ELUVIAN_INTERFACE_VERSION (\d+)') { throw 'No ELUVIAN_INTERFACE_VERSION in src\api\EluvianAPI.h' }
$interface = $Matches[1]

$out = Join-Path $repo 'build\package'
$stage = Join-Path $out $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$plugin = Join-Path $stage 'bin_ship\daolib\plugins\eluvian'
New-Item -ItemType Directory -Force $plugin | Out-Null

# By name, not the folder as built: a debug build leaves the linker's .ilk there.
Copy-Item -Force $builtPlugin, $builtPdb, $builtBridge $plugin
Copy-Item -Force (Join-Path $repo 'README.md') $plugin
Copy-Item -Force (Join-Path $repo 'LICENSE') (Join-Path $plugin 'LICENSE.txt')

$versionText = @"
DAOEluvianMCP $version
https://github.com/Whocares66-Dev/DAOEluvianMCP/releases

Built on $builtOn, plugin API $pluginApi
(https://github.com/Whocares66-Dev/DAOLib/releases). The game needs that release of
DAOLib or a later one of the same plugin API: its loader, and its address files. A loader
of another plugin API does not load this plugin, and the logs in
Documents\BioWare\Dragon Age\daolib (loader.log, eluvian.log) say so.

C interface $interface (EluvianAPI.h). A plugin that adds tools or events of its own to
Eluvian was built for one interface, and finds no Eluvian of another.
"@
Set-Content -Path (Join-Path $plugin 'VERSION.txt') -Value $versionText -Encoding utf8

$zip = Join-Path $out "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item -Recurse -Force $stage
Show-Info "built on $builtOn, plugin API $pluginApi, C interface $interface"
Show-Ok "package -> $zip"
