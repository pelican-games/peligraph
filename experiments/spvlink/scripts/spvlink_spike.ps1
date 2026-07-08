param(
    [Parameter(Mandatory = $true)]
    [string]$Root
)

$ErrorActionPreference = "Stop"

$FunctionExportName = "pelican_surface"
$UserSet = 2
$FirstFreeMaterialBinding = 6

function Resolve-Tool([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    if ($env:VULKAN_SDK) {
        $candidate = Join-Path $env:VULKAN_SDK "Bin\$Name.exe"
        if (Test-Path $candidate) {
            return $candidate
        }
    }
    throw "required tool not found: $Name"
}

function Invoke-SpvTool([string[]]$CommandArgs) {
    Write-Host ("+ " + ($CommandArgs -join " "))
    & $CommandArgs[0] @($CommandArgs[1..($CommandArgs.Length - 1)])
    if ($LASTEXITCODE -ne 0) {
        throw "command failed: $($CommandArgs -join ' ')"
    }
}

function Read-Lines([string]$Path) {
    return [System.IO.File]::ReadAllLines($Path)
}

function Write-Lines([string]$Path, [string[]]$Lines) {
    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, (($Lines -join "`n") + "`n"), $encoding)
}

function Add-LinkageCapability([string[]]$Lines) {
    foreach ($line in $Lines) {
        if ($line.Trim() -eq "OpCapability Linkage") {
            return $Lines
        }
    }

    $out = New-Object System.Collections.Generic.List[string]
    $inserted = $false
    foreach ($line in $Lines) {
        $out.Add($line)
        if (-not $inserted -and $line.Trim().StartsWith("OpCapability ")) {
            $out.Add("               OpCapability Linkage")
            $inserted = $true
        }
    }
    if (-not $inserted) {
        throw "could not insert OpCapability Linkage"
    }
    return $out.ToArray()
}

function Find-NamedId([string[]]$Lines, [string]$Prefix) {
    foreach ($line in $Lines) {
        if ($line -match '^\s*OpName\s+(%\S+)\s+"([^"]+)"') {
            if ($Matches[2].StartsWith($Prefix)) {
                return $Matches[1]
            }
        }
    }
    throw "could not find SPIR-V id named $Prefix"
}

function Find-EntryPointId([string[]]$Lines, [string]$Name = "main") {
    foreach ($line in $Lines) {
        if ($line -match '^\s*OpEntryPoint\s+\w+\s+(%\S+)\s+"([^"]+)"') {
            if ($Matches[2] -eq $Name) {
                return $Matches[1]
            }
        }
    }
    throw "could not find entry point $Name"
}

function Add-FunctionLinkageDecoration([string[]]$Lines, [string]$FunctionId, [string]$Linkage) {
    $decoration = "               OpDecorate $FunctionId LinkageAttributes `"$FunctionExportName`" $Linkage"
    $out = New-Object System.Collections.Generic.List[string]
    $inserted = $false
    foreach ($line in $Lines) {
        if ($line.Contains("OpDecorate $FunctionId LinkageAttributes")) {
            if (-not $inserted) {
                $out.Add($decoration)
                $inserted = $true
            }
            continue
        }
        $out.Add($line)
        if (-not $inserted -and $line.Trim().StartsWith("OpName ")) {
            $out.Add($decoration)
            $inserted = $true
        }
    }
    if (-not $inserted) {
        throw "could not add linkage decoration for $FunctionId"
    }
    return $out.ToArray()
}

function Strip-FunctionBodyToImport([string[]]$Lines, [string]$FunctionId) {
    $out = New-Object System.Collections.Generic.List[string]
    $i = 0
    while ($i -lt $Lines.Length) {
        $line = $Lines[$i]
        if ($line -match "^\s*$([regex]::Escape($FunctionId))\s*=\s*OpFunction\b") {
            $out.Add($line)
            $i++
            while ($i -lt $Lines.Length) {
                $current = $Lines[$i]
                $trimmed = $current.Trim()
                if ($trimmed.Contains("OpFunctionParameter")) {
                    $out.Add($current)
                    $i++
                    continue
                }
                if ($trimmed -eq "OpFunctionEnd") {
                    $out.Add($current)
                    $i++
                    while ($i -lt $Lines.Length) {
                        $out.Add($Lines[$i])
                        $i++
                    }
                    return $out.ToArray()
                }
                $i++
            }
            throw "unterminated function $FunctionId"
        }
        $out.Add($line)
        $i++
    }
    throw "function not found: $FunctionId"
}

function Remove-DebugNames([string[]]$Lines) {
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($line in $Lines) {
        $trimmed = $line.Trim()
        if ($trimmed.StartsWith("OpName ") -or $trimmed.StartsWith("OpMemberName ")) {
            continue
        }
        $out.Add($line)
    }
    return $out.ToArray()
}

function Remove-MemberLayoutDecorations([string[]]$Lines) {
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($line in $Lines) {
        if ($line.Trim().StartsWith("OpMemberDecorate ")) {
            continue
        }
        $out.Add($line)
    }
    return $out.ToArray()
}

function Remove-Function([string[]]$Lines, [string]$FunctionId) {
    $out = New-Object System.Collections.Generic.List[string]
    $removed = $false
    $i = 0
    while ($i -lt $Lines.Length) {
        $line = $Lines[$i]
        if ($line -match "^\s*$([regex]::Escape($FunctionId))\s*=\s*OpFunction\b") {
            $removed = $true
            $i++
            while ($i -lt $Lines.Length -and $Lines[$i].Trim() -ne "OpFunctionEnd") {
                $i++
            }
            if ($i -lt $Lines.Length) {
                $i++
            }
            continue
        }
        $out.Add($line)
        $i++
    }
    if (-not $removed) {
        throw "function not found for removal: $FunctionId"
    }
    return $out.ToArray()
}

function Remove-EntryPointMetadata([string[]]$Lines, [string]$EntryId) {
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($line in $Lines) {
        if ($line -match "^\s*OpEntryPoint\s+\w+\s+$([regex]::Escape($EntryId))\b") {
            continue
        }
        if ($line -match "^\s*OpExecutionMode\s+$([regex]::Escape($EntryId))\b") {
            continue
        }
        if ($line -match "^\s*OpName\s+$([regex]::Escape($EntryId))\b") {
            continue
        }
        $out.Add($line)
    }
    return $out.ToArray()
}

function Get-DescriptorNames([string[]]$Lines) {
    $names = @{}
    foreach ($line in $Lines) {
        if ($line -match '^\s*OpName\s+(%\S+)\s+"([^"]+)"') {
            $names[$Matches[1]] = $Matches[2]
        }
    }
    return $names
}

function Get-Descriptors([string[]]$Lines) {
    $names = Get-DescriptorNames $Lines
    $sets = @{}
    $bindings = @{}
    foreach ($line in $Lines) {
        if ($line -match '^\s*OpDecorate\s+(%\S+)\s+DescriptorSet\s+(\d+)') {
            $sets[$Matches[1]] = [int]$Matches[2]
        }
        if ($line -match '^\s*OpDecorate\s+(%\S+)\s+Binding\s+(\d+)') {
            $bindings[$Matches[1]] = [int]$Matches[2]
        }
    }
    $descriptors = @()
    foreach ($id in $sets.Keys) {
        if ($bindings.ContainsKey($id)) {
            $name = ""
            if ($names.ContainsKey($id)) {
                $name = $names[$id]
            }
            $descriptors += [pscustomobject]@{
                id = $id
                name = $name
                set = $sets[$id]
                binding = $bindings[$id]
            }
        }
    }
    return $descriptors | Sort-Object id
}

function Remap-UserDescriptors([string[]]$Lines) {
    $descriptors = Get-Descriptors $Lines
    $userIds = @(
        $descriptors |
            Where-Object { $_.set -eq $UserSet -and $_.binding -lt $FirstFreeMaterialBinding } |
            Sort-Object set, binding, id |
            ForEach-Object { $_.id }
    )

    $remap = @{}
    for ($i = 0; $i -lt $userIds.Count; ++$i) {
        $remap[$userIds[$i]] = $FirstFreeMaterialBinding + $i
    }

    $mapping = @()
    foreach ($desc in $descriptors) {
        if ($remap.ContainsKey($desc.id)) {
            $mapping += [pscustomobject]@{
                id = $desc.id
                name = $desc.name
                from = [pscustomobject]@{ set = $desc.set; binding = $desc.binding }
                to = [pscustomobject]@{ set = $UserSet; binding = $remap[$desc.id] }
            }
        }
    }

    $out = New-Object System.Collections.Generic.List[string]
    foreach ($line in $Lines) {
        if ($line -match '^(\s*OpDecorate\s+)(%\S+)(\s+Binding\s+)(\d+)(.*)') {
            $id = $Matches[2]
            if ($remap.ContainsKey($id)) {
                $out.Add("$($Matches[1])$id$($Matches[3])$($remap[$id])$($Matches[5])")
                continue
            }
        }
        $out.Add($line)
    }
    return [pscustomobject]@{
        lines = $out.ToArray()
        mapping = $mapping
    }
}

function Patch-Template([string]$RawAsm, [string]$PatchedAsm) {
    $lines = Add-LinkageCapability (Read-Lines $RawAsm)
    $surfaceId = Find-NamedId $lines $FunctionExportName
    $lines = Add-FunctionLinkageDecoration $lines $surfaceId "Import"
    $lines = Strip-FunctionBodyToImport $lines $surfaceId
    $lines = Remove-MemberLayoutDecorations $lines
    $lines = Remove-DebugNames $lines
    Write-Lines $PatchedAsm $lines
}

function Patch-UserLibrary([string]$RawAsm, [string]$PatchedAsm) {
    $lines = Add-LinkageCapability (Read-Lines $RawAsm)
    $surfaceId = Find-NamedId $lines $FunctionExportName
    $entryId = Find-EntryPointId $lines
    $lines = Remove-EntryPointMetadata $lines $entryId
    $lines = Remove-Function $lines $entryId
    $lines = Add-FunctionLinkageDecoration $lines $surfaceId "Export"
    $result = Remap-UserDescriptors $lines
    $patched = Remove-MemberLayoutDecorations $result.lines
    $patched = Remove-DebugNames $patched
    Write-Lines $PatchedAsm $patched
    return $result.mapping
}

function Disassemble([string]$Spv, [string]$Asm) {
    Invoke-SpvTool @((Resolve-Tool "spirv-dis"), $Spv, "-o", $Asm)
}

function Assemble([string]$Asm, [string]$Spv) {
    Invoke-SpvTool @((Resolve-Tool "spirv-as"), "--target-env", "spv1.5", $Asm, "-o", $Spv)
}

function Optimize-AndValidate([string]$Linked, [string]$Final) {
    Invoke-SpvTool @(
        (Resolve-Tool "spirv-opt"),
        "--target-env=vulkan1.2",
        "--inline-entry-points-exhaustive",
        "--eliminate-dead-functions",
        "--eliminate-dead-code-aggressive",
        "--eliminate-dead-variables",
        "--compact-ids",
        $Linked,
        "-o",
        $Final
    )
    Invoke-SpvTool @((Resolve-Tool "spirv-val"), "--target-env", "vulkan1.2", $Final)
}

function Compile-Glsl([string]$Source, [string]$Output, [string[]]$Defines = @()) {
    $stage = "frag"
    if ([System.IO.Path]::GetFileName($Source).Contains(".vert")) {
        $stage = "vert"
    }
    $commandArgs = @(
        (Resolve-Tool "glslangValidator"),
        "-V",
        "--target-env",
        "vulkan1.2",
        "-S",
        $stage,
        "--keep-uncalled"
    )
    foreach ($define in $Defines) {
        $commandArgs += "-D$define"
    }
    $commandArgs += @($Source, "-o", $Output)
    Invoke-SpvTool $commandArgs
}

function Compile-Slang([string]$Source, [string]$Output, [string[]]$Defines = @()) {
    $commandArgs = @(
        (Resolve-Tool "slangc"),
        "-target",
        "spirv",
        "-profile",
        "ps_6_0+spirv_1_5",
        "-entry",
        "main",
        "-stage",
        "fragment",
        "-O0",
        "-emit-spirv-directly",
        "-fvk-use-entrypoint-name"
    )
    foreach ($define in $Defines) {
        $commandArgs += "-D$define"
    }
    $commandArgs += @($Source, "-o", $Output)
    Invoke-SpvTool $commandArgs
}

function Link-Case([string]$Name, [string]$TemplateSpv, [string]$UserRawSpv, [string]$OutDir) {
    $userRawAsm = Join-Path $OutDir "$Name.user.raw.spvasm"
    $userExportAsm = Join-Path $OutDir "$Name.user.export.spvasm"
    $userExportSpv = Join-Path $OutDir "$Name.user.export.spv"
    $linkedSpv = Join-Path $OutDir "$Name.linked.spv"
    $finalSpv = Join-Path $OutDir "$Name.final.spv"

    Disassemble $UserRawSpv $userRawAsm
    $mapping = Patch-UserLibrary $userRawAsm $userExportAsm
    Assemble $userExportAsm $userExportSpv
    Invoke-SpvTool @(
        (Resolve-Tool "spirv-link"),
        "--target-env",
        "vulkan1.2",
        "--verify-ids",
        $TemplateSpv,
        $userExportSpv,
        "-o",
        $linkedSpv
    )
    Optimize-AndValidate $linkedSpv $finalSpv
    Disassemble $finalSpv (Join-Path $OutDir "$Name.final.spvasm")
    return $mapping
}

$rootPath = (Resolve-Path $Root).Path
$shaderDir = Join-Path $rootPath "shaders"
$outDir = Join-Path $rootPath "out"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Compile-Glsl (Join-Path $shaderDir "fullscreen.vert.glsl") (Join-Path $outDir "fullscreen.vert.spv")
Compile-Glsl (Join-Path $shaderDir "template.frag.glsl") (Join-Path $outDir "template.raw.spv")

Disassemble (Join-Path $outDir "template.raw.spv") (Join-Path $outDir "template.raw.spvasm")
Patch-Template (Join-Path $outDir "template.raw.spvasm") (Join-Path $outDir "template.import.spvasm")
Assemble (Join-Path $outDir "template.import.spvasm") (Join-Path $outDir "template.import.spv")

Compile-Glsl (Join-Path $shaderDir "surface.glsl") (Join-Path $outDir "surface_glsl_warm.raw.spv") @("PELICAN_VARIANT_WARM")
Compile-Glsl (Join-Path $shaderDir "surface.glsl") (Join-Path $outDir "surface_glsl_cool.raw.spv")
Compile-Slang (Join-Path $shaderDir "surface.slang") (Join-Path $outDir "surface_slang_warm.raw.spv") @("PELICAN_VARIANT_WARM")

$templateSpv = Join-Path $outDir "template.import.spv"
$bindings = [ordered]@{
    glsl_warm = @(Link-Case "glsl_warm" $templateSpv (Join-Path $outDir "surface_glsl_warm.raw.spv") $outDir)
    glsl_cool = @(Link-Case "glsl_cool" $templateSpv (Join-Path $outDir "surface_glsl_cool.raw.spv") $outDir)
    slang_warm = @(Link-Case "slang_warm" $templateSpv (Join-Path $outDir "surface_slang_warm.raw.spv") $outDir)
}

$json = $bindings | ConvertTo-Json -Depth 8
$encoding = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText((Join-Path $outDir "bindings.json"), ($json + "`n"), $encoding)
Write-Host "wrote $(Join-Path $outDir 'bindings.json')"
