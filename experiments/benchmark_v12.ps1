param([string[]]$Datasets=@('movies','imdb_legacy'), [int]$Repeats=3, [switch]$IncludeHinBaseline)
$ErrorActionPreference='Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    $rows=[System.Collections.Generic.List[object]]::new()
    function Run([string]$Exe,[string[]]$Arguments,[string]$Log) {
        $timer=[Diagnostics.Stopwatch]::StartNew()
        $lines=& $Exe @Arguments 2>&1
        $code=$LASTEXITCODE
        $timer.Stop()
        $lines | Set-Content -LiteralPath $Log -Encoding utf8
        if ($code -ne 0) {throw "$Exe exited $code; see $Log"}
        $metrics=@{}
        foreach($line in $lines) { $kv="$line" -split '=',2; if($kv.Count -eq 2){$metrics[$kv[0]]=$kv[1]} }
        return @{wall=$timer.Elapsed.TotalMilliseconds;metrics=$metrics}
    }
    foreach($dataset in $Datasets) {
        $path=if($dataset -eq 'dblp_small'){'A-P-A'}elseif($dataset -eq 'yelp'){'U-R-B-R-U'}else{'A-M-D-M-A'}
        $eps=if($dataset -eq 'yelp'){'0.9'}else{'0.5'}
        $root="experiments/v12/benchmark/$dataset"
        New-Item -ItemType Directory -Force -Path $root | Out-Null
        $bri="experiments/v11/benchmark/$dataset/base.bri"
        $briHash=(Get-FileHash $bri).Hash
        for($rep=1;$rep -le $Repeats;$rep++) {
            if($IncludeHinBaseline) {
                $raw=if($dataset -eq 'yelp'){'data/derived/normalized/yelp'}else{"data/raw/hin_text/$dataset"}
                $m=Run build-cmake/bin/hin_materialize.exe @($raw,$path,"$root/projected") "$root/materialize-$rep.log"
                $p=Run build-cmake/bin/pscan_baseline.exe @("$root/projected",$eps,'5','output') "$root/pscan-$rep.log"
                $rows.Add([pscustomobject]@{dataset=$dataset;repeat=$rep;mode='hin_reproduction';wall_ms=($m.wall+$p.wall);metrics=(@{materialize=$m.metrics;materialize_wall_ms=$m.wall;pscan_wall_ms=$p.wall}|ConvertTo-Json -Compress)})
            }
            $modes=@('v11','control','seed','blocks')
            if($rep % 2 -eq 0){[array]::Reverse($modes)}
            $hashes=@{}
            foreach($mode in $modes) {
                $exe=switch($mode){'v11'{'experiments/v12/bri_query_v11.exe'}'control'{'build-cmake/bin/bri_query_index.exe'}'seed'{'build-cmake/bin/bri_query_block_seed.exe'}'blocks'{'build-cmake/bin/bri_query_blocks.exe'}}
                $out="$root/$mode"
                $r=Run $exe @($bri,$path,$eps,'5',$out) "$root/$mode-$rep.log"
                foreach($kind in @('result','roles')){
                    $hash=(Get-FileHash "$out/$kind-$eps-5.txt").Hash
                    if($hashes.ContainsKey($kind) -and $hashes[$kind] -ne $hash){throw "$dataset $mode $kind mismatch"}
                    $hashes[$kind]=$hash
                    $r.metrics["${kind}_sha256"]=$hash
                }
                if($IncludeHinBaseline -and (Get-FileHash "$root/projected/result-$eps-5.txt").Hash -ne $hashes.result){throw 'Upstream cluster mismatch'}
                $rows.Add([pscustomobject]@{dataset=$dataset;repeat=$rep;mode=$mode;wall_ms=$r.wall;metrics=($r.metrics|ConvertTo-Json -Compress)})
                $rows | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "experiments/v12/timing-$($Datasets -join '-').csv"
                Write-Host "$dataset $rep $mode $([math]::Round($r.wall,2)) ms, results match"
            }
        }
        if((Get-FileHash $bri).Hash -ne $briHash){throw 'Persistent BRI changed'}
    }
    $rows | Group-Object dataset,mode | ForEach-Object {
        $s=@($_.Group.wall_ms|Sort-Object);$mid=[int][math]::Floor($s.Count/2)
        $median=if($s.Count%2){$s[$mid]}else{($s[$mid-1]+$s[$mid])/2}
        [pscustomobject]@{dataset=$_.Group[0].dataset;mode=$_.Group[0].mode;n=$s.Count;median_ms=$median;min_ms=$s[0];max_ms=$s[-1]}
    } | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "experiments/v12/summary-$($Datasets -join '-').csv"
}finally{Pop-Location}
