param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$IndexRoot = 'experiments\upi_tseci'
)

$ErrorActionPreference = 'Stop'
$query = Join-Path $ProjectRoot 'build-cmake\bin\upi_query_index.exe'

$cases = @(
    [pscustomobject]@{ Dataset='movies'; Path='A-M-D-M-A'; Epsilon='0.2'; Mu=2; Reference='experiments\upi_multilayer\validation\movies_AMDMA_02' },
    [pscustomobject]@{ Dataset='movies'; Path='A-M-D-M-A'; Epsilon='0.3'; Mu=2; Reference='experiments\upi_multilayer\validation\movies_AMDMA' },
    [pscustomobject]@{ Dataset='movies'; Path='A-M-D-M-A'; Epsilon='0.5'; Mu=5; Reference='experiments\upi_multilayer\validation\movies_AMDMA' },
    [pscustomobject]@{ Dataset='movies'; Path='A-M-D-M-A'; Epsilon='0.7'; Mu=5; Reference='experiments\upi_multilayer\validation\movies_AMDMA' },
    [pscustomobject]@{ Dataset='movies'; Path='A-M-D-M-A'; Epsilon='0.8'; Mu=5; Reference='experiments\upi_multilayer\validation\movies_AMDMA_08' },
    [pscustomobject]@{ Dataset='movies'; Path='A-M-D-M-A'; Epsilon='0.9'; Mu=2; Reference='experiments\upi_multilayer\validation\movies_AMDMA' },
    [pscustomobject]@{ Dataset='imdb_legacy'; Path='A-M-D-M-A'; Epsilon='0.3'; Mu=2; Reference='experiments\upi_multilayer\validation\imdb_AMDMA' },
    [pscustomobject]@{ Dataset='imdb_legacy'; Path='A-M-D-M-A'; Epsilon='0.5'; Mu=5; Reference='experiments\upi_multilayer\validation\imdb_AMDMA' },
    [pscustomobject]@{ Dataset='imdb_legacy'; Path='A-M-D-M-A'; Epsilon='0.7'; Mu=5; Reference='experiments\upi_multilayer\validation\imdb_AMDMA' },
    [pscustomobject]@{ Dataset='imdb_legacy'; Path='A-M-D-M-A'; Epsilon='0.9'; Mu=2; Reference='experiments\upi_multilayer\validation\imdb_AMDMA' }
)

Push-Location -LiteralPath $ProjectRoot
try {
    $rows = foreach ($case in $cases) {
        $indexDirectory = Join-Path $IndexRoot $case.Dataset
        $outputDirectory = Join-Path $IndexRoot (
            'validation\' + $case.Dataset + '\eps-' + $case.Epsilon)
        $lines = & $query $indexDirectory $case.Path $case.Epsilon $case.Mu `
                        $outputDirectory
        if ($LASTEXITCODE -ne 0) {
            throw "T-SECI query failed for $($case.Dataset), epsilon=$($case.Epsilon)"
        }
        $metrics = @{}
        foreach ($line in $lines) {
            $parts = $line -split '=', 2
            if ($parts.Count -eq 2) { $metrics[$parts[0]] = $parts[1] }
        }
        $suffix = $case.Epsilon + '-' + $case.Mu + '.txt'
        $result = Join-Path $outputDirectory ('result-' + $suffix)
        $roles = Join-Path $outputDirectory ('roles-' + $suffix)
        $referenceResult = Join-Path $case.Reference ('result-' + $suffix)
        $referenceRoles = Join-Path $case.Reference ('roles-' + $suffix)
        [pscustomobject]@{
            dataset = $case.Dataset
            epsilon = $case.Epsilon
            result_exact = (Get-FileHash $result).Hash -eq
                           (Get-FileHash $referenceResult).Hash
            roles_exact = (Get-FileHash $roles).Hash -eq
                          (Get-FileHash $referenceRoles).Hash
            index_mode = [string]$metrics['index_mode']
            floor = [string]$metrics['selected_selective_floor']
            loaded_certificates = [int64]$metrics['selective_certificates_loaded']
            online_with_output_ms = [int64]$metrics['online_with_output_ms']
        }
    }
    $rows | Format-Table dataset, epsilon, result_exact, roles_exact, `
        index_mode, floor, loaded_certificates, online_with_output_ms -AutoSize
    if (($rows | Where-Object { -not $_.result_exact -or -not $_.roles_exact }).Count) {
        throw 'At least one T-SECI result differs from the exact reference.'
    }
    Write-Host "All $($rows.Count) T-SECI cases match result and role files exactly."
} finally {
    Pop-Location
}
