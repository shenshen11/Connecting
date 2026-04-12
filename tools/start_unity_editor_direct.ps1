[CmdletBinding()]
param(
    [string]$PcIp,
    [string]$HeadsetIp,
    [string]$AdbPath = 'D:\platform-tools-latest-windows\platform-tools\adb.exe',
    [switch]$SkipRuntimeSync,
    [switch]$SkipSenderLaunch,
    [switch]$DryRun
)

$launcher = Join-Path $PSScriptRoot 'start_stream_profile.ps1'
& $launcher `
    -Profile 'unity_editor_direct' `
    -PcIp $PcIp `
    -HeadsetIp $HeadsetIp `
    -AdbPath $AdbPath `
    -SkipRuntimeSync:$SkipRuntimeSync `
    -SkipSenderLaunch:$SkipSenderLaunch `
    -DryRun:$DryRun

exit $LASTEXITCODE
