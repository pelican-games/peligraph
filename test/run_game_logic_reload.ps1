param(
    [Parameter(Mandatory=$true)][string]$Player,
    [Parameter(Mandatory=$true)][string]$FixtureV1,
    [Parameter(Mandatory=$true)][string]$FixtureV2,
    [Parameter(Mandatory=$true)][string]$FixtureBadAbi,
    [Parameter(Mandatory=$true)][string]$ProjectPass,
    [Parameter(Mandatory=$true)][string]$FixtureModel,
    [Parameter(Mandatory=$true)][string]$OutDir,
    [int]$DemonstrationDelaySeconds = 0
)

$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutDir) { Remove-Item -LiteralPath $OutDir -Recurse -Force }
$project = Join-Path $OutDir 'project'
New-Item -ItemType Directory -Force -Path (Join-Path $project 'assets'), (Join-Path $project 'scenes'), (Join-Path $project 'passes'), (Join-Path $project 'ui') | Out-Null
Copy-Item -LiteralPath $ProjectPass -Destination (Join-Path $project 'passes/main_rendering_config.json')
Copy-Item -LiteralPath $FixtureModel -Destination (Join-Path $project 'assets/character.glb')

@'
{"schema":"pelican.project","version":1,"name":"wp90_reload","engine_min_version":"0.1.0","basic_config":{"window_title":"WP90","window_size":{"width":160,"height":90},"fullscreen":false,"framerate":30,"camera":{"yfov":0.7853981633974483,"znear":0.1,"zfar":1000.0,"up":[0.0,1.0,0.0]},"default_scene_id":"default_scene","scene_data_json":"scenes/main.scene.json","asset_data_json":"assets/asset_data.json","rendering_config_json":"passes/main_rendering_config.json","default_rendering_pass":"main_render","ui_config_json":"ui/ui_overlay.json"}}
'@ | Set-Content -LiteralPath (Join-Path $project 'project.json') -Encoding ascii
@'
{"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}}
'@ | Set-Content -LiteralPath (Join-Path $project 'scenes/main.scene.json') -Encoding ascii
'{"models":[{"name":"character","path":"assets/character.glb"}]}' | Set-Content -LiteralPath (Join-Path $project 'assets/asset_data.json') -Encoding ascii
'{"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}}' | Set-Content -LiteralPath (Join-Path $project 'ui/ui_overlay.json') -Encoding ascii

$sourceDll = Join-Path $OutDir 'wp90_live.dll'
Copy-Item -LiteralPath $FixtureV1 -Destination $sourceDll

$start = [System.Diagnostics.ProcessStartInfo]::new()
$start.FileName = $Player
$start.Arguments = "--headless --rpc --project `"$project`" --game-logic `"$sourceDll`""
$start.WorkingDirectory = $OutDir
$start.UseShellExecute = $false
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $false
$start.CreateNoWindow = $true
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $start
if (-not $process.Start()) { throw 'failed to start player' }

function Invoke-Rpc([int]$Id, [string]$Method) {
    $request = @{jsonrpc='2.0'; id=$Id; method=$Method; params=@{}} | ConvertTo-Json -Compress
    $process.StandardInput.WriteLine($request)
    $process.StandardInput.Flush()
    $line = $process.StandardOutput.ReadLine()
    if ([string]::IsNullOrWhiteSpace($line)) {
        Start-Sleep -Milliseconds 100
        throw "empty RPC response for $Method (exited=$($process.HasExited) code=$($process.ExitCode))"
    }
    return $line | ConvertFrom-Json
}

try {
    $step1 = Invoke-Rpc 1 'step_frame'
    if ($null -eq $step1.result) { throw 'v1 step failed' }

    if ($DemonstrationDelaySeconds -gt 0) {
        Start-Sleep -Seconds ([Math]::Floor($DemonstrationDelaySeconds / 2))
    }

    # This overwrite is the lock-avoidance assertion: the running process has a shadow copy loaded.
    Copy-Item -LiteralPath $FixtureV2 -Destination $sourceDll -Force
    $reload1 = Invoke-Rpc 2 'reload_game_logic'
    if ($reload1.result.generation -ne 2 -or $reload1.result.systems -ne 1) { throw "unexpected v2 reload result: $($reload1 | ConvertTo-Json -Compress)" }
    $step2 = Invoke-Rpc 3 'step_frame'
    if ($null -eq $step2.result) { throw 'v2 step failed' }
    if ($DemonstrationDelaySeconds -gt 0) {
        Start-Sleep -Seconds ([Math]::Ceiling($DemonstrationDelaySeconds / 2))
    }

    Set-Content -LiteralPath $sourceDll -Value 'not a PE DLL' -Encoding ascii
    $badLoad = Invoke-Rpc 4 'reload_game_logic'
    if ($null -eq $badLoad.error -or $badLoad.error.message -notmatch 'wp90_live.dll') { throw 'bad DLL did not return a named error' }
    $stepAfterBad = Invoke-Rpc 5 'step_frame'
    if ($null -eq $stepAfterBad.result) { throw 'old behavior did not continue after bad DLL' }

    Copy-Item -LiteralPath $FixtureBadAbi -Destination $sourceDll -Force
    $badAbi = Invoke-Rpc 6 'reload_game_logic'
    if ($null -eq $badAbi.error -or $badAbi.error.message -notmatch 'ABI version') { throw 'ABI mismatch was not rejected' }
    $stepAfterAbi = Invoke-Rpc 7 'step_frame'
    if ($null -eq $stepAfterAbi.result) { throw 'old behavior did not continue after ABI mismatch' }

    Copy-Item -LiteralPath $FixtureV1 -Destination $sourceDll -Force
    $reload2 = Invoke-Rpc 8 'reload_game_logic'
    if ($reload2.result.generation -ne 3 -or $reload2.result.systems -ne 1) { throw 'system registration leaked across reloads' }
    $status = Invoke-Rpc 9 'get_status'
    $runtime = $status.result.reload.runtime.'pelican.game_logic'
    if ($null -eq $runtime) { throw 'game logic runtime participant status is missing' }
    if ($runtime.attempted -ne 4 -or $runtime.applied -ne 2 -or $runtime.failed -ne 2) {
        throw "unexpected game logic participant counters: $($runtime | ConvertTo-Json -Compress)"
    }
    if ($null -ne $runtime.last_error) { throw 'successful reload did not clear participant error' }
} finally {
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(30000)) { $process.Kill($true); throw 'player did not stop' }
}

$log = Get-Content -LiteralPath (Join-Path $OutDir 'pelican.log') -Raw -ErrorAction SilentlyContinue
if ($process.ExitCode -ne 0) { throw "player exited with $($process.ExitCode)`n$log" }
if ($log -notmatch 'WP90 playercontrol behavior=v1 speed=2.5') { throw 'v1 behavior was not observed' }
if ($log -notmatch 'WP90 playercontrol behavior=v2 speed=5.0') { throw 'v2 behavior was not observed' }
if ($log -notmatch 'game logic DLL reloaded:.*generation=2.*systems=1') { throw 'successful reload evidence missing' }
