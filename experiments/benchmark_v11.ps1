param([int]$Repeats = 3, [string[]]$Datasets = @('dblp_small','movies','imdb_legacy'), [string]$OutputRoot = 'experiments/v11/accepted')
$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    $root = $OutputRoot
    $before = 'experiments/v11'
    $bin = 'build-cmake/bin'
    $rows = [System.Collections.Generic.List[object]]::new()
    function Run-Recorded([string]$Exe, [string[]]$Arguments, [string]$Log) {
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $lines = & $Exe @Arguments 2>&1
        $code = $LASTEXITCODE
        $timer.Stop()
        $lines | Set-Content -LiteralPath $Log -Encoding utf8
        if ($code -ne 0) { throw "Exit $code from $Exe. See $Log" }
        $metrics = @{}
        foreach ($line in $lines) {
            $parts = "$line" -split '=', 2
            if ($parts.Count -eq 2) { $metrics[$parts[0]] = $parts[1] }
        }
        return @{ Wall=$timer.Elapsed.TotalMilliseconds; Metrics=$metrics }
    }
    foreach ($dataset in $Datasets) {
        $path = if ($dataset -eq 'dblp_small') { 'A-P-A' } elseif ($dataset -eq 'yelp') { 'U-R-B-R-U' } else { 'A-M-D-M-A' }
        $stem = $path -replace '-', ''
        $epsilon = if ($dataset -eq 'yelp') { '0.9' } else { '0.5' }
        $suffix = "$epsilon-5.txt"
        $dir = "$root/benchmark/$dataset"
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $raw = "data/raw/hin_text/$dataset"
        if ($dataset -eq 'yelp') { $raw = 'data/derived/normalized/yelp' }
        $fli = "data/derived/indexes/$dataset/$stem.fli"
        for ($rep=1; $rep -le $Repeats; $rep++) {
            # Each process is sequential. All timers include its loading, output,
            # and termination. OS file caches are NOT flushed (warm OS cache).
            $bri = Run-Recorded "$bin/bri_build_index.exe" @($raw,"$dir/base.bri") "$dir/offline-$rep.log"
            $rows.Add([pscustomobject]@{dataset=$dataset; repeat=$rep; mode='offline_bri'; wall_ms=$bri.Wall; index_bytes=(Get-Item "$dir/base.bri").Length; metrics=($bri.Metrics|ConvertTo-Json -Compress)})
            foreach ($version in @('before','after')) {
                $exe = if ($version -eq 'before') { "$before/fli_build_before.exe" } else { "$bin/fli_build_index.exe" }
                $build = Run-Recorded $exe @($raw,$path,"$dir/$version.fli") "$dir/build-$version-$rep.log"
                $rows.Add([pscustomobject]@{dataset=$dataset; repeat=$rep; mode="build_fli_$version"; wall_ms=$build.Wall; index_bytes=(Get-Item "$dir/$version.fli").Length; metrics=($build.Metrics|ConvertTo-Json -Compress)})
            }
            $material = Run-Recorded "$bin/hin_materialize.exe" @($raw,$path,"$dir/projected") "$dir/materialize-$rep.log"
            $scan = Run-Recorded "$bin/pscan_baseline.exe" @("$dir/projected",$epsilon,'5','output') "$dir/pscan-$rep.log"
            $baselineHash = (Get-FileHash "$dir/projected/result-$suffix").Hash
            $rows.Add([pscustomobject]@{dataset=$dataset; repeat=$rep; mode='hinscan_reproduction'; wall_ms=($material.Wall+$scan.Wall); index_bytes=0; metrics=(@{materialize_wall_ms=$material.Wall; pscan_wall_ms=$scan.Wall; materialize=$material.Metrics}|ConvertTo-Json -Compress)})
            $rows | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "$root/timing-$($Datasets -join '-').csv"
            # Reverse indexed variant order on even repeats to reduce order bias.
            $modes = @('before32','adaptive32','bri_cold')
            if ($rep % 2 -eq 0) { [array]::Reverse($modes) }
            $roleHash = $null
            foreach ($mode in $modes) {
                $out = "$dir/$mode"
                if ($mode -eq 'bri_cold') {
                    $run = Run-Recorded "$bin/bri_query_index.exe" @("$dir/base.bri",$path,$epsilon,'5',$out) "$dir/$mode-$rep.log"
                    $bytes = (Get-Item "$dir/base.bri").Length
                } else {
                    $exe = if ($mode -eq 'before32') { "$before/pscan_on_fli_before.exe" } else { "$bin/pscan_on_fli.exe" }
                    $run = Run-Recorded $exe @($fli,$epsilon,'5',$out,'32') "$dir/$mode-$rep.log"
                    $bytes = (Get-Item $fli).Length
                }
                $hash = (Get-FileHash "$out/result-$suffix").Hash
                if ($hash -ne $baselineHash) { throw "Cluster mismatch: $dataset $mode $rep" }
                $roles = (Get-FileHash "$out/roles-$suffix").Hash
                if ($null -ne $roleHash -and $roles -ne $roleHash) { throw "Role mismatch: $dataset $mode $rep" }
                $roleHash = $roles
                $run.Metrics['cluster_sha256'] = $hash
                $run.Metrics['roles_sha256'] = $roles
                $rows.Add([pscustomobject]@{dataset=$dataset; repeat=$rep; mode=$mode; wall_ms=$run.Wall; index_bytes=$bytes; metrics=($run.Metrics|ConvertTo-Json -Compress)})
                $rows | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "$root/timing-$($Datasets -join '-').csv"
                Write-Host "$dataset run $rep $mode $([math]::Round($run.Wall,2)) ms, cluster/roles OK"
            }
            $rows | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "$root/timing-$($Datasets -join '-').csv"
        }
    }
    $rows | Group-Object dataset,mode | ForEach-Object {
        $sorted = @($_.Group.wall_ms | Sort-Object)
        $mid = [int][math]::Floor($sorted.Count/2)
        $median = if ($sorted.Count % 2) { $sorted[$mid] } else { ($sorted[$mid-1]+$sorted[$mid])/2 }
        [pscustomobject]@{dataset=$_.Group[0].dataset; mode=$_.Group[0].mode; n=$sorted.Count; median_ms=[math]::Round($median,3); min_ms=[math]::Round($sorted[0],3); max_ms=[math]::Round($sorted[-1],3); index_bytes=$_.Group[0].index_bytes}
    } | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath "$root/summary-$($Datasets -join '-').csv"
} finally { Pop-Location }
