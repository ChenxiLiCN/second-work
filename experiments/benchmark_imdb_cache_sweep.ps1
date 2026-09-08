param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$Epsilon = '0.5',
    [int]$Mu = 5,
    [int[]]$CacheMiB = @(0, 8, 32)
)

$ErrorActionPreference = 'Stop'
$pscan = Join-Path $ProjectRoot 'build-cmake\bin\pscan_baseline.exe'
$fli = Join-Path $ProjectRoot 'build-cmake\bin\pscan_on_fli.exe'
Push-Location -LiteralPath $ProjectRoot

$graphDir = 'data\derived\transformed\imdb_legacy\AMDMA'
$indexFile = 'data\derived\indexes\imdb_legacy\AMDMA.fli'
$resultRoot = 'data\results\pscan_on_fli\imdb_legacy\AMDMA_cache_sweep'
New-Item -ItemType Directory -Force -Path $resultRoot | Out-Null

$baselineTimer = [Diagnostics.Stopwatch]::StartNew()
& $pscan $graphDir $Epsilon $Mu output | Out-Null
$baselineExit = $LASTEXITCODE
$baselineTimer.Stop()
if ($baselineExit -ne 0) {
    throw "pSCAN baseline failed with exit code $baselineExit"
}

$name = "result-$Epsilon-$Mu.txt"
$baselineFile = Join-Path $graphDir $name
$baselineHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $baselineFile).Hash

$rows = foreach ($cache in $CacheMiB) {
    $outputDir = Join-Path $resultRoot ("cache_$cache`MiB")
    $lines = & $fli $indexFile $Epsilon $Mu $outputDir $cache
    $exitCode = $LASTEXITCODE
    $metrics = @{}
    foreach ($line in $lines) {
        $parts = $line -split '=', 2
        if ($parts.Count -eq 2) {
            $metrics[$parts[0]] = $parts[1]
        }
    }

    $resultFile = Join-Path $outputDir $name
    $resultHash = if ($exitCode -eq 0) {
        (Get-FileHash -Algorithm SHA256 -LiteralPath $resultFile).Hash
    } else { '' }

    [pscustomobject]@{
        epsilon = $Epsilon
        mu = $Mu
        cache_mib = $cache
        exact_file_match = ($exitCode -eq 0 -and $resultHash -eq $baselineHash)
        pscan_wall_ms = $baselineTimer.ElapsedMilliseconds
        index_load_ms = [int64]$metrics['index_load_ms']
        query_ms = [int64]$metrics['query_ms']
        exact_similarity_checks = [int64]$metrics['exact_similarity_checks']
        similarity_posting_entries_read = [int64]$metrics['similarity_posting_entries_read']
        cached_similarity_checks = [int64]$metrics['cached_similarity_checks']
        cached_similarity_entries_read = [int64]$metrics['cached_similarity_entries_read']
        cache_hits = [int64]$metrics['neighborhood_cache_hits']
        cache_evictions = [int64]$metrics['neighborhood_cache_evictions']
        cache_peak_payload_bytes = [int64]$metrics['neighborhood_cache_peak_payload_bytes']
        certificate_entries = [int64]$metrics['sparse_certificate_entries']
        certificate_table_bytes = [int64]$metrics['sparse_certificate_table_bytes']
        result_sha256 = $resultHash
    }
}

$matrix = Join-Path $resultRoot "cache_sweep-$Epsilon-$Mu.csv"
$rows | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath $matrix
$rows | Format-Table cache_mib, exact_file_match, pscan_wall_ms, index_load_ms, query_ms, cache_peak_payload_bytes -AutoSize
Write-Host "Matrix: $(Join-Path $ProjectRoot $matrix)"
Pop-Location
