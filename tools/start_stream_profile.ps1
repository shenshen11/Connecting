[CmdletBinding()]
param(
    [string]$Profile = 'mixed_demo',
    [string]$PcIp,
    [string]$HeadsetIp,
    [string]$AdbPath = 'D:\platform-tools-latest-windows\platform-tools\adb.exe',
    [switch]$SkipRuntimeSync,
    [switch]$SkipSenderLaunch,
    [switch]$ListProfiles,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script:ToolsRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $script:ToolsRoot '..'))
$script:ProfilesRoot = Join-Path $script:ToolsRoot 'profiles'
$script:LogsRoot = Join-Path $script:ToolsRoot 'logs'
$script:ResolvedAdbPath = $null

function Get-ConfigValue {
    param(
        [Parameter(Mandatory = $true)]
        [object]$InputObject,
        [Parameter(Mandatory = $true)]
        [string]$PropertyName,
        $DefaultValue = $null
    )

    if ($null -eq $InputObject) {
        return $DefaultValue
    }

    $property = $InputObject.PSObject.Properties[$PropertyName]
    if ($null -eq $property) {
        return $DefaultValue
    }

    return $property.Value
}

function Resolve-AbsolutePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [string]$BaseDirectory = $script:RepoRoot
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $BaseDirectory $Path))
}

function Resolve-ProfilePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProfileRef
    )

    $candidates = New-Object System.Collections.Generic.List[string]

    if ([System.IO.Path]::IsPathRooted($ProfileRef)) {
        $candidates.Add($ProfileRef)
    } else {
        if ($ProfileRef.EndsWith('.json', [System.StringComparison]::OrdinalIgnoreCase)) {
            $candidates.Add((Join-Path $script:ProfilesRoot $ProfileRef))
            $candidates.Add((Join-Path $script:RepoRoot $ProfileRef))
        } else {
            $candidates.Add((Join-Path $script:ProfilesRoot ($ProfileRef + '.json')))
            $candidates.Add((Join-Path $script:ProfilesRoot $ProfileRef))
            $candidates.Add((Join-Path $script:RepoRoot ($ProfileRef + '.json')))
            $candidates.Add((Join-Path $script:RepoRoot $ProfileRef))
        }
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "Profile not found: $ProfileRef"
}

function Load-ProfileConfig {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProfileRef
    )

    $profilePath = Resolve-ProfilePath -ProfileRef $ProfileRef
    $profile = Get-Content -LiteralPath $profilePath -Raw -Encoding UTF8 | ConvertFrom-Json

    if ($null -eq (Get-ConfigValue -InputObject $profile -PropertyName 'name')) {
        $profile | Add-Member -MemberType NoteProperty -Name 'name' -Value ([System.IO.Path]::GetFileNameWithoutExtension($profilePath))
    }

    $profile | Add-Member -MemberType NoteProperty -Name '_profilePath' -Value $profilePath -Force
    return $profile
}

function Show-AvailableProfiles {
    $profiles = Get-ChildItem -LiteralPath $script:ProfilesRoot -Filter '*.json' | Sort-Object Name
    if ($profiles.Count -eq 0) {
        Write-Output 'No profiles found.'
        return
    }

    $rows = foreach ($file in $profiles) {
        $profile = Load-ProfileConfig -ProfileRef $file.FullName
        $desktopSender = Get-ConfigValue -InputObject $profile -PropertyName 'desktopSender'
        [PSCustomObject]@{
            Profile       = Get-ConfigValue -InputObject $profile -PropertyName 'name' ([System.IO.Path]::GetFileNameWithoutExtension($file.Name))
            Description   = Get-ConfigValue -InputObject $profile -PropertyName 'description' ''
            DesktopSender = [bool](Get-ConfigValue -InputObject $desktopSender -PropertyName 'enabled' $false)
        }
    }

    $rows | Format-Table -AutoSize
}

function Resolve-AdbPath {
    param([string]$RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath) -and (Test-Path -LiteralPath $RequestedPath)) {
        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }

    $adbCommand = Get-Command adb -ErrorAction SilentlyContinue
    if ($null -ne $adbCommand -and -not [string]::IsNullOrWhiteSpace($adbCommand.Source)) {
        return $adbCommand.Source
    }

    throw "adb not found. Checked: $RequestedPath and PATH."
}

function Invoke-Adb {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [switch]$AllowFailure
    )

    $output = & $script:ResolvedAdbPath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0 -and -not $AllowFailure) {
        $message = ($output | Out-String).Trim()
        if ([string]::IsNullOrWhiteSpace($message)) {
            $message = 'no output'
        }
        throw "adb failed: $($Arguments -join ' ')`n$message"
    }

    return ($output | Out-String).Trim()
}

function Assert-AdbReady {
    $state = Invoke-Adb -Arguments @('get-state') -AllowFailure
    if ($state -notmatch 'device') {
        throw "No adb device is ready. Resolved adb path: $script:ResolvedAdbPath"
    }
}

function Resolve-HeadsetIp {
    param(
        [string]$ExplicitIp,
        [string]$PreferredInterface = 'wlan0'
    )

    if (-not [string]::IsNullOrWhiteSpace($ExplicitIp)) {
        return $ExplicitIp.Trim()
    }

    Assert-AdbReady
    $addressListing = Invoke-Adb -Arguments @('shell', 'ip', '-o', '-4', 'addr', 'show')
    $lines = @($addressListing -split "`r?`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })

    $preferredPattern = '^\d+:\s+' + [regex]::Escape($PreferredInterface) + '\s+inet\s+(\d+\.\d+\.\d+\.\d+)/'
    foreach ($line in $lines) {
        if ($line -match $preferredPattern) {
            return $matches[1]
        }
    }

    foreach ($line in $lines) {
        if ($line -match '^\d+:\s+\S+\s+inet\s+(\d+\.\d+\.\d+\.\d+)/' -and $matches[1] -ne '127.0.0.1') {
            return $matches[1]
        }
    }

    throw "Unable to resolve headset IPv4 from adb output.`n$addressListing"
}

function Ensure-Directory {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        $null = New-Item -ItemType Directory -Path $Path -Force
    }
}

function Stop-ExistingSender {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExecutablePath
    )

    $resolvedExecutablePath = [System.IO.Path]::GetFullPath($ExecutablePath)
    $fileName = [System.IO.Path]::GetFileName($resolvedExecutablePath)
    $running = Get-CimInstance Win32_Process -Filter ("Name='{0}'" -f $fileName) -ErrorAction SilentlyContinue

    foreach ($process in @($running)) {
        $matchesPath = $false
        if (-not [string]::IsNullOrWhiteSpace($process.ExecutablePath)) {
            $matchesPath = [string]::Equals(
                [System.IO.Path]::GetFullPath($process.ExecutablePath),
                $resolvedExecutablePath,
                [System.StringComparison]::OrdinalIgnoreCase)
        }

        if ($matchesPath -or [string]::IsNullOrWhiteSpace($process.ExecutablePath)) {
            Stop-Process -Id $process.ProcessId -Force -ErrorAction Stop
        }
    }
}

function Invoke-RuntimeSync {
    param(
        [Parameter(Mandatory = $true)]
        [object]$SyncConfig,
        [Parameter(Mandatory = $true)]
        [string]$ResolvedAdbPathValue
    )

    $syncScriptPath = Join-Path $script:ToolsRoot 'sync_editor_runtime.ps1'
    if (-not (Test-Path -LiteralPath $syncScriptPath)) {
        throw "Runtime sync script not found: $syncScriptPath"
    }

    $syncParameters = [ordered]@{
        AdbPath          = $ResolvedAdbPathValue
        VideoPort        = [uint16](Get-ConfigValue -InputObject $SyncConfig -PropertyName 'videoPort' 25673)
        EncodedVideoPort = [uint16](Get-ConfigValue -InputObject $SyncConfig -PropertyName 'encodedVideoPort' 25674)
    }

    $displayMode = Get-ConfigValue -InputObject $SyncConfig -PropertyName 'displayMode' $null
    if (-not [string]::IsNullOrWhiteSpace([string]$displayMode)) {
        $syncParameters.DisplayMode = [string]$displayMode
    }

    $poseTargetPort = Get-ConfigValue -InputObject $SyncConfig -PropertyName 'poseTargetPort' $null
    if ($null -ne $poseTargetPort) {
        $syncParameters.PoseTargetPort = [uint16]$poseTargetPort
    }

    $controlTargetPort = Get-ConfigValue -InputObject $SyncConfig -PropertyName 'controlTargetPort' $null
    if ($null -ne $controlTargetPort) {
        $syncParameters.ControlTargetPort = [uint16]$controlTargetPort
    }

    if (-not [string]::IsNullOrWhiteSpace($PcIp)) {
        $syncParameters.PcIp = $PcIp.Trim()
    }

    if ([bool](Get-ConfigValue -InputObject $SyncConfig -PropertyName 'skipUnityCacheReset' $false)) {
        $syncParameters.SkipUnityCacheReset = $true
    }

    if ($DryRun) {
        $dryRunParts = New-Object System.Collections.Generic.List[string]
        foreach ($entry in $syncParameters.GetEnumerator()) {
            if ($entry.Value -is [System.Management.Automation.SwitchParameter] -or $entry.Value -is [bool]) {
                if ([bool]$entry.Value) {
                    $dryRunParts.Add('-' + $entry.Key)
                }
            } else {
                $dryRunParts.Add('-' + $entry.Key)
                $dryRunParts.Add([string]$entry.Value)
            }
        }

        Write-Output ('[DryRun] sync_editor_runtime.ps1 ' + ($dryRunParts -join ' '))
        return
    }

    & $syncScriptPath @syncParameters
}

function Start-DesktopSender {
    param(
        [Parameter(Mandatory = $true)]
        [object]$DesktopSenderConfig,
        [Parameter(Mandatory = $true)]
        [string]$ResolvedHeadsetIp,
        [Parameter(Mandatory = $true)]
        [string]$ProfileName
    )

    $executable = Get-ConfigValue -InputObject $DesktopSenderConfig -PropertyName 'executable'
    if ([string]::IsNullOrWhiteSpace($executable)) {
        throw 'desktopSender.executable is required when desktopSender.enabled=true.'
    }

    $senderPath = Resolve-AbsolutePath -Path $executable
    if (-not (Test-Path -LiteralPath $senderPath)) {
        throw "Desktop sender executable not found: $senderPath"
    }

    if ([bool](Get-ConfigValue -InputObject $DesktopSenderConfig -PropertyName 'restartIfRunning' $true)) {
        Stop-ExistingSender -ExecutablePath $senderPath
    }

    Ensure-Directory -Path $script:LogsRoot

    $timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $logPrefix = '{0}_{1}_desktop_sender' -f $timestamp, $ProfileName
    $stdoutPath = Join-Path $script:LogsRoot ($logPrefix + '.out.log')
    $stderrPath = Join-Path $script:LogsRoot ($logPrefix + '.err.log')

    $arguments = @(
        $ResolvedHeadsetIp,
        [string](Get-ConfigValue -InputObject $DesktopSenderConfig -PropertyName 'videoPort' 25674),
        [string](Get-ConfigValue -InputObject $DesktopSenderConfig -PropertyName 'fps' 10),
        [string](Get-ConfigValue -InputObject $DesktopSenderConfig -PropertyName 'bitrate' 4000000),
        [string](Get-ConfigValue -InputObject $DesktopSenderConfig -PropertyName 'outputIndex' 0),
        [string](Get-ConfigValue -InputObject $DesktopSenderConfig -PropertyName 'width' 1280),
        [string](Get-ConfigValue -InputObject $DesktopSenderConfig -PropertyName 'height' 720),
        [string](Get-ConfigValue -InputObject $DesktopSenderConfig -PropertyName 'controlPort' 25672)
    )

    if ($DryRun) {
        return [PSCustomObject]@{
            DryRun       = $true
            SenderPath   = $senderPath
            Arguments    = $arguments -join ' '
            StdoutLog    = $stdoutPath
            StderrLog    = $stderrPath
            HeadsetIp    = $ResolvedHeadsetIp
            ProcessId    = $null
        }
    }

    $process = Start-Process -FilePath $senderPath `
        -ArgumentList $arguments `
        -WorkingDirectory (Split-Path -Parent $senderPath) `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -PassThru

    Start-Sleep -Milliseconds 800
    if ($process.HasExited) {
        $stderrPreview = if (Test-Path -LiteralPath $stderrPath) { Get-Content -LiteralPath $stderrPath -Raw } else { '' }
        $stdoutPreview = if (Test-Path -LiteralPath $stdoutPath) { Get-Content -LiteralPath $stdoutPath -Raw } else { '' }
        throw "Desktop sender exited immediately.`nSTDOUT:`n$stdoutPreview`nSTDERR:`n$stderrPreview"
    }

    return [PSCustomObject]@{
        DryRun       = $false
        SenderPath   = $senderPath
        Arguments    = $arguments -join ' '
        StdoutLog    = $stdoutPath
        StderrLog    = $stderrPath
        HeadsetIp    = $ResolvedHeadsetIp
        ProcessId    = $process.Id
    }
}

if ($ListProfiles) {
    Show-AvailableProfiles
    return
}

$profileConfig = Load-ProfileConfig -ProfileRef $Profile
$profileName = [string](Get-ConfigValue -InputObject $profileConfig -PropertyName 'name' ([System.IO.Path]::GetFileNameWithoutExtension($profileConfig._profilePath)))
$profileDescription = [string](Get-ConfigValue -InputObject $profileConfig -PropertyName 'description' '')
$syncConfig = Get-ConfigValue -InputObject $profileConfig -PropertyName 'syncRuntime'
$desktopSenderConfig = Get-ConfigValue -InputObject $profileConfig -PropertyName 'desktopSender'
$nextSteps = @(Get-ConfigValue -InputObject $profileConfig -PropertyName 'nextSteps' @())

$runtimeSyncEnabled = [bool](Get-ConfigValue -InputObject $syncConfig -PropertyName 'enabled' $false)
$desktopSenderEnabled = [bool](Get-ConfigValue -InputObject $desktopSenderConfig -PropertyName 'enabled' $false)
$needsAdb = $runtimeSyncEnabled -or ($desktopSenderEnabled -and [string]::IsNullOrWhiteSpace($HeadsetIp))

if ($needsAdb) {
    $script:ResolvedAdbPath = Resolve-AdbPath -RequestedPath $AdbPath
}

Write-Output "Profile: $profileName"
if (-not [string]::IsNullOrWhiteSpace($profileDescription)) {
    Write-Output "Description: $profileDescription"
}
Write-Output "Profile file: $($profileConfig._profilePath)"

if ($runtimeSyncEnabled -and -not $SkipRuntimeSync) {
    Write-Output '--- Sync headset runtime ---'
    Invoke-RuntimeSync -SyncConfig $syncConfig -ResolvedAdbPathValue $script:ResolvedAdbPath
} elseif ($runtimeSyncEnabled -and $SkipRuntimeSync) {
    Write-Output '--- Sync headset runtime skipped ---'
}

$senderResult = $null
if ($desktopSenderEnabled -and -not $SkipSenderLaunch) {
    Write-Output '--- Start desktop sender ---'
    $preferredInterface = [string](Get-ConfigValue -InputObject $desktopSenderConfig -PropertyName 'preferredInterface' 'wlan0')
    $resolvedHeadsetIp = Resolve-HeadsetIp -ExplicitIp $HeadsetIp -PreferredInterface $preferredInterface
    $senderResult = Start-DesktopSender -DesktopSenderConfig $desktopSenderConfig -ResolvedHeadsetIp $resolvedHeadsetIp -ProfileName $profileName
} elseif ($desktopSenderEnabled -and $SkipSenderLaunch) {
    Write-Output '--- Desktop sender launch skipped ---'
}

Write-Output '--- Summary ---'
if ($needsAdb) {
    Write-Output "adb: $script:ResolvedAdbPath"
}

if ($null -ne $senderResult) {
    Write-Output "Headset IPv4: $($senderResult.HeadsetIp)"
    Write-Output "Desktop sender: $($senderResult.SenderPath)"
    Write-Output "Desktop sender args: $($senderResult.Arguments)"
    Write-Output "Desktop sender stdout log: $($senderResult.StdoutLog)"
    Write-Output "Desktop sender stderr log: $($senderResult.StderrLog)"

    if ($null -ne $senderResult.ProcessId) {
        Write-Output "Desktop sender PID: $($senderResult.ProcessId)"
    }
}

if ($nextSteps.Count -gt 0) {
    Write-Output 'Next steps:'
    foreach ($step in $nextSteps) {
        Write-Output (" - {0}" -f $step)
    }
}
