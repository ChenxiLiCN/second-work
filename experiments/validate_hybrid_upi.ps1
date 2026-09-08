param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$query = Join-Path $ProjectRoot 'build-cmake\bin\upi_query_index.exe'
$cases = @(
    [pscustomobject]@{ Index='experiments\upi_hybrid\movies_limit2'; Dataset='movies'; Epsilon='0.5'; Mu=5; Reference='experiments\upi_multilayer\validation\movies_AMDMA'; ExpectedMode='base_relation_on_demand_fli' },
    [pscustomobject]@{ Index='experiments\upi_hybrid\movies_base_only_v2'; Dataset='movies'; Epsilon='0.3'; Mu=2; Reference='experiments\upi_multilayer\validation\movies_AMDMA'; ExpectedMode='selective_equivalence_certificates' },
    [pscustomobject]@{ Index='experiments\upi_hybrid\movies_base_only_v2'; Dataset='movies'; Epsilon='0.5'; Mu=5; Reference='experiments\upi_multilayer\validation\movies_AMDMA'; ExpectedMode='selective_equivalence_certificates' },
    [pscustomobject]@{ Index='experiments\upi_hybrid\imdb_base_only'; Dataset='imdb_legacy'; Epsilon='0.3'; Mu=2; Reference='experiments\upi_multilayer\validation\imdb_AMDMA'; ExpectedMode='selective_equivalence_certificates' },
    [pscustomobject]@{ Index='experiments\upi_hybrid\imdb_base_only'; Dataset='imdb_legacy'; Epsilon='0.5'; Mu=5; Reference='experiments\upi_multilayer\validation\imdb_AMDMA'; ExpectedMode='selective_equivalence_certificates' },
    [pscustomobject]@{ Index='experiments\upi_hybrid\imdb_base_only'; Dataset='imdb_legacy'; Epsilon='0.9'; Mu=2; Reference='experiments\upi_multilayer\validation\imdb_AMDMA'; ExpectedMode='selective_equivalence_certificates' }
)

Push-Location -LiteralPath $ProjectRoot
try {
    $rows = foreach ($case in $cases) {
        $output = 'experiments\upi_hybrid\validation\' + $case.Dataset +
                  '\eps-' + $case.Epsilon
        $lines = & $query $case.Index 'A-M-D-M-A' $case.Epsilon $case.Mu $output
        if ($LASTEXITCODE -ne 0) { throw 'hybrid UPI query failed' }
        $metrics = @{}
        foreach ($line in $lines) {
            $parts = $line -split '=', 2
            if ($parts.Count -eq 2) { $metrics[$parts[0]] = $parts[1] }
        }
        $suffix = $case.Epsilon + '-' + $case.Mu + '.txt'
        $result = Join-Path $output ('result-' + $suffix)
        $roles = Join-Path $output ('roles-' + $suffix)
        [pscustomobject]@{
            dataset = $case.Dataset
            epsilon = $case.Epsilon
            mode = [string]$metrics['index_mode']
            mode_expected = [string]$metrics['index_mode'] -eq $case.ExpectedMode
            result_exact = (Get-FileHash $result).Hash -eq
                           (Get-FileHash (Join-Path $case.Reference ('result-' + $suffix))).Hash
            roles_exact = (Get-FileHash $roles).Hash -eq
                          (Get-FileHash (Join-Path $case.Reference ('roles-' + $suffix))).Hash
            online_with_output_ms = [int64]$metrics['online_with_output_ms']
        }
    }
    $rows | Format-Table -AutoSize
    if (($rows | Where-Object {
            -not $_.mode_expected -or -not $_.result_exact -or
            -not $_.roles_exact }).Count) {
        throw 'Hybrid UPI validation failed.'
    }
    Write-Host "All $($rows.Count) hybrid UPI cases are exact and use the expected route."
} finally {
    Pop-Location
}
