param(
    [Parameter(Mandatory=$true)][string]$Player,
    [Parameter(Mandatory=$true)][string]$Cli,
    [Parameter(Mandatory=$true)][string]$OutDir,
    [Parameter(Mandatory=$true)][int]$ExpectedGui
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null

# Inspect the built PE, not just the requested CMake option.
$pe = [IO.File]::ReadAllBytes($Player)
$peOffset = [BitConverter]::ToInt32($pe, 0x3c)
$subsystem = [BitConverter]::ToUInt16($pe, $peOffset + 24 + 68)
$expectedSubsystem = if ($ExpectedGui) { 2 } else { 3 }
if ($subsystem -ne $expectedSubsystem) {
    throw "player subsystem=$subsystem, expected=$expectedSubsystem"
}

$savedSuppression = $env:PELICAN_NO_MESSAGEBOX
try {
    Remove-Item Env:PELICAN_NO_MESSAGEBOX -ErrorAction SilentlyContinue
    foreach ($mode in @('standalone', 'headless', 'rpc', 'embedded', 'console', 'help')) {
        $args = if ($mode -eq 'help') { '--console --help' } else {
            '--project "' + (Join-Path $OutDir 'missing-project') + '"'
        }
        if ($mode -ne 'standalone' -and $mode -ne 'help') { $args += " --$mode" }
        $start = New-Object System.Diagnostics.ProcessStartInfo
        $start.FileName = $Player
        $start.Arguments = $args
        $start.WorkingDirectory = $OutDir
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $child = New-Object System.Diagnostics.Process
        $child.StartInfo = $start
        try {
            if (-not $child.Start()) { throw "could not start $mode" }
            $stdout = $child.StandardOutput.ReadToEndAsync()
            $stderr = $child.StandardError.ReadToEndAsync()
            $child.WaitForExit()
            $exitCode = $child.ExitCode
            $out = $stdout.Result
            $err = $stderr.Result
            [IO.File]::WriteAllText((Join-Path $OutDir "$mode.log"), "$out`n$err")
            if ($mode -eq 'help') {
                if ($exitCode -ne 0 -or $out -notmatch '--headless') {
                    throw "help: exit=$exitCode, stdout pipe was not preserved: $out $err"
                }
            } elseif ($exitCode -eq 0 -or $err -notmatch 'missing-project') {
                throw "$mode : exit=$exitCode, missing fatal stderr: $err"
            }
            Write-Output "$mode : WaitForExit/ExitCode=$exitCode, captured output OK"
        } finally { $child.Dispose() }
    }

    # Exercise disk redirection as well as ProcessStartInfo's pipes.
    $diskOut = Join-Path $OutDir 'disk.stdout'
    $diskErr = Join-Path $OutDir 'disk.stderr'
    $diskArgs = '--project "' + (Join-Path $OutDir 'missing-project') + '"'
    $child = Start-Process -FilePath $Player -ArgumentList $diskArgs -WindowStyle Hidden `
        -RedirectStandardOutput $diskOut -RedirectStandardError $diskErr -Wait -PassThru
    try {
        if ($null -eq $child.ExitCode -or $child.ExitCode -eq 0 -or
            [IO.File]::ReadAllText($diskErr) -notmatch 'missing-project') {
            throw "disk redirection: invalid exit code or missing fatal stderr"
        }
        Write-Output "disk: Start-Process -Wait -PassThru ExitCode=$($child.ExitCode)"
    } finally { $child.Dispose() }
} finally { $env:PELICAN_NO_MESSAGEBOX = $savedSuppression }

$project = Join-Path $OutDir 'project'
New-Item -ItemType Directory -Force $project | Out-Null
[IO.File]::WriteAllText((Join-Path $project 'project.json'), @'
{"schema":"pelican.project","version":1,"name":"DS-1f distribution","engine_min_version":"0.1.0","basic_config":{"asset_data_json":"assets.json","ui_config_json":"ui.json"}}
'@)
[IO.File]::WriteAllText((Join-Path $project 'assets.json'), '{"schema":"pelican.asset_data","version":2,"models":[]}')
[IO.File]::WriteAllText((Join-Path $project 'ui.json'), '{"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}}')
foreach ($withStudio in @($false, $true)) {
    $preset = Join-Path $OutDir "dist-$withStudio.cmake"
    $cliArgs = @('dist-config', $project, '--out', $preset)
    if ($withStudio) { $cliArgs += '--with-studio' }
    & $Cli @cliArgs
    if ($LASTEXITCODE -ne 0) { throw 'dist-config failed' }
    $contents = [IO.File]::ReadAllText($preset)
    if (-not $contents.Contains('set(PELICAN_PLAYER_GUI_SUBSYSTEM ON CACHE BOOL "" FORCE)')) {
        throw 'distribution preset must enable the GUI subsystem'
    }
}
Write-Output "DS-1f startup PASS (PE subsystem=$subsystem)"
