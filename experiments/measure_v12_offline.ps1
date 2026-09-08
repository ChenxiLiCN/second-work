$ErrorActionPreference='Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    $rows=foreach($dataset in @('movies','imdb_legacy','yelp')){
        $root="experiments/v12/offline/$dataset"
        New-Item -ItemType Directory -Force -Path $root | Out-Null
        $raw=if($dataset -eq 'yelp'){'data/derived/normalized/yelp'}else{"data/raw/hin_text/$dataset"}
        for($rep=1;$rep -le 3;$rep++){
            $timer=[Diagnostics.Stopwatch]::StartNew()
            $lines=& ./build-cmake/bin/bri_build_index.exe $raw "$root/base.bri" 2>&1
            $code=$LASTEXITCODE
            $timer.Stop()
            $lines | Set-Content -LiteralPath "$root/build-$rep.log" -Encoding utf8
            if($code -ne 0){throw 'BRI build failed'}
            $hash=(Get-FileHash "$root/base.bri").Hash
            if($hash -ne (Get-FileHash "experiments/v11/benchmark/$dataset/base.bri").Hash){throw 'BRI content changed'}
            [pscustomobject]@{dataset=$dataset;repeat=$rep;wall_ms=$timer.Elapsed.TotalMilliseconds;bytes=(Get-Item "$root/base.bri").Length;sha256=$hash}
        }
    }
    $rows | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath experiments/v12/offline-times.csv
    $rows | Group-Object dataset | ForEach-Object {
        $sorted=@($_.Group.wall_ms | Sort-Object)
        [pscustomobject]@{dataset=$_.Name;median_ms=$sorted[1];min_ms=$sorted[0];max_ms=$sorted[2];bytes=$_.Group[0].bytes}
    } | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath experiments/v12/offline-summary.csv
}finally{Pop-Location}
