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
$runtimeDlls = @(
    'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll',
    'libfftw3f-3.dll', 'libiio.dll', 'libusb-1.0.dll', 'libxml2-16.dll',
    'libiconv-2.dll', 'zlib1.dll',
    'libfreetype-6.dll', 'libmd4c.dll', 'libharfbuzz-0.dll', 'libpng16-16.dll',
    'libb2-1.dll', 'libdouble-conversion.dll', 'libicuin78.dll', 'libicuuc78.dll',
    'libicudt78.dll', 'libpcre2-16-0.dll', 'libpcre2-8-0.dll', 'libzstd.dll',
    'libbrotlidec.dll', 'libbrotlicommon.dll', 'libbz2-1.dll', 'libglib-2.0-0.dll',
    'libgraphite2.dll', 'libintl-8.dll'
)
foreach ($dll in $runtimeDlls) {
    $path = Join-Path (Split-Path $deploy) $dll
    if (-not (Test-Path -LiteralPath $path)) { throw "Runtime DLL not found: $path" }
    Copy-Item -LiteralPath $path -Destination $stage -Force
}

$spectrumData = Join-Path $buildRoot 'src\data\spectrum'
if (-not (Test-Path -LiteralPath $spectrumData)) {
    throw "Spectrum calibration data not found: $spectrumData"
}
[IO.Directory]::CreateDirectory((Join-Path $stage 'data\spectrum')) | Out-Null
Get-ChildItem -LiteralPath $spectrumData -File | Copy-Item -Destination (Join-Path $stage 'data\spectrum') -Force

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
foreach ($name in @('qt6-base','gcc-libs','libiio','libusb','libxml2','libiconv','zlib')) {
    $licenseSource = Join-Path $msysLicenses $name
    if (Test-Path -LiteralPath $licenseSource) {
        Copy-Item -LiteralPath $licenseSource -Destination $licenseRoot -Recurse
    }
}
Copy-Item -LiteralPath (Join-Path $sourceRoot 'modules\spectrum\third_party\imgui\LICENSE.txt') `
    -Destination (Join-Path $licenseRoot 'imgui-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $sourceRoot 'modules\spectrum\third_party\implot\LICENSE') `
    -Destination (Join-Path $licenseRoot 'implot-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $sourceRoot 'modules\spectrum\third_party\miniaudio\miniaudio.h') `
    -Destination (Join-Path $licenseRoot 'miniaudio-license-in-source.h')
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
        @{ name='Dear ImGui and ImPlot'; SPDXID='SPDXRef-Package-ImGui'; versionInfo='vendored';
           downloadLocation='NOASSERTION'; filesAnalyzed=$false; licenseConcluded='MIT'; licenseDeclared='MIT' }
        @{ name='Spectrum runtime libraries'; SPDXID='SPDXRef-Package-SpectrumRuntime'; versionInfo='MSYS2 UCRT64';
           downloadLocation='NOASSERTION'; filesAnalyzed=$false; licenseConcluded='NOASSERTION'; licenseDeclared='NOASSERTION' }
    )
    relationships = @(
        @{ spdxElementId='SPDXRef-DOCUMENT'; relationshipType='DESCRIBES'; relatedSpdxElement='SPDXRef-Package-App' },
        @{ spdxElementId='SPDXRef-Package-App'; relationshipType='DEPENDS_ON'; relatedSpdxElement='SPDXRef-Package-Qt' },
        @{ spdxElementId='SPDXRef-Package-App'; relationshipType='DEPENDS_ON'; relatedSpdxElement='SPDXRef-Package-Json' },
        @{ spdxElementId='SPDXRef-Package-App'; relationshipType='DEPENDS_ON'; relatedSpdxElement='SPDXRef-Package-GccRuntime' }
        @{ spdxElementId='SPDXRef-Package-App'; relationshipType='DEPENDS_ON'; relatedSpdxElement='SPDXRef-Package-ImGui' }
        @{ spdxElementId='SPDXRef-Package-App'; relationshipType='DEPENDS_ON'; relatedSpdxElement='SPDXRef-Package-SpectrumRuntime' }
    )
}
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText((Join-Path $stage 'SBOM.spdx.json'), ($sbom | ConvertTo-Json -Depth 8), $utf8)

# Clean-PATH smoke: open both workspaces and exit through the normal Qt path.
$oldPlatform = $env:QT_QPA_PLATFORM
$oldDisableAutoS2Vna = $env:AFAR_DISABLE_AUTO_S2VNA
$oldPath = $env:PATH
$oldSwitchCount = $env:AFAR_TEST_SWITCH_COUNT
$env:QT_QPA_PLATFORM = 'windows'
$env:AFAR_DISABLE_AUTO_S2VNA = '1'
$env:AFAR_TEST_SWITCH_COUNT = '4'
$env:PATH = "$stage;$env:SystemRoot\System32;$env:SystemRoot"
try {
    $process = Start-Process -FilePath (Join-Path $stage 'AfarRxCalibrationStudio.exe') `
        -WorkingDirectory $stage -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "Packaged application smoke failed: $($process.ExitCode)" }
} finally {
    $env:QT_QPA_PLATFORM = $oldPlatform
    $env:AFAR_DISABLE_AUTO_S2VNA = $oldDisableAutoS2Vna
    $env:AFAR_TEST_SWITCH_COUNT = $oldSwitchCount
    $env:PATH = $oldPath
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
