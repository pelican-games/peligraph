param(
    [Parameter(Mandatory=$true)][string]$Player,
    [Parameter(Mandatory=$true)][string]$Fixture,
    [Parameter(Mandatory=$true)][string]$ProjectPass,
    [Parameter(Mandatory=$true)][string]$OutDir
)

$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
$project = Join-Path $OutDir 'project'
New-Item -ItemType Directory -Force -Path (Join-Path $project 'assets'), (Join-Path $project 'scenes'), (Join-Path $project 'passes'), (Join-Path $project 'ui') | Out-Null
Copy-Item -LiteralPath $ProjectPass -Destination (Join-Path $project 'passes/main_rendering_config.json')

@'
{"schema":"pelican.project","version":1,"name":"wp155_behavior","engine_min_version":"0.1.0","basic_config":{"window_title":"WP155","window_size":{"width":160,"height":90},"fullscreen":false,"framerate":30,"camera":{"yfov":0.7853981633974483,"znear":0.1,"zfar":1000.0,"up":[0.0,1.0,0.0]},"default_scene_id":"default_scene","scene_data_json":"scenes/main.scene.json","asset_data_json":"assets/asset_data.json","rendering_config_json":"passes/main_rendering_config.json","default_rendering_pass":"main_render","ui_config_json":"ui/ui_overlay.json"}}
'@ | Set-Content -LiteralPath (Join-Path $project 'project.json') -Encoding ascii
@'
{"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[{"name":"BehaviorOnly","components":[{"name":"behavior","type":"wp155_project_behavior"},{"name":"behavior","type":"wp155_project_behavior","params":{"label":"second"}}]}]}}}
'@ | Set-Content -LiteralPath (Join-Path $project 'scenes/main.scene.json') -Encoding ascii
'{"schema":"pelican.asset_data","version":1,"models":[]}' | Set-Content -LiteralPath (Join-Path $project 'assets/asset_data.json') -Encoding ascii
'{"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}}' | Set-Content -LiteralPath (Join-Path $project 'ui/ui_overlay.json') -Encoding ascii

$liveDll = Join-Path $OutDir 'wp155_behavior.dll'
Copy-Item -LiteralPath $Fixture -Destination $liveDll
$playerOutput = @()
Push-Location $OutDir
try {
    $playerOutput = & $Player --headless --project $project --game-logic $liveDll --frames 3 --size 160x90 --fps 30 2>&1
    $playerExitCode = $LASTEXITCODE
} finally {
    Pop-Location
}
$playerOutput | Write-Output
if ($playerExitCode -ne 0) { throw "behavior project player failed with $playerExitCode" }

$log = $playerOutput | Out-String
$logPath = Join-Path $OutDir 'pelican.log'
if (Test-Path -LiteralPath $logPath) {
    $log += Get-Content -LiteralPath $logPath -Raw
}
foreach ($expected in @(
    'WP155 behavior init label=default',
    'WP155 behavior init label=second',
    'WP155 behavior event label=default',
    'WP155 behavior event label=second',
    'WP155 behavior update label=default',
    'WP155 behavior update label=second',
    'WP155 behavior destroy label=second',
    'WP155 behavior destroy label=default'
)) {
    if (-not $log.Contains($expected)) { throw "missing behavior lifecycle evidence: $expected" }
}

$destroySecond = $log.LastIndexOf('WP155 behavior destroy label=second')
$destroyDefault = $log.LastIndexOf('WP155 behavior destroy label=default')
if ($destroySecond -ge $destroyDefault) { throw 'behavior onDestroy was not reverse attachment order' }
