$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    $cases = @()
    foreach ($pair in @(@('0.2',2),@('0.2',5),@('0.4',3),@('0.5',5),@('0.6',5),@('0.7',10),@('0.8',5),@('0.9',2))) {
        $cases += [pscustomobject]@{Dataset='dblp_small'; Path='A-P-A'; Epsilon=$pair[0]; Mu=$pair[1]; Reference='data/results/pscan_on_fli/dblp_small/APA'}
    }
    foreach ($dataset in @('movies','imdb_legacy')) {
        foreach ($pair in @(@('0.3',2),@('0.5',5),@('0.7',5),@('0.9',2))) {
            $reference = if ($dataset -eq 'movies') { 'data/results/pscan_on_fli/movies/AMDMA' } else { 'experiments/upi_multilayer/validation/imdb_AMDMA' }
            $cases += [pscustomobject]@{Dataset=$dataset; Path='A-M-D-M-A'; Epsilon=$pair[0]; Mu=$pair[1]; Reference=$reference}
        }
    }
    foreach ($spec in @('A-M-A','A-M-W-M-A')) {
        $stem = $spec -replace '-', ''
        $cases += [pscustomobject]@{Dataset='movies'; Path=$spec; Epsilon='0.5'; Mu=5; Reference="experiments/upi_v2/queries/movies_$stem"}
    }
    $rows = foreach ($case in $cases) {
        $stem = $case.Path -replace '-', ''
        $out = "experiments/v11/validation/$($case.Dataset)/$stem/$($case.Epsilon)-$($case.Mu)"
        New-Item -ItemType Directory -Force -Path $out | Out-Null
        $bri = "experiments/v11/benchmark/$($case.Dataset)/base.bri"
        $briHash = (Get-FileHash -LiteralPath $bri).Hash
        $lines = & ./build-cmake/bin/bri_query_index.exe $bri $case.Path $case.Epsilon $case.Mu $out 2>&1
        $code = $LASTEXITCODE
        $lines | Set-Content -LiteralPath "$out/query.log" -Encoding utf8
        if ($code -ne 0) { throw "Query failed: $out" }
        if ((Get-FileHash -LiteralPath $bri).Hash -ne $briHash) { throw 'Query modified persistent BRI' }
        foreach ($kind in @('result','roles')) {
            $file = "$kind-$($case.Epsilon)-$($case.Mu).txt"
            $actual = (Get-FileHash -LiteralPath "$out/$file").Hash
            $expected = (Get-FileHash -LiteralPath "$($case.Reference)/$file").Hash
            if ($actual -ne $expected) { throw "Mismatch: $out/$file" }
            [pscustomobject]@{dataset=$case.Dataset; meta_path=$case.Path; epsilon=$case.Epsilon; mu=$case.Mu; output=$kind; match=$true; sha256=$actual; reference="$($case.Reference)/$file"}
        }
        Write-Host "$($case.Dataset) $($case.Path) epsilon=$($case.Epsilon) mu=$($case.Mu): cluster/roles matched"
    }
    $rows | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath experiments/v11/real-correctness.csv
} finally { Pop-Location }
