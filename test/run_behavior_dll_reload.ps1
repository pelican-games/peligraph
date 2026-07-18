param(
    [Parameter(Mandatory=$true)][ValidateSet(
        'code_only',
        'schema_drift',
        'version_bump_success',
        'version_bump_failure',
        'type_removed',
        'candidate_lifecycle_zero',
        'pending_recovery',
        'queued_event_purge',
        'oninit_rollback'
    )][string]$Case,
    [Parameter(Mandatory=$true)][string]$Player,
    [Parameter(Mandatory=$true)][string]$FixtureV1,
    [Parameter(Mandatory=$true)][string]$FixtureCodeV2,
    [Parameter(Mandatory=$true)][string]$FixtureDrift,
    [Parameter(Mandatory=$true)][string]$FixtureBumpOk,
    [Parameter(Mandatory=$true)][string]$FixtureBumpBad,
    [Parameter(Mandatory=$true)][string]$FixtureRemoved,
    [Parameter(Mandatory=$true)][string]$FixtureInitFault,
    [Parameter(Mandatory=$true)][string]$ProjectPass,
    [Parameter(Mandatory=$true)][string]$OutDir
)

$ErrorActionPreference = 'Stop'

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Match-Count([string]$Text, [string]$Pattern) {
    return ([regex]::Matches($Text, $Pattern)).Count
}

if (Test-Path -LiteralPath $OutDir) {
    Remove-Item -LiteralPath $OutDir -Recurse -Force
}
$project = Join-Path $OutDir 'project'
New-Item -ItemType Directory -Force -Path `
    (Join-Path $project 'assets'), `
    (Join-Path $project 'scenes'), `
    (Join-Path $project 'passes'), `
    (Join-Path $project 'ui') | Out-Null
Copy-Item -LiteralPath $ProjectPass -Destination `
    (Join-Path $project 'passes/main_rendering_config.json')

@'
{"schema":"pelican.project","version":1,"name":"wp162_behavior_reload","engine_min_version":"0.1.0","basic_config":{"window_title":"WP162","window_size":{"width":160,"height":90},"fullscreen":false,"framerate":30,"camera":{"yfov":0.7853981633974483,"znear":0.1,"zfar":1000.0,"up":[0.0,1.0,0.0]},"default_scene_id":"default_scene","scene_data_json":"scenes/main.scene.json","asset_data_json":"assets/asset_data.json","rendering_config_json":"passes/main_rendering_config.json","default_rendering_pass":"main_render","ui_config_json":"ui/ui_overlay.json"}}
'@ | Set-Content -LiteralPath (Join-Path $project 'project.json') -Encoding ascii
@'
{"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[{"name":"CurrentBehavior","components":[{"name":"behavior","type":"wp162_reload_behavior","params":{"count":2,"label":"committed"}}]}]},"unused_scene":{"objects":[{"name":"OffscreenBehavior","components":[{"name":"behavior","type":"wp162_reload_behavior","params":{"count":99,"label":"offscreen"}}]}]}}}
'@ | Set-Content -LiteralPath (Join-Path $project 'scenes/main.scene.json') -Encoding ascii
'{"models":[]}' | Set-Content -LiteralPath `
    (Join-Path $project 'assets/asset_data.json') -Encoding ascii
'{"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}}' | `
    Set-Content -LiteralPath (Join-Path $project 'ui/ui_overlay.json') -Encoding ascii

$liveDll = Join-Path $OutDir 'wp162_live.dll'
if ($Case -ne 'pending_recovery') {
    Copy-Item -LiteralPath $FixtureV1 -Destination $liveDll
}

$start = [System.Diagnostics.ProcessStartInfo]::new()
$start.FileName = $Player
$start.Arguments = "--headless --rpc --project `"$project`" --game-logic `"$liveDll`" --size 160x90 --fps 30"
$start.WorkingDirectory = $OutDir
$start.UseShellExecute = $false
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $false
$start.CreateNoWindow = $true
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $start
if (-not $process.Start()) { throw 'failed to start WP162 fixture player' }

$rpcId = 0
function Invoke-Rpc([string]$Method) {
    $script:rpcId++
    $request = @{jsonrpc='2.0'; id=$script:rpcId; method=$Method; params=@{}} | `
        ConvertTo-Json -Compress
    $script:process.StandardInput.WriteLine($request)
    $script:process.StandardInput.Flush()
    $line = $script:process.StandardOutput.ReadLine()
    if ([string]::IsNullOrWhiteSpace($line)) {
        throw "empty RPC response for $Method (exited=$($script:process.HasExited))"
    }
    return $line | ConvertFrom-Json
}

function Assert-Step {
    $response = Invoke-Rpc 'step_frame'
    Assert-True ($null -ne $response.result) `
        "step_frame failed: $($response | ConvertTo-Json -Compress)"
}

function Replace-Dll([string]$Path) {
    Copy-Item -LiteralPath $Path -Destination $liveDll -Force
}

try {
    switch ($Case) {
        'code_only' {
            Assert-Step
            Replace-Dll $FixtureCodeV2
            $reload = Invoke-Rpc 'reload_game_logic'
            Assert-True ($reload.result.generation -eq 2) 'code-only reload did not commit generation 2'
            Assert-Step
        }
        'schema_drift' {
            Assert-Step
            Replace-Dll $FixtureDrift
            $reload = Invoke-Rpc 'reload_game_logic'
            Assert-True ($reload.error.message -match 'schema_changed_without_version_bump') `
                "schema drift did not return its stable error: $($reload | ConvertTo-Json -Compress)"
            Assert-Step
        }
        'version_bump_success' {
            Assert-Step
            Replace-Dll $FixtureBumpOk
            $reload = Invoke-Rpc 'reload_game_logic'
            Assert-True ($reload.result.generation -eq 2) 'compatible version bump did not commit'
            Assert-Step
        }
        'version_bump_failure' {
            Assert-Step
            Replace-Dll $FixtureBumpBad
            $reload = Invoke-Rpc 'reload_game_logic'
            $message = [string]$reload.error.message
            Assert-True ($message -match 'schema_incompatible') 'incompatible version bump was not rejected'
            Assert-True ($message -match "stable_name='wp162_reload_behavior'") 'schema error omitted stable name'
            Assert-True ($message -match 'version=1->2') 'schema error omitted versions'
            Assert-True ($message -match "scene='unused_scene'") 'side-decode did not inspect the unused scene'
            Assert-True ($message -match "field='params.count'") 'schema error omitted the field path'
            Assert-Step
        }
        'type_removed' {
            Assert-Step
            Replace-Dll $FixtureRemoved
            $reload = Invoke-Rpc 'reload_game_logic'
            $message = [string]$reload.error.message
            Assert-True ($message -match 'behavior_type_removed') 'behavior type deletion was not a hard error'
            Assert-True ($message -match "stable_name='wp162_reload_behavior'") 'type deletion error omitted stable name'
            Assert-Step
        }
        'candidate_lifecycle_zero' {
            Assert-Step
            Replace-Dll $FixtureDrift
            $reload = Invoke-Rpc 'reload_game_logic'
            Assert-True ($reload.error.message -match 'schema_changed_without_version_bump') `
                'candidate lifecycle probe did not reject drift'
            Assert-Step
        }
        'pending_recovery' {
            Assert-Step
            Replace-Dll $FixtureV1
            $reload = Invoke-Rpc 'reload_game_logic'
            Assert-True ($reload.result.generation -eq 1) 'pending DLL did not recover as generation 1'
            Assert-Step
        }
        'queued_event_purge' {
            Assert-Step
            Replace-Dll $FixtureCodeV2
            $reload = Invoke-Rpc 'reload_game_logic'
            Assert-True ($reload.result.generation -eq 2) 'queued-event reload did not commit'
            Assert-Step
        }
        'oninit_rollback' {
            Assert-Step
            Replace-Dll $FixtureInitFault
            $reload = Invoke-Rpc 'reload_game_logic'
            Assert-True ($reload.error.message -match 'WP162 fixture onInit fault') `
                'onInit fault did not fail the reload transaction'
            Assert-Step
        }
    }
} finally {
    try { $process.StandardInput.Close() } catch {}
    if (-not $process.WaitForExit(30000)) {
        $process.Kill($true)
        throw 'WP162 fixture player did not stop'
    }
}

$logPath = Join-Path $OutDir 'pelican.log'
$log = Get-Content -LiteralPath $logPath -Raw -ErrorAction SilentlyContinue
Assert-True ($process.ExitCode -eq 0) "WP162 fixture player exited with $($process.ExitCode)`n$log"

switch ($Case) {
    'code_only' {
        Assert-True ((Match-Count $log 'WP162 generation=v1 phase=destroy') -eq 1) `
            'code-only reload did not destroy the old instance exactly once'
        Assert-True ((Match-Count $log 'WP162 generation=code_v2 phase=init') -eq 1) `
            'code-only reload did not recreate the new instance exactly once'
        Assert-True ($log -match 'generation=code_v2 phase=init committed_count=2 label=committed internal_state=0') `
            'reload restored runtime-mutated params/internal state instead of committed authoring values'
    }
    'schema_drift' {
        Assert-True ((Match-Count $log 'WP162 generation=drift phase=(init|destroy)') -eq 0) `
            'schema drift candidate ran lifecycle callbacks'
        Assert-True ((Match-Count $log 'WP162 generation=v1 phase=update') -ge 2) `
            'old runtime did not continue after schema drift rejection'
    }
    'version_bump_success' {
        Assert-True ((Match-Count $log 'WP162 generation=bump_ok phase=init') -eq 1) `
            'compatible version bump did not create exactly one candidate instance'
        Assert-True ($log -match 'generation=bump_ok phase=init committed_count=2 label=committed internal_state=0') `
            'compatible version bump did not decode committed params'
    }
    'version_bump_failure' {
        Assert-True ((Match-Count $log 'WP162 generation=bump_bad phase=(init|destroy)') -eq 0) `
            'incompatible candidate ran lifecycle callbacks'
        Assert-True ((Match-Count $log 'WP162 generation=v1 phase=update') -ge 2) `
            'old runtime did not continue after schema incompatibility'
    }
    'type_removed' {
        Assert-True ((Match-Count $log 'WP162 generation=v1 phase=update') -ge 2) `
            'old runtime did not continue after behavior type deletion rejection'
    }
    'candidate_lifecycle_zero' {
        Assert-True ((Match-Count $log 'WP162 generation=drift phase=(init|destroy|update|event)') -eq 0) `
            'candidate validation produced a runtime/lifecycle trace'
        Assert-True ((Match-Count $log 'WP162 generation=v1 phase=init') -eq 1) `
            'candidate validation rebuilt the active runtime'
    }
    'pending_recovery' {
        Assert-True ($log -match 'pending because the game-logic DLL is unavailable') `
            'missing DLL did not preserve the attachment as pending'
        Assert-True ((Match-Count $log 'WP162 generation=v1 phase=init') -eq 1) `
            'pending attachment did not activate exactly once after DLL recovery'
    }
    'queued_event_purge' {
        Assert-True ((Match-Count $log 'WP162 generation=code_v2 phase=event') -eq 0) `
            'an event queued by the unloaded owner crossed the DLL generation boundary'
    }
    'oninit_rollback' {
        Assert-True ((Match-Count $log 'WP162 generation=init_fault phase=init_fault') -eq 1) `
            'faulting candidate onInit did not run exactly once'
        Assert-True ((Match-Count $log 'WP162 generation=init_fault phase=destroy') -eq 0) `
            'failed onInit incorrectly received onDestroy'
        Assert-True ((Match-Count $log 'WP162 generation=v1 phase=init') -eq 2) `
            'old DLL/runtime was not rebuilt exactly once during rollback'
        Assert-True ((Match-Count $log 'WP162 generation=v1 phase=update') -ge 2) `
            'rolled-back old runtime did not continue'
    }
}
