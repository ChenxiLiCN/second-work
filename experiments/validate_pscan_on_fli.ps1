param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'

$pscan = Join-Path $ProjectRoot 'build-cmake\bin\pscan_baseline.exe'
$fli = Join-Path $ProjectRoot 'build-cmake\bin\pscan_on_fli.exe'
$resultRoot = 'data\results\pscan_on_fli'

if (-not (Test-Path -LiteralPath $pscan)) {
    throw "Missing pSCAN baseline executable: $pscan"
}
if (-not (Test-Path -LiteralPath $fli)) {
    throw "Missing pSCAN-on-FLI executable: $fli"
}

Push-Location -LiteralPath $ProjectRoot

$cases = @(
    [pscustomobject]@{ Dataset='dblp_small'; Index='APA';   Graph='dblp_small\APA'; Epsilon='0.2'; Mu=2  },
    [pscustomobject]@{ Dataset='dblp_small'; Index='APA';   Graph='dblp_small\APA'; Epsilon='0.2'; Mu=5  },
    [pscustomobject]@{ Dataset='dblp_small'; Index='APA';   Graph='dblp_small\APA'; Epsilon='0.4'; Mu=3  },
    [pscustomobject]@{ Dataset='dblp_small'; Index='APA';   Graph='dblp_small\APA'; Epsilon='0.5'; Mu=5  },
    [pscustomobject]@{ Dataset='dblp_small'; Index='APA';   Graph='dblp_small\APA'; Epsilon='0.6'; Mu=5  },
    [pscustomobject]@{ Dataset='dblp_small'; Index='APA';   Graph='dblp_small\APA'; Epsilon='0.7'; Mu=10 },
    [pscustomobject]@{ Dataset='dblp_small'; Index='APA';   Graph='dblp_small\APA'; Epsilon='0.8'; Mu=5  },
    [pscustomobject]@{ Dataset='dblp_small'; Index='APA';   Graph='dblp_small\APA'; Epsilon='0.9'; Mu=2  },
    [pscustomobject]@{ Dataset='movies';     Index='AMDMA'; Graph='movies\AMD_MA'; Epsilon='0.3'; Mu=2 },
    [pscustomobject]@{ Dataset='movies';     Index='AMDMA'; Graph='movies\AMD_MA'; Epsilon='0.5'; Mu=5 },
    [pscustomobject]@{ Dataset='movies';     Index='AMDMA'; Graph='movies\AMD_MA'; Epsilon='0.7'; Mu=5 },
    [pscustomobject]@{ Dataset='movies';     Index='AMDMA'; Graph='movies\AMD_MA'; Epsilon='0.9'; Mu=2 }
)

$rows = foreach ($case in $cases) {
    # Keep paths passed into MinGW binaries relative and ASCII-only. The current
    # libstdc++ filesystem conversion cannot consume this project's Chinese
    # absolute path on every Windows locale.
    $graphDir = 'data\derived\transformed\' + $case.Graph
    $indexFile = 'data\derived\indexes\' + $case.Dataset + '\' + $case.Index + '.fli'
    $outputDir = Join-Path $resultRoot ($case.Dataset + '\' + $case.Index)
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

    $baselineTimer = [System.Diagnostics.Stopwatch]::StartNew()
    & $pscan $graphDir $case.Epsilon $case.Mu output | Out-Null
    $baselineExit = $LASTEXITCODE
    $baselineTimer.Stop()

    $fliLines = & $fli $indexFile $case.Epsilon $case.Mu $outputDir
    $fliExit = $LASTEXITCODE
    $metrics = @{}
    foreach ($line in $fliLines) {
        $parts = $line -split '=', 2
        if ($parts.Count -eq 2) {
            $metrics[$parts[0]] = $parts[1]
        }
    }

    $name = 'result-' + $case.Epsilon + '-' + $case.Mu + '.txt'
    $baselineFile = Join-Path $graphDir $name
    $fliFile = Join-Path $outputDir $name
    $baselineHash = if ($baselineExit -eq 0) {
        (Get-FileHash -Algorithm SHA256 -LiteralPath $baselineFile).Hash
    } else { '' }
    $fliHash = if ($fliExit -eq 0) {
        (Get-FileHash -Algorithm SHA256 -LiteralPath $fliFile).Hash
    } else { '' }

    [pscustomobject]@{
        dataset = $case.Dataset
        epsilon = $case.Epsilon
        mu = $case.Mu
        pscan_exit = $baselineExit
        pscan_on_fli_exit = $fliExit
        exact_file_match = ($baselineExit -eq 0 -and $fliExit -eq 0 -and
                            $baselineHash -eq $fliHash)
        pscan_wall_ms = $baselineTimer.ElapsedMilliseconds
        fli_index_load_ms = [int64]$metrics['index_load_ms']
        fli_query_ms = [int64]$metrics['query_ms']
        exact_similarity_checks = [int64]$metrics['exact_similarity_checks']
        sparse_certificate_entries = [int64]$metrics['sparse_certificate_entries']
        sparse_certificate_table_bytes = [int64]$metrics['sparse_certificate_table_bytes']
        timestamp_workspace_bytes = [int64]$metrics['timestamp_workspace_bytes']
        peak_ephemeral_neighborhood_bytes = [int64]$metrics['peak_ephemeral_neighborhood_bytes']
        pscan_sha256 = $baselineHash
        pscan_on_fli_sha256 = $fliHash
    }
}

$matrix = Join-Path $resultRoot 'correctness_matrix.csv'
$rows | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath $matrix
$rows | Format-Table dataset, epsilon, mu, exact_file_match, pscan_wall_ms, fli_index_load_ms, fli_query_ms -AutoSize

if (($rows | Where-Object { -not $_.exact_file_match }).Count -ne 0) {
    throw "At least one pSCAN-on-FLI result differs from pSCAN. See $matrix"
}

Write-Host "All $($rows.Count) cases match exactly."
Write-Host "Matrix: $(Join-Path $ProjectRoot $matrix)"
Pop-Location
