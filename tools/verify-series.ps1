param([Parameter(Position=0)][string]$SeriesDirectory, [switch]$SelfTest)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-Ok([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message } }

function Assert-Prefix([string]$Path, [byte[]]$Magic) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $data = [byte[]]::new($Magic.Length)
        Assert-Ok ($stream.Read($data, 0, $data.Length) -eq $data.Length) `
            "Truncated file: $(Split-Path $Path -Leaf)"
        for ($i = 0; $i -lt $Magic.Length; $i++) {
            Assert-Ok ($data[$i] -eq $Magic[$i]) "Invalid magic: $(Split-Path $Path -Leaf)"
        }
    } finally { $stream.Dispose() }
}

function Read-AfarPq([string]$Path, [string[]]$Columns, [bool]$ScanRows = $true) {
    Assert-Prefix $Path ([byte[]](65,70,65,82,80,81,1))
    $stream = [IO.File]::OpenRead($Path)
    $stream.Position = 7
    $reader = [IO.StreamReader]::new($stream, [Text.UTF8Encoding]::new($false), $true, 65536)
    $valid = 0
    $rows = 0
    try {
        $header = $reader.ReadLine()
        Assert-Ok ($null -ne $header) "Empty AFARPQ: $(Split-Path $Path -Leaf)"
        Assert-Ok ((($header -split "`t") -join '|') -eq ($Columns -join '|')) `
            "Unexpected header: $(Split-Path $Path -Leaf)"
        while ($null -ne ($line = $reader.ReadLine())) {
            if ($line.Length -eq 0) { continue }
            $lastTab = $line.LastIndexOf("`t")
            Assert-Ok ($lastTab -ge 0) "Malformed row: $(Split-Path $Path -Leaf)"
            $flag = $line.Substring($lastTab + 1)
            Assert-Ok ($flag -in @('0','1','true','false','True','False')) `
                "Invalid valid flag: $(Split-Path $Path -Leaf)"
            if ($flag -in @('1','true','True')) { $valid++ }
            $rows++
            if (-not $ScanRows) { break }
            if ($rows -le 10 -or $rows % 100000 -eq 0) {
                Assert-Ok (@($line -split "`t").Count -eq $Columns.Count) `
                    "Malformed sampled row: $(Split-Path $Path -Leaf)"
            }
        }
    } finally { $reader.Dispose() }
    Assert-Ok ($rows -gt 0) "No data rows: $(Split-Path $Path -Leaf)"
    [pscustomobject]@{ Rows = $rows; Valid = $valid }
}

function Test-Series([string]$Directory) {
    $seriesRoot = [IO.Path]::GetFullPath($Directory)
    Assert-Ok ([IO.Directory]::Exists($seriesRoot)) "Series directory does not exist: $seriesRoot"
    $required = @('run-config.json','attenuator-codes.csv','raw-s21.h5','raw-s21.csv',
        'direct-lut.parquet','inverse-lut.parquet','run-events.jsonl','report.pdf','manifest.sha256')
    foreach ($name in $required) {
        Assert-Ok ([IO.File]::Exists((Join-Path $seriesRoot $name))) "Missing required file: $name"
    }

    $listed = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($line in [IO.File]::ReadAllLines((Join-Path $seriesRoot 'manifest.sha256'))) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        Assert-Ok ($line -match '^([0-9a-fA-F]{64})  ([^/\\]+)$') "Invalid manifest line: $line"
        $hash, $name = $Matches[1].ToLowerInvariant(), $Matches[2]
        Assert-Ok ($name -notin @('.','..','manifest.sha256')) "Unsafe manifest name: $name"
        Assert-Ok ($listed.Add($name)) "Duplicate manifest entry: $name"
        $path = Join-Path $seriesRoot $name
        Assert-Ok ([IO.File]::Exists($path)) "Manifest file is missing: $name"
        Assert-Ok ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $hash) `
            "SHA-256 mismatch: $name"
        if ($name -ne 'run-events.jsonl') {
            Assert-Ok ((Get-Item -LiteralPath $path).Length -gt 0) "Empty artifact: $name"
        }
    }
    Assert-Ok ($listed.Count -gt 0) 'Manifest is empty'
    $actual = @(Get-ChildItem -LiteralPath $seriesRoot -File |
        Where-Object Name -ne 'manifest.sha256' | ForEach-Object Name)
    foreach ($name in $actual) { Assert-Ok ($listed.Contains($name)) "Not in manifest: $name" }
    Assert-Ok ($listed.Count -eq $actual.Count) 'Manifest/file count mismatch'

    Assert-Prefix (Join-Path $seriesRoot 'raw-s21.h5') ([byte[]](65,70,65,82,72,53,1,0))
    Assert-Prefix (Join-Path $seriesRoot 'report.pdf') ([Text.Encoding]::ASCII.GetBytes('%PDF-'))
    $config = Get-Content -LiteralPath (Join-Path $seriesRoot 'run-config.json') -Raw | ConvertFrom-Json
    Assert-Ok ($config.schema -eq 'afar.stage1.run-config/v1') 'Unexpected run-config schema'
    Assert-Ok (-not [string]::IsNullOrWhiteSpace([string]$config.run_id)) 'Empty run_id'
    $attenuatorHeader = [IO.File]::ReadAllLines((Join-Path $seriesRoot 'attenuator-codes.csv'))[0]
    Assert-Ok ($attenuatorHeader -eq 'att_code,att_cmd_db,enabled,settle_ms') `
        'Unexpected attenuator-codes.csv header'

    $csvPath = Join-Path $seriesRoot 'raw-s21.csv'
    $csvHeader = 'run_id,timestamp_utc,channel,att_code,phase_code,freq_hz,s21_re,s21_im,temp_c,attempt,overload,valid'
    $csvReader = [IO.File]::OpenText($csvPath)
    try {
        Assert-Ok ($csvReader.ReadLine() -eq $csvHeader) 'Unexpected raw-s21.csv header'
        $runPrefix = [string]$config.run_id + ','
        $line = $csvReader.ReadLine()
        Assert-Ok ($null -ne $line) 'raw-s21.csv has no data rows'
        Assert-Ok ($line.StartsWith($runPrefix, [StringComparison]::Ordinal)) `
            'raw-s21.csv run_id mismatch'
    } finally { $csvReader.Dispose() }

    $directColumns = @('channel','freq_hz','att_code','phase_code','s21_re','s21_im','mag_db',
        'phase_unwrapped_deg','atten_meas_db','phase_error_deg','drift_phase_deg',
        'repeatability_db','repeatability_deg','valid')
    $inverseColumns = @('channel','freq_hz','target_atten_db','target_phase_deg','selected_att_code',
        'selected_phase_code','measured_atten_db','measured_phase_deg','atten_residual_db',
        'phase_residual_deg','valid')
    $direct = Read-AfarPq (Join-Path $seriesRoot 'direct-lut.parquet') $directColumns $false
    $inverse = Read-AfarPq (Join-Path $seriesRoot 'inverse-lut.parquet') $inverseColumns $false
    foreach ($file in Get-ChildItem -LiteralPath $seriesRoot -File -Filter 'channel-*-calibration.parquet') {
        [void](Read-AfarPq $file.FullName $inverseColumns $false)
    }
    foreach ($file in Get-ChildItem -LiteralPath $seriesRoot -File -Filter '*.pdf') {
        Assert-Prefix $file.FullName ([Text.Encoding]::ASCII.GetBytes('%PDF-'))
    }

    $eventCount = 0
    foreach ($file in Get-ChildItem -LiteralPath $seriesRoot -File -Filter 'run-events*.jsonl') {
        foreach ($line in [IO.File]::ReadLines($file.FullName)) {
            if ([string]::IsNullOrWhiteSpace($line)) { continue }
            $event = $line | ConvertFrom-Json
            foreach ($field in @('timestamp_utc','level','component','event_code','run_id','text')) {
                Assert-Ok ($event.PSObject.Properties.Name -contains $field) `
                    "$($file.Name) event has no $field"
            }
            Assert-Ok ($event.run_id -eq $config.run_id) "$($file.Name) event has a different run_id"
            Assert-Ok ($event.PSObject.Properties.Name -notcontains 's21') `
                "$($file.Name) contains forbidden S21 payload"
            $eventCount++
        }
    }

    [pscustomobject]@{ Series=$config.run_id; Files=$listed.Count; StructuralSamples='PASS';
        Events=$eventCount; Result='PASS' }
}

function Test-Verifier {
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('afar-verifier-' + [Guid]::NewGuid())
    [IO.Directory]::CreateDirectory($tempRoot) | Out-Null
    try {
        $utf8 = [Text.UTF8Encoding]::new($false)
        [IO.File]::WriteAllText((Join-Path $tempRoot 'run-config.json'),
            '{"schema":"afar.stage1.run-config/v1","run_id":"SELFTEST"}', $utf8)
        [IO.File]::WriteAllText((Join-Path $tempRoot 'attenuator-codes.csv'),
            "att_code,att_cmd_db,enabled,settle_ms`n0,0,true,1`n", $utf8)
        [IO.File]::WriteAllBytes((Join-Path $tempRoot 'raw-s21.h5'), [byte[]](65,70,65,82,72,53,1,0))
        [IO.File]::WriteAllText((Join-Path $tempRoot 'raw-s21.csv'),
            "run_id,timestamp_utc,channel,att_code,phase_code,freq_hz,s21_re,s21_im,temp_c,attempt,overload,valid`nSELFTEST,2026-01-01T00:00:00Z,1,0,0,1,1,0,25,1,false,true`n", $utf8)
        $d = "channel`tfreq_hz`tatt_code`tphase_code`ts21_re`ts21_im`tmag_db`tphase_unwrapped_deg`tatten_meas_db`tphase_error_deg`tdrift_phase_deg`trepeatability_db`trepeatability_deg`tvalid`n1`t1`t0`t0`t1`t0`t0`t0`t0`t0`t0`t0`t0`t1`n"
        $i = "channel`tfreq_hz`ttarget_atten_db`ttarget_phase_deg`tselected_att_code`tselected_phase_code`tmeasured_atten_db`tmeasured_phase_deg`tatten_residual_db`tphase_residual_deg`tvalid`n1`t1`t0`t0`t0`t0`t0`t0`t0`t0`t1`n"
        [IO.File]::WriteAllBytes((Join-Path $tempRoot 'direct-lut.parquet'), [byte[]](65,70,65,82,80,81,1) + $utf8.GetBytes($d))
        [IO.File]::WriteAllBytes((Join-Path $tempRoot 'inverse-lut.parquet'), [byte[]](65,70,65,82,80,81,1) + $utf8.GetBytes($i))
        [IO.File]::WriteAllText((Join-Path $tempRoot 'run-events.jsonl'),
            '{"timestamp_utc":"2026-01-01T00:00:00Z","level":"info","component":"self-test","event_code":"TEST","run_id":"SELFTEST","text":"ok"}', $utf8)
        [IO.File]::WriteAllText((Join-Path $tempRoot 'report.pdf'), '%PDF-1.4 self-test', $utf8)
        $manifest = @(Get-ChildItem -LiteralPath $tempRoot -File | Sort-Object Name | ForEach-Object {
            '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
        })
        [IO.File]::WriteAllLines((Join-Path $tempRoot 'manifest.sha256'), $manifest, $utf8)
        Assert-Ok ((Test-Series $tempRoot).Result -eq 'PASS') 'Valid series was rejected'
        [IO.File]::AppendAllText((Join-Path $tempRoot 'raw-s21.csv'), 'corruption', $utf8)
        $rejected = $false
        try { [void](Test-Series $tempRoot) } catch { $rejected = $true }
        Assert-Ok $rejected 'Corrupted series was accepted'
        [pscustomobject]@{ Result='PASS'; Test='valid series accepted; corruption rejected' }
    } finally { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}

if ($SelfTest) { Test-Verifier | Format-List }
else {
    Assert-Ok (-not [string]::IsNullOrWhiteSpace($SeriesDirectory)) `
        'Usage: verify-series.ps1 <series-directory> or -SelfTest'
    Test-Series $SeriesDirectory | Format-List
}
