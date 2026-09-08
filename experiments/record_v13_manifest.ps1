$ErrorActionPreference='Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    $files=@(
        'CMakeLists.txt','src/scan/PscanOnFli.h','src/scan/PscanOnFli.cpp',
        'src/scan/ExactNeighborhoodCache.h','src/scan/ExactNeighborhoodCache.cpp',
        'src/tools/bri_query_index.cpp','src/tools/verify_adaptive_fli.cpp',
        'build-cmake/bin/bri_query_adaptive_bounds.exe','build-cmake/bin/bri_query_witness_bounds.exe',
        'build-cmake/bin/bri_query_blocks.exe','build-cmake/bin/verify_adaptive_fli.exe',
        'experiments/v13/bri_query_v12.exe','experiments/v13/bri_query_pressure_misses_pilot.exe',
        'experiments/benchmark_v13.ps1','experiments/validate_v13.ps1',
        'docs/V13_见证交集界与按需激活试验.md','BUILDING.md',
        'experiments/v13/correctness-final.log','experiments/v13/real-correctness-adaptive.csv',
        'experiments/v13/summary-final-movies-imdb_legacy-yelp.csv',
        'experiments/v13/timing-final-movies-imdb_legacy-yelp.csv',
        'experiments/v11/benchmark/movies/base.bri','experiments/v11/benchmark/imdb_legacy/base.bri',
        'experiments/v11/benchmark/yelp/base.bri'
    )
    $files | ForEach-Object {
        $item=Get-Item -LiteralPath $_
        [pscustomobject]@{path=$_;bytes=$item.Length;sha256=(Get-FileHash -LiteralPath $_).Hash}
    } | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath experiments/v13/build-manifest.csv
} finally {Pop-Location}
