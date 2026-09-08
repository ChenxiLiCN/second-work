$ErrorActionPreference='Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    $files=@('CMakeLists.txt','src/scan/QueryProfile.h','src/scan/PscanOnFli.cpp',
        'src/scan/ExactNeighborhoodCache.cpp','src/tools/bri_query_index.cpp',
        'build-cmake/bin/bri_query_witness_exclusion.exe','build-cmake/bin/bri_profile_v14.exe',
        'build-cmake/bin/bri_profile_control_v14.exe','experiments/profile_v14/bri_query_v14.exe',
        'experiments/benchmark_v13.ps1','experiments/validate_v13.ps1',
        'experiments/summarize_profile_v14.ps1','docs/V14_分阶段性能诊断.md','BUILDING.md',
        'experiments/profile_v14/summary-final-movies-imdb_legacy-yelp.csv',
        'experiments/profile_v14/timing-final-movies-imdb_legacy-yelp.csv',
        'experiments/profile_v14/parts-summary.csv','experiments/profile_v14/parts-total-summary.csv',
        'experiments/profile_v14/real-correctness-profile.csv',
        'experiments/profile_v14/real-correctness-profile_control.csv')
    $files | ForEach-Object {
        [pscustomobject]@{path=$_;bytes=(Get-Item -LiteralPath $_).Length;sha256=(Get-FileHash -LiteralPath $_).Hash}
    } | Export-Csv -NoTypeInformation -Encoding utf8 experiments/profile_v14/build-manifest.csv
}finally{Pop-Location}
