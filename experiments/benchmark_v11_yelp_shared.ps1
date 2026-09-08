param([string]$OutputRoot = 'experiments/v11/accepted')
$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    # Reuse the freshly measured same-session baseline from benchmark_v11.ps1.
    # Do not repeat the known-slow unshared variant or rebuild its 1.5 GB graph.
    $reference = 'experiments/v11/benchmark/yelp'
    $root = "$OutputRoot/benchmark/yelp"
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $rows = [System.Collections.Generic.List[object]]::new()
    foreach ($row in (Import-Csv experiments/v11/timing-yelp.csv | Where-Object { $_.mode -notin @('adaptive32','bri_cold') })) { $rows.Add($row) }
    foreach ($mode in @('adaptive32','bri_cold')) {
        $out = "$root/$mode"
        $timer = [Diagnostics.Stopwatch]::StartNew()
        if ($mode -eq 'adaptive32') {
            $lines = & ./build-cmake/bin/pscan_on_fli.exe data/derived/indexes/yelp/URBRU.fli 0.9 5 $out 32 2>&1
            $bytes = (Get-Item data/derived/indexes/yelp/URBRU.fli).Length
        } else {
            $lines = & ./build-cmake/bin/bri_query_index.exe "$reference/base.bri" U-R-B-R-U 0.9 5 $out 2>&1
            $bytes = (Get-Item "$reference/base.bri").Length
        }
        $code = $LASTEXITCODE
        $timer.Stop()
        $lines | Set-Content -LiteralPath "$root/$mode-1.log" -Encoding utf8
        if ($code -ne 0) { throw "$mode exited $code" }
        $metrics = @{}
        foreach ($line in $lines) {
            $parts = "$line" -split '=', 2
            if ($parts.Count -eq 2) { $metrics[$parts[0]] = $parts[1] }
        }
        foreach ($kind in @('result','roles')) {
            $actual = (Get-FileHash "$out/$kind-0.9-5.txt").Hash
            $expected = (Get-FileHash "$reference/before32/$kind-0.9-5.txt").Hash
            if ($actual -ne $expected) { throw "$mode $kind mismatch" }
            $metrics["${kind}_sha256"] = $actual
        }
        $rows.Add([pscustomobject]@{dataset='yelp';repeat=1;mode=$mode;wall_ms=$timer.Elapsed.TotalMilliseconds;index_bytes=$bytes;metrics=($metrics|ConvertTo-Json -Compress)})
        $rows | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "$OutputRoot/timing-yelp.csv"
        Write-Host "$mode $($timer.Elapsed.TotalMilliseconds) ms, clusters/roles matched"
    }
} finally { Pop-Location }
