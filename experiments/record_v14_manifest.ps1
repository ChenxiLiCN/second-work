$ErrorActionPreference='Stop'
Push-Location -LiteralPath (Split-Path -Parent $PSScriptRoot)
try {
    $files=@(
        'CMakeLists.txt','src/scan/PscanOnFli.h','src/scan/PscanOnFli.cpp',
        'src/scan/ExactNeighborhoodCache.h','src/scan/ExactNeighborhoodCache.cpp',
        'src/tools/bri_query_index.cpp','src/tools/verify_adaptive_fli.cpp',
        'build-cmake/bin/bri_query_witness_exclusion.exe','build-cmake/bin/bri_query_witness_bitmaps.exe',
        'build-cmake/bin/verify_adaptive_fli.exe','experiments/v14/bri_query_v13.exe',
        'experiments/v14/bri_query_bitmap_pilot.exe',
        'experiments/benchmark_v13.ps1','experiments/validate_v13.ps1',
        'docs/V14_见证缺失上界试验.md','BUILDING.md',
        'experiments/v14/correctness-final.log','experiments/v14/real-correctness-exclusion.csv',
        'experiments/v14/summary-final-movies-imdb_legacy-yelp.csv',
        'experiments/v14/timing-final-movies-imdb_legacy-yelp.csv',
        'experiments/v11/benchmark/movies/base.bri','experiments/v11/benchmark/imdb_legacy/base.bri',
        'experiments/v11/benchmark/yelp/base.bri'
    )
    $files | ForEach-Object {
        $item=Get-Item -LiteralPath $_
        [pscustomobject]@{path=$_;bytes=$item.Length;sha256=(Get-FileHash -LiteralPath $_).Hash}
    } | Export-Csv -NoTypeInformation -Encoding utf8 -LiteralPath experiments/v14/build-manifest.csv
} finally {Pop-Location}
