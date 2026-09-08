param([string]$Executable='build-cmake/bin/bri_query_adaptive_bounds.exe', [string]$RunName='adaptive', [string]$ExperimentRoot='experiments/v13')
$ErrorActionPreference='Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    $rows=foreach($group in (Import-Csv experiments/v11/real-correctness.csv | Group-Object dataset,meta_path,epsilon,mu)) {
        $case=$group.Group[0]
        $stem=$case.meta_path -replace '-',''
        $out="$ExperimentRoot/validation/$RunName/$($case.dataset)/$stem/$($case.epsilon)-$($case.mu)"
        New-Item -ItemType Directory -Force -Path $out | Out-Null
        $bri="experiments/v11/benchmark/$($case.dataset)/base.bri"
        $before=(Get-FileHash $bri).Hash
        $lines=& $Executable $bri $case.meta_path $case.epsilon $case.mu $out 2>&1
        $code=$LASTEXITCODE
        $lines | Set-Content -LiteralPath "$out/query.log" -Encoding utf8
        if($code -ne 0){throw "Query failed: $out"}
        if((Get-FileHash $bri).Hash -ne $before){throw 'BRI modified'}
        foreach($reference in $group.Group){
            $file="$($reference.output)-$($case.epsilon)-$($case.mu).txt"
            $hash=(Get-FileHash "$out/$file").Hash
            if($hash -ne $reference.sha256){throw "Mismatch: $out/$file"}
            [pscustomobject]@{dataset=$case.dataset;meta_path=$case.meta_path;epsilon=$case.epsilon;mu=$case.mu;output=$reference.output;match=$true;sha256=$hash}
        }
        Write-Host "$($case.dataset) $($case.meta_path) $($case.epsilon) $($case.mu): matched"
    }
    $rows | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "$ExperimentRoot/real-correctness-$RunName.csv"
}finally{Pop-Location}
