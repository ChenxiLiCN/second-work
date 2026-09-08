param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$IndexRoot = 'experiments\upi_v5_fli_seci'
)

$ErrorActionPreference = 'Stop'
$query = Join-Path $ProjectRoot 'build-cmake\bin\upi_query_index.exe'

$cases = @(
    [pscustomobject]@{ Dataset='dblp_small'; Path='A-P-A';     Epsilon='0.2'; Mu=2  },
    [pscustomobject]@{ Dataset='dblp_small'; Path='A-P-A';     Epsilon='0.2'; Mu=5  },
    [pscustomobject]@{ Dataset='dblp_small'; Path='A-P-A';     Epsilon='0.4'; Mu=3  },
    [pscustomobject]@{ Dataset='dblp_small'; Path='A-P-A';     Epsilon='0.5'; Mu=5  },
    [pscustomobject]@{ Dataset='dblp_small'; Path='A-P-A';     Epsilon='0.6'; Mu=5  },
    [pscustomobject]@{ Dataset='dblp_small'; Path='A-P-A';     Epsilon='0.7'; Mu=10 },
    [pscustomobject]@{ Dataset='dblp_small'; Path='A-P-A';     Epsilon='0.8'; Mu=5  },
    [pscustomobject]@{ Dataset='dblp_small'; Path='A-P-A';     Epsilon='0.9'; Mu=2  },
    [pscustomobject]@{ Dataset='movies';     Path='A-M-D-M-A'; Epsilon='0.3'; Mu=2 },
    [pscustomobject]@{ Dataset='movies';     Path='A-M-D-M-A'; Epsilon='0.5'; Mu=5 },
    [pscustomobject]@{ Dataset='movies';     Path='A-M-D-M-A'; Epsilon='0.7'; Mu=5 },
    [pscustomobject]@{ Dataset='movies';     Path='A-M-D-M-A'; Epsilon='0.9'; Mu=2 }
)

Push-Location -LiteralPath $ProjectRoot
try {
    $rows = foreach ($case in $cases) {
        $pathName = $case.Path.Replace('-', '')
        $indexDirectory = $IndexRoot + '\' + $case.Dataset
        $outputDirectory = $IndexRoot + '\validation\' +
                           $case.Dataset + '\' + $pathName
        $referenceDirectory = 'data\results\pscan_on_fli\' +
                              $case.Dataset + '\' + $pathName
        $lines = & $query $indexDirectory $case.Path $case.Epsilon $case.Mu `
                        $outputDirectory
        if ($LASTEXITCODE -ne 0) {
            throw "UPI query failed for $($case.Dataset) $($case.Path)"
        }
        $metrics = @{}
        foreach ($line in $lines) {
            $parts = $line -split '=', 2
            if ($parts.Count -eq 2) { $metrics[$parts[0]] = $parts[1] }
        }
        $suffix = $case.Epsilon + '-' + $case.Mu + '.txt'
        $result = Join-Path $outputDirectory ('result-' + $suffix)
        $roles = Join-Path $outputDirectory ('roles-' + $suffix)
        $referenceResult = Join-Path $referenceDirectory ('result-' + $suffix)
        $referenceRoles = Join-Path $referenceDirectory ('roles-' + $suffix)
        $resultMatch = (Get-FileHash $result).Hash -eq
                       (Get-FileHash $referenceResult).Hash
        $rolesMatch = (Get-FileHash $roles).Hash -eq
                      (Get-FileHash $referenceRoles).Hash
        [pscustomobject]@{
            dataset = $case.Dataset
            meta_path = $case.Path
            epsilon = $case.Epsilon
            mu = $case.Mu
            result_exact = $resultMatch
            roles_exact = $rolesMatch
            index_mode = [string]$metrics['index_mode']
            online_total_ms = [int64]$metrics['online_total_ms']
            query_ms = [int64]$metrics['query_ms']
            degree_pruned_edges = [int64]$metrics['degree_pruned_edges']
            exact_similarity_checks = [int64]$metrics['exact_similarity_checks']
        }
    }
    $rows | Format-Table dataset, epsilon, mu, result_exact, roles_exact, `
        index_mode, online_total_ms, query_ms -AutoSize
    if (($rows | Where-Object { -not $_.result_exact -or -not $_.roles_exact }).Count) {
        throw 'At least one UPI result differs from the pSCAN reference.'
    }
    Write-Host "All $($rows.Count) UPI cases match result and role files exactly."
} finally {
    Pop-Location
}
