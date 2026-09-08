$ErrorActionPreference='Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    $inputRows=Import-Csv experiments/profile_v14/timing-final-movies-imdb_legacy-yelp.csv
    $details=foreach($row in $inputRows){
        if($row.mode -notin @('profile','profile_control')){continue}
        $m=$row.metrics | ConvertFrom-Json
        foreach($property in $m.PSObject.Properties){
            if($property.Name -match '^profile_(setup|prune|core|noncore)_(.+)_clock_adjusted_ms$'){
                $phase=$Matches[1];$part=$Matches[2];$prefix="profile_${phase}_${part}"
                [pscustomobject]@{dataset=$row.dataset;repeat=$row.repeat;mode=$row.mode;phase=$phase;part=$part;
                    adjusted_ms=[double]$property.Value;raw_ms=[double]$m."${prefix}_inclusive_ms";
                    sampling_se_ms=[double]$m."${prefix}_sampling_se_ms";
                    calls=[long]$m."${prefix}_calls";samples=[long]$m."${prefix}_samples"}
            }
        }
    }
    $details | Export-Csv -NoTypeInformation -Encoding utf8 experiments/profile_v14/detail.csv
    function Median($values){$sorted=@($values|Sort-Object);return $sorted[[int][math]::Floor($sorted.Count/2)]}
    $details | Group-Object dataset,mode,phase,part | ForEach-Object {
        $g=$_.Group
        [pscustomobject]@{dataset=$g[0].dataset;mode=$g[0].mode;phase=$g[0].phase;part=$g[0].part;
            median_adjusted_ms=(Median $g.adjusted_ms);min_adjusted_ms=($g.adjusted_ms|Measure-Object -Minimum).Minimum;
            max_adjusted_ms=($g.adjusted_ms|Measure-Object -Maximum).Maximum;
            median_sampling_se_ms=(Median $g.sampling_se_ms);calls=$g[0].calls}
    } | Export-Csv -NoTypeInformation -Encoding utf8 experiments/profile_v14/parts-summary.csv
    $details | Group-Object dataset,mode,repeat,part | ForEach-Object {
        $g=$_.Group
        [pscustomobject]@{dataset=$g[0].dataset;mode=$g[0].mode;repeat=$g[0].repeat;part=$g[0].part;
            adjusted_ms=($g.adjusted_ms|Measure-Object -Sum).Sum}
    } | Group-Object dataset,mode,part | ForEach-Object {
        $g=$_.Group
        [pscustomobject]@{dataset=$g[0].dataset;mode=$g[0].mode;part=$g[0].part;
            median_adjusted_ms=(Median $g.adjusted_ms)}
    } | Export-Csv -NoTypeInformation -Encoding utf8 experiments/profile_v14/parts-total-summary.csv
}finally{Pop-Location}
