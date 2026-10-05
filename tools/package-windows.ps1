param(
    [string]$BuildDirectory = 'build',
    [string]$OutputDirectory = 'dist',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $sourceRoot $BuildDirectory))
$distRoot = [IO.Path]::GetFullPath((Join-Path $sourceRoot $OutputDirectory))
$version = '0.1.0'
$packageName = "AfarRxCalibrationStudio-$version-win64"
$stage = Join-Path $distRoot $packageName
$archive = Join-Path $distRoot "$packageName.zip"
$sourceArchive = Join-Path $distRoot "AfarRxCalibrationStudio-$version-source.zip"

if (-not $SkipBuild) {
    & cmake --build $buildRoot --config Release --target AfarRxCalibrationStudio -j 4
    if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
}
$exe = Join-Path $buildRoot 'src\AfarRxCalibrationStudio.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Executable not found: $exe" }
$deploy = (Get-Command windeployqt6.exe -ErrorAction Stop).Source

[IO.Directory]::CreateDirectory($distRoot) | Out-Null
foreach ($target in @($stage, $archive, "$archive.sha256", $sourceArchive, "$sourceArchive.sha256")) {
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
}
[IO.Directory]::CreateDirectory($stage) | Out-Null
Copy-Item -LiteralPath $exe -Destination $stage

& $deploy --release --no-translations --compiler-runtime --dir $stage $exe
if ($LASTEXITCODE -ne 0) { throw 'windeployqt6 failed' }
foreach ($dll in @('libgcc_s_seh-1.dll','libstdc++-6.dll','libwinpthread-1.dll')) {
    $path = Join-Path (Split-Path $deploy) $dll
    if (-not (Test-Path -LiteralPath $path)) { throw "Runtime DLL not found: $path" }
    Copy-Item -LiteralPath $path -Destination $stage -Force
}

Copy-Item -LiteralPath (Join-Path $sourceRoot 'examples') -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $sourceRoot 'schemas') -Destination $stage -Recurse
[IO.Directory]::CreateDirectory((Join-Path $stage 'docs')) | Out-Null
foreach ($doc in @('operator-guide.md','admin-guide.md','software-test-procedure.md','S2VNA-setup.md')) {
    Copy-Item -LiteralPath (Join-Path $sourceRoot "docs\$doc") -Destination (Join-Path $stage 'docs')
}
[IO.Directory]::CreateDirectory((Join-Path $stage 'tools')) | Out-Null
Copy-Item -LiteralPath (Join-Path $sourceRoot 'tools\verify-series.ps1') -Destination (Join-Path $stage 'tools')
Copy-Item -LiteralPath (Join-Path $sourceRoot 'packaging\THIRD_PARTY_NOTICES.md') -Destination $stage

$licenseRoot = Join-Path $stage 'licenses'
[IO.Directory]::CreateDirectory($licenseRoot) | Out-Null
$msysLicenses = 'C:\msys64\ucrt64\share\licenses'
foreach ($name in @('qt6-base','gcc-libs')) {
    $licenseSource = Join-Path $msysLicenses $name
    if (Test-Path -LiteralPath $licenseSource) {
        Copy-Item -LiteralPath $licenseSource -Destination $licenseRoot -Recurse
    }
}
$jsonLicense = Join-Path $buildRoot '_deps\nlohmann_json-src\LICENSE.MIT'
if (Test-Path -LiteralPath $jsonLicense) {
    Copy-Item -LiteralPath $jsonLicense -Destination (Join-Path $licenseRoot 'nlohmann-json-LICENSE.MIT')
}

$qtVersion = (& (Join-Path (Split-Path $deploy) 'qmake6.exe') -query QT_VERSION).Trim()
$created = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
$sbom = [ordered]@{
    spdxVersion = 'SPDX-2.3'
    dataLicense = 'CC0-1.0'
    SPDXID = 'SPDXRef-DOCUMENT'
    name = $packageName
    documentNamespace = "https://example.invalid/afar/sbom/$version/$([Guid]::NewGuid())"
    creationInfo = @{ created = $created; creators = @('Tool: package-windows.ps1') }
    packages = @(
        @{ name='AFAR RX Calibration Studio'; SPDXID='SPDXRef-Package-App'; versionInfo=$version;
           downloadLocation='NOASSERTION'; filesAnalyzed=$false; licenseConcluded='NOASSERTION'; licenseDeclared='NOASSERTION' },
        @{ name='Qt'; SPDXID='SPDXRef-Package-Qt'; versionInfo=$qtVersion;
           downloadLocation='https://www.qt.io/'; filesAnalyzed=$false; licenseConcluded='NOASSERTION'; licenseDeclared='LGPL-3.0-only OR GPL-3.0-only' },
        @{ name='nlohmann-json'; SPDXID='SPDXRef-Package-Json'; versionInfo='3.11.3';
           downloadLocation='https://github.com/nlohmann/json'; filesAnalyzed=$false; licenseConcluded='MIT'; licenseDeclared='MIT' },
        @{ name='GCC-MinGW-runtime'; SPDXID='SPDXRef-Package-GccRuntime'; versionInfo='MSYS2 UCRT64';
           downloadLocation='https://www.msys2.org/'; filesAnalyzed=$false; licenseConcluded='NOASSERTION'; licenseDeclared='GPL-3.0-or-later WITH GCC-exception-3.1' }
    )
    relationships = @(
        @{ spdxElementId='SPDXRef-DOCUMENT'; relationshipType='DESCRIBES'; relatedSpdxElement='SPDXRef-Package-App' },
        @{ spdxElementId='SPDXRef-Package-App'; relationshipType='DEPENDS_ON'; relatedSpdxElement='SPDXRef-Package-Qt' },
        @{ spdxElementId='SPDXRef-Package-App'; relationshipType='DEPENDS_ON'; relatedSpdxElement='SPDXRef-Package-Json' },
        @{ spdxElementId='SPDXRef-Package-App'; relationshipType='DEPENDS_ON'; relatedSpdxElement='SPDXRef-Package-GccRuntime' }
    )
}
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $stage 'SBOM.spdx.json'), ($sbom | ConvertTo-Json -Depth 8), $utf8)

# Smoke-start the deployed executable with its own plugins and DLLs.
$oldPlatform = $env:QT_QPA_PLATFORM
$oldDisableAutoS2Vna = $env:AFAR_DISABLE_AUTO_S2VNA
$env:QT_QPA_PLATFORM = 'offscreen'
$env:AFAR_DISABLE_AUTO_S2VNA = '1'
try {
    $process = Start-Process -FilePath (Join-Path $stage 'AfarRxCalibrationStudio.exe') `
        -WorkingDirectory $stage -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 2
    if ($process.HasExited) { throw "Packaged application exited early: $($process.ExitCode)" }
    Stop-Process -Id $process.Id -Force
    $process.WaitForExit()
} finally {
    $env:QT_QPA_PLATFORM = $oldPlatform
    $env:AFAR_DISABLE_AUTO_S2VNA = $oldDisableAutoS2Vna
}

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$archive.sha256", "$hash  $(Split-Path $archive -Leaf)`n", $utf8)

$sourceStage = Join-Path $distRoot '_source-stage'
if (Test-Path -LiteralPath $sourceStage) { Remove-Item -LiteralPath $sourceStage -Recurse -Force }
[IO.Directory]::CreateDirectory($sourceStage) | Out-Null
foreach ($item in Get-ChildItem -LiteralPath $sourceRoot -Force) {
    if ($item.Name -in @('build','dist','.git')) { continue }
    Copy-Item -LiteralPath $item.FullName -Destination $sourceStage -Recurse
}
Compress-Archive -Path (Join-Path $sourceStage '*') -DestinationPath $sourceArchive -CompressionLevel Optimal
Remove-Item -LiteralPath $sourceStage -Recurse -Force
$sourceHash = (Get-FileHash -LiteralPath $sourceArchive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$sourceArchive.sha256", "$sourceHash  $(Split-Path $sourceArchive -Leaf)`n", $utf8)

$installer = $null
$iscc = Get-Command iscc.exe -ErrorAction SilentlyContinue
if ($null -ne $iscc) {
    & $iscc.Source (Join-Path $sourceRoot 'packaging\windows-installer.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed' }
    $installer = Join-Path $distRoot "AfarRxCalibrationStudio-$version-setup.exe"
    $installerHash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText("$installer.sha256", "$installerHash  $(Split-Path $installer -Leaf)`n", $utf8)
}

[pscustomobject]@{
    RuntimeArchive = $archive
    RuntimeSha256 = $hash
    SourceArchive = $sourceArchive
    SourceSha256 = $sourceHash
    Installer = if ($null -ne $installer) { $installer } else { 'not built (iscc.exe not found)' }
    DeployedFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File).Count
} | Format-List
