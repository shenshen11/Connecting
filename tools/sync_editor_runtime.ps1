[CmdletBinding()]
param(
    [string]$PcIp,
    [string]$AdbPath = 'D:\platform-tools-latest-windows\platform-tools\adb.exe',
    [string]$PackageName = 'com.videotest.nativeapp',
    [string]$ActivityName = 'android.app.NativeActivity',
    [ValidateSet('quad_mono', 'projection_mono', 'projection_stereo')]
    [string]$DisplayMode = 'quad_mono',
    [UInt16]$TargetPort = 0,
    [UInt16]$PoseTargetPort = 0,
    [UInt16]$ControlTargetPort = 0,
    [UInt16]$VideoPort = 25673,
    [UInt16]$EncodedVideoPort = 25674,
    [string]$UnitySavedEndpointPath = (Join-Path $env:USERPROFILE 'AppData\LocalLow\DefaultCompany\pc-unity-app\VideoTestUnitySender\last_successful_endpoint.json'),
    [switch]$SkipUnityCacheReset
)

$ErrorActionPreference = 'Stop'

function Resolve-PcIp {
    param([string]$ExplicitIp)

    if (-not [string]::IsNullOrWhiteSpace($ExplicitIp)) {
        return $ExplicitIp.Trim()
    }

    $configs = Get-NetIPConfiguration | Where-Object {
        $_.IPv4Address -and
        $_.IPv4DefaultGateway
    }

    foreach ($config in $configs) {
        $hasRealGateway = $false
        foreach ($gateway in $config.IPv4DefaultGateway) {
            if ($null -ne $gateway -and
                -not [string]::IsNullOrWhiteSpace($gateway.NextHop) -and
                $gateway.NextHop -ne '0.0.0.0') {
                $hasRealGateway = $true
                break
            }
        }

        if (-not $hasRealGateway) {
            continue
        }

        foreach ($address in $config.IPv4Address) {
            if ($null -ne $address -and
                -not [string]::IsNullOrWhiteSpace($address.IPAddress) -and
                -not $address.IPAddress.StartsWith('169.254.')) {
                return $address.IPAddress
            }
        }
    }

    throw 'Unable to resolve the active PC IPv4 address. Pass -PcIp explicitly.'
}

function Invoke-Adb {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [switch]$SuppressOutput
    )

    $stdoutPath = Join-Path ([System.IO.Path]::GetTempPath()) ("videotest_adb_stdout_{0}.log" -f [guid]::NewGuid().ToString('N'))
    $stderrPath = Join-Path ([System.IO.Path]::GetTempPath()) ("videotest_adb_stderr_{0}.log" -f [guid]::NewGuid().ToString('N'))

    try {
        $process = Start-Process -FilePath $script:ResolvedAdbPath `
            -ArgumentList $Arguments `
            -NoNewWindow `
            -Wait `
            -PassThru `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath

        $stdout = if (Test-Path -LiteralPath $stdoutPath) { Get-Content -LiteralPath $stdoutPath -Raw } else { '' }
        $stderr = if (Test-Path -LiteralPath $stderrPath) { Get-Content -LiteralPath $stderrPath -Raw } else { '' }

        if ($process.ExitCode -ne 0) {
            $message = (($stdout + "`n" + $stderr).Trim())
            if ([string]::IsNullOrWhiteSpace($message)) {
                $message = 'no output'
            }
            throw "adb failed: $($Arguments -join ' ')`n$message"
        }

        if ($SuppressOutput) {
            return $null
        }

        $combined = ($stdout + $stderr).TrimEnd("`r", "`n")
        if (-not [string]::IsNullOrWhiteSpace($combined)) {
            return $combined
        }

        return $null
    } finally {
        foreach ($path in @($stdoutPath, $stderrPath)) {
            if (Test-Path -LiteralPath $path) {
                Remove-Item -LiteralPath $path -Force
            }
        }
    }
}

function Assert-AdbReady {
    $state = & $script:ResolvedAdbPath get-state 2>$null
    if ($LASTEXITCODE -ne 0 -or $state -notmatch 'device') {
        throw "No adb device is ready. Resolved adb path: $script:ResolvedAdbPath"
    }
}

function Get-UnityCaptureMode {
    param(
        [Parameter(Mandatory = $true)]
        [string]$DisplayModeValue
    )

    switch ($DisplayModeValue) {
        'projection_stereo' { return 'stereo_projection' }
        default { return 'mono' }
    }
}

function Write-UnitySavedEndpoint {
    param(
        [string]$Path,
        [string]$TargetHost,
        [UInt16]$VideoPort,
        [UInt16]$PosePort,
        [string]$DisplayModeValue,
        [switch]$SkipReset
    )

    if ($SkipReset) {
        return 'skipped'
    }

    $directory = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($directory) -and -not (Test-Path -LiteralPath $directory)) {
        $null = New-Item -ItemType Directory -Path $directory -Force
    }

    $captureViewMode = Get-UnityCaptureMode -DisplayModeValue $DisplayModeValue
    $record = [ordered]@{
        targetHost      = $TargetHost
        videoPort       = [int]$VideoPort
        posePort        = [int]$PosePort
        captureViewMode = $captureViewMode
    }

    $json = $record | ConvertTo-Json
    Set-Content -LiteralPath $Path -Value $json -Encoding UTF8
    return "written mode=$captureViewMode"
}

function Write-HeadsetRuntimeConfig {
    param(
        [string]$TargetHost,
        [string]$DisplayModeValue,
        [UInt16]$PosePort,
        [UInt16]$ControlPort,
        [UInt16]$RawVideoPort,
        [UInt16]$EncodedPort
    )

    $contentLines = @(
        '# Saved after first successful decoded frame'
        "target_host=$TargetHost"
    )

    if ($PosePort -eq $ControlPort) {
        $contentLines += "target_port=$PosePort"
    }

    $contentLines += @(
        "pose_target_port=$PosePort"
        "control_target_port=$ControlPort"
        "video_port=$RawVideoPort"
        "encoded_video_port=$EncodedPort"
        "display_mode=$DisplayModeValue"
    )

    $tempFile = Join-Path ([System.IO.Path]::GetTempPath()) 'videotest_runtime_config.txt'
    Set-Content -LiteralPath $tempFile -Value ($contentLines -join "`n")

    $null = Invoke-Adb -Arguments @('push', $tempFile, '/data/local/tmp/videotest_runtime_config.txt') -SuppressOutput
    $null = Invoke-Adb -Arguments @('shell', "run-as $PackageName cp /data/local/tmp/videotest_runtime_config.txt files/last_successful_runtime_config.txt") -SuppressOutput

    Remove-Item -LiteralPath $tempFile -Force
}

function Restart-HeadsetApp {
    param(
        [string]$TargetHost,
        [string]$DisplayModeValue,
        [UInt16]$PosePort,
        [UInt16]$ControlPort,
        [UInt16]$RawVideoPort,
        [UInt16]$EncodedPort
    )

    $null = Invoke-Adb -Arguments @('shell', 'am', 'force-stop', $PackageName) -SuppressOutput
    Start-Sleep -Seconds 1
    $null = Invoke-Adb -Arguments @(
            'shell',
            'am',
            'start',
            '-n',
            "$PackageName/$ActivityName",
            '--es', 'target_host', $TargetHost,
            '--es', 'display_mode', $DisplayModeValue,
            '--ei', 'target_port', "$PosePort",
            '--ei', 'pose_target_port', "$PosePort",
            '--ei', 'control_target_port', "$ControlPort",
            '--ei', 'video_port', "$RawVideoPort",
            '--ei', 'encoded_video_port', "$EncodedPort"
        ) -SuppressOutput
}

if (-not (Test-Path -LiteralPath $AdbPath)) {
    $script:ResolvedAdbPath = 'adb'
} else {
    $script:ResolvedAdbPath = $AdbPath
}

$ResolvedPoseTargetPort = if ($PoseTargetPort -ne 0) {
    $PoseTargetPort
} elseif ($TargetPort -ne 0) {
    $TargetPort
} else {
    [UInt16]25672
}

$ResolvedControlTargetPort = if ($ControlTargetPort -ne 0) {
    $ControlTargetPort
} elseif ($TargetPort -ne 0) {
    $TargetPort
} else {
    [UInt16]25672
}

$ResolvedPcIp = Resolve-PcIp -ExplicitIp $PcIp
Assert-AdbReady

$unityCacheState = Write-UnitySavedEndpoint `
    -Path $UnitySavedEndpointPath `
    -TargetHost 'auto' `
    -VideoPort $EncodedVideoPort `
    -PosePort $ResolvedPoseTargetPort `
    -DisplayModeValue $DisplayMode `
    -SkipReset:$SkipUnityCacheReset
Write-HeadsetRuntimeConfig `
    -TargetHost $ResolvedPcIp `
    -DisplayModeValue $DisplayMode `
    -PosePort $ResolvedPoseTargetPort `
    -ControlPort $ResolvedControlTargetPort `
    -RawVideoPort $VideoPort `
    -EncodedPort $EncodedVideoPort
Restart-HeadsetApp `
    -TargetHost $ResolvedPcIp `
    -DisplayModeValue $DisplayMode `
    -PosePort $ResolvedPoseTargetPort `
    -ControlPort $ResolvedControlTargetPort `
    -RawVideoPort $VideoPort `
    -EncodedPort $EncodedVideoPort

Write-Output "PC IPv4: $ResolvedPcIp"
Write-Output "adb: $script:ResolvedAdbPath"
Write-Output "Unity runtime config: $unityCacheState ($UnitySavedEndpointPath)"
Write-Output "Pose target port: $ResolvedPoseTargetPort"
Write-Output "Control target port: $ResolvedControlTargetPort"
Write-Output "Display mode: $DisplayMode"
Write-Output "Headset runtime config:"
Invoke-Adb @('shell', "run-as $PackageName cat files/last_successful_runtime_config.txt")
Write-Output 'Headset process:'
Invoke-Adb @('shell', 'pidof', $PackageName)
