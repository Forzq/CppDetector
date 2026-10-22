param(
    [string]$Frames = 'C:\Users\ShablinskiyM\Desktop\frames36',
    [ValidateSet('all','dataset','sweep','seam','timing','geometry','pipeline','http')][string]$Phase = 'all',
    [int]$Repeats = 30,
    [string]$Output = 'artifacts/frames36-study',
    [string]$Config = 'config/config_frames36.json'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$framesPath = (Resolve-Path -LiteralPath $Frames).Path
Push-Location $repo
try {
    New-Item -ItemType Directory -Force -Path $Output | Out-Null
    $manifest = @('src/inspect/inspector.cpp','src/inspect/seam.cpp','src/inspect/geometry.cpp','src/inspect/auto_roi.cpp','src/app/app.cpp','src/app/decision_log.cpp',$Config,'tools/inspection-study/study.cpp') | ForEach-Object {
        [pscustomobject]@{ path = $_; sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }
    }
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Output ("source_manifest_" + $Phase + '.json')) -Encoding UTF8
    docker build -q -t cppdetector-study:local -f tools/inspection-study/Dockerfile tools/inspection-study
    if ($LASTEXITCODE -ne 0) { throw 'Docker build failed' }
    $common = 'src/inspect/inspector.cpp src/inspect/seam.cpp src/inspect/geometry.cpp src/inspect/auto_roi.cpp src/app/app.cpp src/app/decision_log.cpp'
    $source = 'study.cpp'; $extra = ''; $runArgs = @('--frames','/frames','--config',$Config,'--out',$Output,'--phase',$Phase,'--repeats',"$Repeats")
    if ($Phase -eq 'geometry') { $source = 'geometry_checks.cpp'; $runArgs = @($Output,'/frames') }
    if ($Phase -eq 'pipeline') { $source = 'pipeline_bench.cpp'; $runArgs = @('/frames',$Config,$Output) }
    if ($Phase -eq 'http') { $source = 'http_checks.cpp'; $extra = 'src/web/routes.cpp -lcpp-httplib'; $runArgs = @() }
    $command = 'mkdir -p build/study && g++ -std=c++17 -O3 -DNDEBUG -pthread -Wall -Wextra -I/usr/include/opencv4 tools/inspection-study/' + $source + ' ' + $common + ' ' + $extra + ' -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -o build/study/inspection-study && exec build/study/inspection-study "$@"'
    docker run --rm --mount "type=bind,source=$repo,target=/src" --mount "type=bind,source=$framesPath,target=/frames,readonly" cppdetector-study:local sh -c $command study @runArgs
    if ($LASTEXITCODE -ne 0) { throw 'Study failed' }
} finally { Pop-Location }
