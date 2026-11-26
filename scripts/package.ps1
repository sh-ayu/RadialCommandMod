param(
    [string]$Configuration = "release"
)

$ErrorActionPreference = "Stop"

$repoRoot     = Split-Path -Parent $PSScriptRoot
$binDir       = Join-Path $repoRoot "bin\RadialCommandMod"
$manifestPath = Join-Path $repoRoot "manifest.json"
$lipPath      = Join-Path $repoRoot "lip.toml"
$toothPath    = Join-Path $repoRoot "tooth.json"
$xmakePath    = Join-Path $repoRoot "xmake.lua"

if (!(Test-Path $lipPath)) {
    throw "Missing required lip manifest: $lipPath"
}
if (!(Test-Path $toothPath)) {
    throw "Missing required lip manifest: $toothPath"
}

$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$lipRaw   = Get-Content $lipPath -Raw
$tooth    = Get-Content $toothPath -Raw | ConvertFrom-Json
$xmakeRaw = Get-Content $xmakePath -Raw
$version  = $manifest.version

if ($lipRaw -match '(?m)^version\s*=\s*"([^"]+)"') {
    if ($Matches[1] -ne $version) {
        throw "Version mismatch: manifest.json ($version) != lip.toml ($($Matches[1]))"
    }
} else {
    throw "Missing version in lip.toml: $lipPath"
}
if ($tooth.version -ne $version) {
    throw "Version mismatch: manifest.json ($version) != tooth.json ($($tooth.version))"
}
if ($xmakeRaw -match 'modVersion\s*=\s*"([^"]+)"') {
    if ($Matches[1] -ne $version) {
        throw "Version mismatch: manifest.json ($version) != xmake.lua ($($Matches[1]))"
    }
}

$requiredFiles = @(
    (Join-Path $binDir "RadialCommandMod.dll"),
    (Join-Path $repoRoot "manifest.json"),
    (Join-Path $repoRoot "lip.toml"),
    (Join-Path $repoRoot "tooth.json"),
    (Join-Path $repoRoot "config.sample.json"),
    (Join-Path $repoRoot "CHANGELOG.md"),
    (Join-Path $repoRoot "LICENSE"),
    (Join-Path $repoRoot "README.md"),
    (Join-Path $repoRoot "README_JA.md")
)

foreach ($f in $requiredFiles) {
    if (!(Test-Path $f)) {
        throw "Missing required release file: $f (run 'xmake build RadialCommandMod' first)"
    }
}

$distDir   = Join-Path $repoRoot "dist"
$stageRoot = Join-Path $distDir "stage"
$stageMod  = Join-Path $stageRoot "RadialCommandMod"
$zipPath   = Join-Path $distDir "RadialCommandMod-client-windows-x64.zip"

if (Test-Path $stageRoot) { Remove-Item -Recurse -Force $stageRoot }
if (Test-Path $zipPath)   { Remove-Item -Force $zipPath }

New-Item -ItemType Directory -Force -Path $stageMod | Out-Null

Copy-Item (Join-Path $binDir "RadialCommandMod.dll")  -Destination $stageMod -Force
Copy-Item (Join-Path $repoRoot "manifest.json")       -Destination $stageMod -Force
Copy-Item (Join-Path $repoRoot "config.sample.json")  -Destination $stageMod -Force
Copy-Item (Join-Path $repoRoot "CHANGELOG.md")        -Destination $stageMod -Force
Copy-Item (Join-Path $repoRoot "LICENSE")             -Destination $stageMod -Force
Copy-Item (Join-Path $repoRoot "README.md")           -Destination $stageMod -Force
Copy-Item (Join-Path $repoRoot "README_JA.md")        -Destination $stageMod -Force

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$zipStream = [System.IO.File]::Open($zipPath, [System.IO.FileMode]::Create)
try {
    $archive = New-Object System.IO.Compression.ZipArchive($zipStream, [System.IO.Compression.ZipArchiveMode]::Create, $false)
    try {
        $files = Get-ChildItem -Path $stageMod -File -Recurse
        foreach ($file in $files) {
            $relPath = $file.FullName.Substring($stageRoot.Length + 1).Replace('\', '/')
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $archive,
                $file.FullName,
                $relPath,
                [System.IO.Compression.CompressionLevel]::Optimal
            ) | Out-Null
        }
    } finally {
        $archive.Dispose()
    }
} finally {
    $zipStream.Dispose()
}

Remove-Item -Recurse -Force $stageRoot

Write-Output "Packaged release archive: $zipPath (version $version)"
