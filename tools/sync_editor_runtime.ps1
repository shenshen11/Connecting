[CmdletBinding()]
param(
    [string]$PcIp,
    [string]$AdbPath = 'D:\platform-tools-latest-windows\platform-tools\adb.exe',
    [string]$PackageName = 'com.videotest.nativeapp',
    [string]$ActivityName = 'android.app.NativeActivity',
    [UInt16]$TargetPort = 25672,
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

function Reset-UnitySavedEndpoint {
    param(
        [string]$Path,
        [switch]$SkipReset
    )

    if ($SkipReset) {
        return 'skipped'
    }

    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Force
        return 'deleted'
    }

    return 'missing'
}

function Write-HeadsetRuntimeConfig {
    param(
        [string]$TargetHost,
        [UInt16]$PosePort,
        [UInt16]$RawVideoPort,
        [UInt16]$EncodedPort
    )

    $content = @"
# Saved after first successful decoded frame
target_host=$TargetHost
target_port=$PosePort
video_port=$RawVideoPort
encoded_video_port=$EncodedPort
"@

    $tempFile = Join-Path ([System.IO.Path]::GetTempPath()) 'videotest_runtime_config.txt'
    Set-Content -LiteralPath $tempFile -Value $content

    $null = Invoke-Adb -Arguments @('push', $tempFile, '/data/local/tmp/videotest_runtime_config.txt') -SuppressOutput
    $null = Invoke-Adb -Arguments @('shell', "run-as $PackageName cp /data/local/tmp/videotest_runtime_config.txt files/last_successful_runtime_config.txt") -SuppressOutput

    Remove-Item -LiteralPath $tempFile -Force
}

function Restart-HeadsetApp {
    param(
        [string]$TargetHost,
        [UInt16]$PosePort,
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
            '--ei', 'target_port', "$PosePort",
            '--ei', 'video_port', "$RawVideoPort",
            '--ei', 'encoded_video_port', "$EncodedPort"
        ) -SuppressOutput
}

if (-not (Test-Path -LiteralPath $AdbPath)) {
    $script:ResolvedAdbPath = 'adb'
} else {
    $script:ResolvedAdbPath = $AdbPath
}

$ResolvedPcIp = Resolve-PcIp -ExplicitIp $PcIp
Assert-AdbReady

$unityCacheState = Reset-UnitySavedEndpoint -Path $UnitySavedEndpointPath -SkipReset:$SkipUnityCacheReset
Write-HeadsetRuntimeConfig -TargetHost $ResolvedPcIp -PosePort $TargetPort -RawVideoPort $VideoPort -EncodedPort $EncodedVideoPort
Restart-HeadsetApp -TargetHost $ResolvedPcIp -PosePort $TargetPort -RawVideoPort $VideoPort -EncodedPort $EncodedVideoPort

Write-Output "PC IPv4: $ResolvedPcIp"
Write-Output "adb: $script:ResolvedAdbPath"
Write-Output "Unity saved endpoint: $unityCacheState ($UnitySavedEndpointPath)"
Write-Output "Headset runtime config:"
Invoke-Adb @('shell', "run-as $PackageName cat files/last_successful_runtime_config.txt")
Write-Output 'Headset process:'
Invoke-Adb @('shell', 'pidof', $PackageName)
