<#
.SYNOPSIS
    Cut a release: tag the version, and put the zip Eluvian is installed from on a GitHub release.

.DESCRIPTION
    DAOLib's and DAOAddressLibrary's tools\release.ps1, for this repository
    (dev/RELEASING.md):

        .\tools\release.ps1 -DryRun     say what it would do, change nothing
        .\tools\release.ps1             release the version CMakeLists.txt names

    A tag is git's: a name for one commit, pushed like a branch. A release is GitHub's,
    on top of a tag: a title, notes and uploaded files. The tag is what the documentation
    site builds that version's pages from (.github\workflows\docs.yml, on the tag's
    push); the release carries the zip extracted into the game (tools\package.ps1).

    The version is project(DAOEluvianMCP VERSION ...) in CMakeLists.txt, and the notes
    are that version's section of CHANGELOG.md, so the release page says what the
    repository says. This script does not bump it, as its siblings can: the version is
    also in docs\_data\eluvian.json, which a test holds to what the build prints, and in
    the bridge, which another holds to CMakeLists.txt. It is set on the work branch, with
    the rest of the release's preparation.

    What can fail without leaving a mark goes first: the release build, its tests and
    the zip come before the tag, and nothing is pushed until the zip exists. The zip is
    of that build, the one the tests ran in. The formatters, the linters, ASan and
    /analyze are not run here: they belong to the commits that got to main.

.PARAMETER Jobs
    Build workers, as tools\build.ps1 takes it: few while someone plays the game.

.PARAMETER Draft
    Make the release a draft: on GitHub with its file, unseen until published by hand.

.PARAMETER DryRun
    Say what would happen. Builds nothing, writes nothing, pushes nothing.

.PARAMETER Force
    Skip the clean-tree and ancestry checks. Not the main check, nor DAOLib's: nothing is
    released off another branch, or built on a DAOLib that is no release's.
#>
[CmdletBinding()]
param(
    [int] $Jobs = 0,
    [switch] $Draft,
    [switch] $DryRun,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'console.ps1')
. (Join-Path $PSScriptRoot 'daolib_release.ps1')
$repo = Split-Path -Parent $PSScriptRoot

# --- where this would go ----------------------------------------------------

$remote = (git -C $repo remote get-url origin).Trim()
# The remote is an SSH host alias (git@github.com-someone:Owner/Repo.git),
# which gh does not read as a GitHub remote, so every gh call names the
# repository.
if ($remote -notmatch '[:/]([^:/]+/[^/]+?)(\.git)?$') { throw "Cannot read an owner/repo out of the origin remote: $remote" }
$github = $Matches[1]

# --- checks -----------------------------------------------------------------

$branch = (git -C $repo rev-parse --abbrev-ref HEAD).Trim()
if ($branch -ne 'main') { throw "On branch '$branch'. A release is cut from main." }

if (-not $Force) {
    # Tracked changes only: build\ is ignored and no reason to refuse. A submodule
    # checked out somewhere other than the commit main holds it at is one.
    $dirty = git -C $repo status --porcelain --untracked-files=no
    if ($dirty) { throw "The working tree has changes:`n$($dirty -join "`n")" }

    git -C $repo fetch --quiet origin
    if ($LASTEXITCODE -ne 0) { throw 'Could not fetch origin; cannot verify main before release.' }
    $upstream = (git -C $repo rev-parse origin/main).Trim()
    git -C $repo merge-base --is-ancestor $upstream HEAD
    if ($LASTEXITCODE -ne 0) { throw 'origin/main is not an ancestor of main. Fetch and merge first (or pass -Force).' }
}

# The plugin is built on a release of DAOLib, named in the zip's VERSION.txt and in
# the notes: what a game has is a DAOLib release's loader and address files, and a
# plugin built on a later commit may ask for what they lack (dev/RELEASING.md).
$daolibVersion = Get-DAOLibRelease (Join-Path $repo 'extern\DAOLib')
if (-not $daolibVersion) {
    throw 'extern\DAOLib is at no release of its own. Check one of its v<version> tags out, commit the submodule, and release that.'
}

# A dry run reports a missing gh as a warning and goes on, so one run says
# everything that is not ready.
$ghProblem = $null
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    $ghProblem = 'The GitHub CLI (gh) is not installed: https://cli.github.com'
}
else {
    gh auth status *> $null
    if ($LASTEXITCODE -ne 0) { $ghProblem = 'gh is not logged in. Run: gh auth login' }
}
if ($ghProblem) {
    if (-not $DryRun) { throw $ghProblem }
    Show-Warn $ghProblem
}

# --- the version ------------------------------------------------------------

$text = Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw
if ($text -notmatch 'project\((\w+) VERSION ((\d+)\.\d+\.\d+)') { throw 'No project(<name> VERSION x.y.z) in CMakeLists.txt' }
$name = $Matches[1]
$version = $Matches[2]
[int] $major = $Matches[3]
$tag = "v$version"

if (git -C $repo tag --list $tag) { throw "Tag $tag already exists here." }
git -C $repo ls-remote --exit-code --tags origin "refs/tags/$tag" *> $null
if ($LASTEXITCODE -eq 0) { throw "Tag $tag already exists on origin." }
if ($LASTEXITCODE -ne 2) { throw "Could not check whether tag $tag exists on origin." }

$notesMatch = [regex]::Match((Get-Content (Join-Path $repo 'CHANGELOG.md') -Raw),
    "(?ms)^## $([regex]::Escape($version))\s*\r?\n(?<notes>.*?)(?=^## |\z)")
if (-not $notesMatch.Success -or -not $notesMatch.Groups['notes'].Value.Trim()) {
    throw "CHANGELOG.md needs a nonempty ## $version section before release."
}
$notes = $notesMatch.Groups['notes'].Value.Trim()
$notesFile = Join-Path $repo "build\release-notes-$version.md"
$zip = Join-Path $repo "build\package\$name-$version.zip"

# Below 1.0 is GitHub's prerelease, as DAOLib's releases are.
$prerelease = $major -eq 0

Show-Line "release $github  $version  (tag $tag)" -Colour Cyan
Show-Info "built on DAOLib $daolibVersion"
if ($prerelease) { Show-Info 'marked prerelease (0.x)' }
if ($Draft) { Show-Info 'draft: published by hand' }

if ($DryRun) {
    Show-Line "`n-DryRun, so this is where it stops. It would:" -Colour Yellow
    @('build the release preset and run its tests', "run tools\package.ps1 -> $zip",
        "tag $tag, push main and the tag (the tag's push builds the site's $version pages)",
        "gh release create $tag on $github, CHANGELOG.md's $version section its notes, the zip attached") |
        ForEach-Object { Show-Info "- $_" }
    # Not return: ls-remote's 2, its "no such tag", would be left as the
    # script's exit code, and a dry run that found nothing wrong read as failed.
    exit 0
}

# --- prove it builds --------------------------------------------------------

try {
    & (Join-Path $PSScriptRoot 'build.ps1') -Preset release -Test -Jobs $Jobs
    & (Join-Path $PSScriptRoot 'package.ps1') -Preset release
    if (-not (Test-Path $zip)) { throw "Packaging did not write $zip." }
}
catch {
    Show-Bad 'failed before anything was tagged or pushed.'
    throw
}

# --- tag, push, release -----------------------------------------------------

New-Item -ItemType Directory -Force (Split-Path -Parent $notesFile) | Out-Null
[IO.File]::WriteAllText($notesFile, "$notes`n", [Text.UTF8Encoding]::new($false))
git -C $repo tag -a $tag -m "$name $version"
if ($LASTEXITCODE -ne 0) { throw "Could not create tag $tag." }
git -C $repo push --quiet origin HEAD
if ($LASTEXITCODE -ne 0) { throw "Could not push main; tag $tag exists here and nothing was published." }
git -C $repo push --quiet origin $tag
if ($LASTEXITCODE -ne 0) { throw "Main was pushed, but tag $tag was not; push the tag before creating the release." }

$arguments = @('release', 'create', $tag, '--repo', $github, '--title', "$name $version",
    '--notes-file', $notesFile, '--verify-tag')
if ($prerelease) { $arguments += '--prerelease' }
if ($Draft) { $arguments += '--draft' }
$arguments += $zip

gh @arguments
if ($LASTEXITCODE -ne 0) {
    throw "The tag and the push went through; gh release create did not. Run it alone, with --notes-file $notesFile and $zip (dev/RELEASING.md)."
}

Show-Ok "released $name $version"
Show-Info "https://github.com/$github/releases/tag/$tag"
