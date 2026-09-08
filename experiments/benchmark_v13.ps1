param([string[]]$Datasets=@('movies','imdb_legacy'), [int]$Repeats=3,
      [string[]]$Modes=@('v12','bounds'), [switch]$IncludeHinBaseline,
      [string]$RunName='main', [string]$ExperimentRoot='experiments/v13')
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
        $root="$ExperimentRoot/$RunName/$dataset"
        New-Item -ItemType Directory -Force -Path $root | Out-Null
        $bri="experiments/v11/benchmark/$dataset/base.bri"
        $briHash=(Get-FileHash $bri).Hash
        for($rep=1;$rep -le $Repeats;$rep++) {
            # One fresh HINSCAN reproduction; query modes repeat independently.
            if($IncludeHinBaseline -and $rep -eq 1) {
                $raw=if($dataset -eq 'yelp'){'data/derived/normalized/yelp'}else{"data/raw/hin_text/$dataset"}
                $m=Run build-cmake/bin/hin_materialize.exe @($raw,$path,"$root/projected") "$root/materialize-$rep.log"
                $p=Run build-cmake/bin/pscan_baseline.exe @("$root/projected",$eps,'5','output') "$root/pscan-$rep.log"
                $rows.Add([pscustomobject]@{dataset=$dataset;repeat=$rep;mode='hin_reproduction';wall_ms=($m.wall+$p.wall);metrics=(@{materialize=$m.metrics;materialize_wall_ms=$m.wall;pscan_wall_ms=$p.wall}|ConvertTo-Json -Compress)})
            }
            $order=@($Modes)
            if($rep % 2 -eq 0){[array]::Reverse($order)}
            $hashes=@{}
            foreach($mode in $order) {
                $exe=switch($mode){'profile_control'{'build-cmake/bin/bri_profile_control_v14.exe'}'v14'{'experiments/profile_v14/bri_query_v14.exe'}'profile'{'build-cmake/bin/bri_profile_v14.exe'}'exclusion'{'build-cmake/bin/bri_query_witness_exclusion.exe'}'v13'{'experiments/v14/bri_query_v13.exe'}'bitmaps'{'build-cmake/bin/bri_query_witness_bitmaps.exe'}'v12'{'experiments/v13/bri_query_v12.exe'}'control'{'build-cmake/bin/bri_query_blocks.exe'}'bounds'{'build-cmake/bin/bri_query_witness_bounds.exe'}'adaptive'{'build-cmake/bin/bri_query_adaptive_bounds.exe'} default{throw "Unknown mode $mode"}}
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
                $rows | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "$ExperimentRoot/timing-$RunName-$($Datasets -join '-').csv"
                Write-Host "$dataset $rep $mode $([math]::Round($r.wall,2)) ms, results match"
            }
        }
        if((Get-FileHash $bri).Hash -ne $briHash){throw 'Persistent BRI changed'}
    }
    $rows | Group-Object dataset,mode | ForEach-Object {
        $s=@($_.Group.wall_ms|Sort-Object);$mid=[int][math]::Floor($s.Count/2)
        $median=if($s.Count%2){$s[$mid]}else{($s[$mid-1]+$s[$mid])/2}
        [pscustomobject]@{dataset=$_.Group[0].dataset;mode=$_.Group[0].mode;n=$s.Count;median_ms=$median;min_ms=$s[0];max_ms=$s[-1]}
    } | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "$ExperimentRoot/summary-$RunName-$($Datasets -join '-').csv"
}finally{Pop-Location}
