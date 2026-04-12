using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Rendering;

namespace VideoTest.UnityIntegration
{
    [DisallowMultipleComponent]
    [RequireComponent(typeof(Camera))]
    public sealed class UnitySenderController : MonoBehaviour
    {
        [Header("Network")]
        [SerializeField] private string targetHost = "127.0.0.1";
        [SerializeField] private int videoPort = 25674;
        [SerializeField] private int posePort = 25672;

        [Header("Encoding")]
        [SerializeField] private int encodeWidth = 1280;
        [SerializeField] private int encodeHeight = 720;
        [SerializeField] private int fps = 15;
        [SerializeField] private int bitrate = 4000000;

        [Header("Runtime")]
        [SerializeField] private bool startOnPlay = true;
        [SerializeField] private bool applyPoseToCamera = true;
        [SerializeField] private KeyCode recenterKey = KeyCode.R;
        [SerializeField] private bool showLocalPreview = true;
        [SerializeField] private float previewCameraDepthOffset = 1.0f;
        [SerializeField] private bool showDebugOverlay = true;
        [SerializeField] private KeyCode toggleDebugOverlayKey = KeyCode.F1;
        [SerializeField] private bool allowCommandLineOverrides = true;
        [SerializeField] private bool useSavedEndpoint = true;
        [SerializeField] private bool rememberSuccessfulEndpoint = true;
        [SerializeField] private float statsPollIntervalSeconds = 0.5f;
        [SerializeField] private bool retryAutoStartWhenUsingAutoHost = true;
        [SerializeField] private float autoStartRetryIntervalSeconds = 1.0f;

        private Camera captureCamera;
        private Camera previewCamera;
        private RenderTexture captureTexture;
        private IntPtr renderEventFunc = IntPtr.Zero;
        private int renderEventId = -1;
        private bool streaming;
        private bool originInitialized;
        private Vector3 originPosition;
        private Quaternion originRotation;
        private bool savedEndpointThisSession;
        private float nextPersistencePollTime;
        private float nextDiagnosticsPollTime;
        private float nextAutoStartRetryTime;
        private bool autoStartRetryPending;
        private string configSourceSummary = "scene_defaults";
        private bool debugOverlayVisible = true;
        private bool hasCachedStats;
        private UnitySenderPluginBindings.UnitySenderStats cachedStats;
        private string cachedPoseSenderHost = string.Empty;
        private string cachedActiveTargetHost = string.Empty;
        private string lastLifecycleMessage = "Idle";

        private const string PreviewCameraName = "UnitySenderPreviewCamera";

        private void Awake()
        {
            captureCamera = GetComponent<Camera>();
            debugOverlayVisible = showDebugOverlay;
            ResolveRuntimeConfiguration();
            EnsureLocalPreviewCamera();
        }

        private void Start()
        {
            if (!Application.isPlaying || !startOnPlay)
            {
                return;
            }

            StartStreaming();
        }

        private void Update()
        {
            if (!streaming)
            {
                TryAutoStartRetry();
                UpdateRuntimeDiagnostics();
                return;
            }

            if (Input.GetKeyDown(toggleDebugOverlayKey))
            {
                debugOverlayVisible = !debugOverlayVisible;
            }

            if (applyPoseToCamera && UnitySenderPluginBindings.UnitySender_GetLatestPose(out var pose) && pose.hasPose != 0)
            {
                ApplyPoseToCamera(pose);
            }

            SyncLocalPreviewCamera();
            UpdateRuntimeDiagnostics();
            PollForSuccessfulEndpoint();

            if (Input.GetKeyDown(recenterKey))
            {
                RecenterFromCurrentPose();
            }
        }

        private void OnDisable()
        {
            StopStreaming();
        }

        private void OnDestroy()
        {
            StopStreaming();
        }

        private void OnPostRender()
        {
            if (!streaming || renderEventFunc == IntPtr.Zero || renderEventId < 0)
            {
                return;
            }

            GL.IssuePluginEvent(renderEventFunc, renderEventId);
        }

        [ContextMenu("Start Streaming")]
        public void StartStreaming()
        {
            if (streaming)
            {
                return;
            }

            if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D11)
            {
                Debug.LogError($"UnitySenderController requires D3D11. Current graphics API: {SystemInfo.graphicsDeviceType}");
                return;
            }

            EnsureCaptureTexture();
            EnsureLocalPreviewCamera();
            SyncLocalPreviewCamera();

            renderEventFunc = UnitySenderPluginBindings.UnitySender_GetRenderEventFunc();
            renderEventId = UnitySenderPluginBindings.UnitySender_GetCopyTextureEventId();
            if (renderEventFunc == IntPtr.Zero || renderEventId < 0)
            {
                Debug.LogError("UnitySenderController failed to get the native render-event callback.");
                return;
            }

            var texturePtr = captureTexture.GetNativeTexturePtr();
            if (texturePtr == IntPtr.Zero)
            {
                Debug.LogError("UnitySenderController received an invalid native texture pointer.");
                return;
            }

            UnitySenderPluginBindings.UnitySender_SetTexture(texturePtr);
            var configured = UnitySenderPluginBindings.UnitySender_Configure(
                targetHost,
                ClampPort(videoPort),
                ClampPort(posePort),
                Mathf.Max(fps, 1),
                (uint)Mathf.Max(bitrate, 1),
                (ushort)Mathf.Max(encodeWidth, 1),
                (ushort)Mathf.Max(encodeHeight, 1));
            if (!configured)
            {
                Debug.LogError("UnitySenderController failed to configure the native sender plugin.");
                return;
            }

            if (!UnitySenderPluginBindings.UnitySender_Start())
            {
                HandleStartFailure();
                return;
            }

            Application.runInBackground = true;
            originInitialized = false;
            savedEndpointThisSession = false;
            nextPersistencePollTime = Time.unscaledTime;
            nextDiagnosticsPollTime = Time.unscaledTime;
            autoStartRetryPending = false;
            streaming = true;

            var activeTargetHost = targetHost;
            if (IsAutoTargetHost() && UnitySenderPluginBindings.TryGetLastPoseSenderIpv4(out var detectedHost))
            {
                activeTargetHost = detectedHost;
            }
            cachedActiveTargetHost = activeTargetHost;
            lastLifecycleMessage = $"Streaming to {activeTargetHost}:{videoPort}";
            Debug.Log(
                $"UnitySenderController started. source={configSourceSummary} " +
                $"target={activeTargetHost}:{videoPort} posePort={posePort} size={encodeWidth}x{encodeHeight} fps={fps} bitrate={bitrate}");
        }

        [ContextMenu("Stop Streaming")]
        public void StopStreaming()
        {
            if (streaming)
            {
                UnitySenderPluginBindings.UnitySender_Stop();
                streaming = false;
            }

            if (captureCamera != null)
            {
                captureCamera.targetTexture = null;
            }

            if (captureTexture != null)
            {
                captureTexture.Release();
                Destroy(captureTexture);
                captureTexture = null;
            }

            renderEventFunc = IntPtr.Zero;
            renderEventId = -1;
            autoStartRetryPending = false;
            lastLifecycleMessage = "Stopped";
        }

        [ContextMenu("Recenter From Current Pose")]
        public void RecenterFromCurrentPose()
        {
            if (!UnitySenderPluginBindings.UnitySender_GetLatestPose(out var pose) || pose.hasPose == 0)
            {
                Debug.LogWarning("UnitySenderController recenter skipped because no pose has been received yet.");
                return;
            }

            var convertedPosition = ConvertOpenXrPositionToUnity(pose);
            var convertedRotation = ConvertOpenXrRotationToUnity(pose);
            originPosition = transform.localPosition - convertedPosition;
            originRotation = transform.localRotation * Quaternion.Inverse(convertedRotation);
            originInitialized = true;
            Debug.Log("UnitySenderController recentered to current pose.");
        }

        private void EnsureCaptureTexture()
        {
            if (captureTexture != null &&
                captureTexture.width == encodeWidth &&
                captureTexture.height == encodeHeight)
            {
                captureCamera.targetTexture = captureTexture;
                return;
            }

            if (captureTexture != null)
            {
                captureTexture.Release();
                Destroy(captureTexture);
            }

            var captureFormat = SystemInfo.SupportsRenderTextureFormat(RenderTextureFormat.BGRA32)
                ? RenderTextureFormat.BGRA32
                : RenderTextureFormat.ARGB32;
            if (captureFormat != RenderTextureFormat.BGRA32)
            {
                Debug.LogWarning(
                    "UnitySenderController could not allocate a BGRA32 RenderTexture. " +
                    "The native NVENC path expects BGRA input, so streaming may fail until a conversion path is added.");
            }

            captureTexture = new RenderTexture(
                encodeWidth,
                encodeHeight,
                24,
                captureFormat,
                RenderTextureReadWrite.Linear)
            {
                name = "UnitySenderCaptureTexture",
                useMipMap = false,
                autoGenerateMips = false,
                antiAliasing = 1
            };
            captureTexture.Create();
            captureCamera.targetTexture = captureTexture;
        }

        private void EnsureLocalPreviewCamera()
        {
            if (!showLocalPreview)
            {
                if (previewCamera != null)
                {
                    previewCamera.enabled = false;
                }
                return;
            }

            if (previewCamera == null)
            {
                var existingTransform = transform.Find(PreviewCameraName);
                if (existingTransform != null)
                {
                    previewCamera = existingTransform.GetComponent<Camera>();
                }
            }

            if (previewCamera == null)
            {
                var previewObject = new GameObject(PreviewCameraName);
                previewObject.transform.SetParent(transform, false);
                previewCamera = previewObject.AddComponent<Camera>();
                Debug.Log("UnitySenderController created a local preview camera for the desktop window.");
            }

            previewCamera.enabled = true;
            SyncLocalPreviewCamera();
        }

        private void SyncLocalPreviewCamera()
        {
            if (!showLocalPreview || previewCamera == null || captureCamera == null)
            {
                return;
            }

            previewCamera.CopyFrom(captureCamera);
            previewCamera.targetTexture = null;
            previewCamera.depth = captureCamera.depth + previewCameraDepthOffset;
            previewCamera.transform.localPosition = Vector3.zero;
            previewCamera.transform.localRotation = Quaternion.identity;
            previewCamera.transform.localScale = Vector3.one;
        }

        private void HandleStartFailure()
        {
            if (IsAutoTargetHost() && startOnPlay && retryAutoStartWhenUsingAutoHost)
            {
                autoStartRetryPending = true;
                nextAutoStartRetryTime = Time.unscaledTime + Mathf.Max(autoStartRetryIntervalSeconds, 0.1f);
                lastLifecycleMessage = "Waiting for pose source to auto-learn target host";
                Debug.LogWarning(
                    $"UnitySenderController deferred startup because targetHost={targetHost} and no pose source was ready yet. " +
                    $"Retrying in {autoStartRetryIntervalSeconds:0.0}s.");
                return;
            }

            lastLifecycleMessage = "Native sender start failed";
            Debug.LogError("UnitySenderController failed to start the native sender plugin.");
        }

        private void TryAutoStartRetry()
        {
            if (!autoStartRetryPending || Time.unscaledTime < nextAutoStartRetryTime)
            {
                return;
            }

            nextAutoStartRetryTime = Time.unscaledTime + Mathf.Max(autoStartRetryIntervalSeconds, 0.1f);
            StartStreaming();
        }

        private void ResolveRuntimeConfiguration()
        {
            var sourceParts = new List<string> { "scene_defaults" };
            var launchOptions = allowCommandLineOverrides
                ? UnitySenderLaunchOptions.Parse(Environment.GetCommandLineArgs())
                : default;

            if (launchOptions.resetSavedEndpoint && UnitySenderRuntimeEndpointStore.DeleteSavedEndpoint())
            {
                sourceParts.Add("reset_saved_endpoint");
            }

            if (useSavedEndpoint &&
                !launchOptions.ignoreSavedEndpoint &&
                UnitySenderRuntimeEndpointStore.TryLoadSavedEndpoint(out var savedEndpoint))
            {
                targetHost = savedEndpoint.targetHost;
                videoPort = savedEndpoint.videoPort;
                posePort = savedEndpoint.posePort;
                sourceParts.Add("saved_endpoint");
            }

            if (allowCommandLineOverrides && launchOptions.HasAnyOverrides)
            {
                ApplyLaunchOptions(launchOptions);
                sourceParts.Add("command_line");
            }

            targetHost = string.IsNullOrWhiteSpace(targetHost) ? "auto" : targetHost.Trim();
            videoPort = Mathf.Clamp(videoPort, 1, 65535);
            posePort = Mathf.Clamp(posePort, 1, 65535);
            encodeWidth = Mathf.Max(encodeWidth, 1);
            encodeHeight = Mathf.Max(encodeHeight, 1);
            fps = Mathf.Max(fps, 1);
            bitrate = Mathf.Max(bitrate, 1);
            statsPollIntervalSeconds = Mathf.Max(statsPollIntervalSeconds, 0.1f);
            autoStartRetryIntervalSeconds = Mathf.Max(autoStartRetryIntervalSeconds, 0.1f);

            configSourceSummary = string.Join("+", sourceParts);
            cachedActiveTargetHost = targetHost;
            lastLifecycleMessage = "Configured";
            Debug.Log(
                $"UnitySenderController config resolved. source={configSourceSummary} " +
                $"target={targetHost}:{videoPort} posePort={posePort} size={encodeWidth}x{encodeHeight} fps={fps} bitrate={bitrate} " +
                $"savedEndpointPath={UnitySenderRuntimeEndpointStore.GetConfigPath()}");
        }

        private void ApplyLaunchOptions(UnitySenderLaunchOptions launchOptions)
        {
            if (!string.IsNullOrWhiteSpace(launchOptions.targetHost))
            {
                targetHost = launchOptions.targetHost.Trim();
            }
            if (launchOptions.videoPort.HasValue)
            {
                videoPort = launchOptions.videoPort.Value;
            }
            if (launchOptions.posePort.HasValue)
            {
                posePort = launchOptions.posePort.Value;
            }
            if (launchOptions.encodeWidth.HasValue)
            {
                encodeWidth = launchOptions.encodeWidth.Value;
            }
            if (launchOptions.encodeHeight.HasValue)
            {
                encodeHeight = launchOptions.encodeHeight.Value;
            }
            if (launchOptions.fps.HasValue)
            {
                fps = launchOptions.fps.Value;
            }
            if (launchOptions.bitrate.HasValue)
            {
                bitrate = launchOptions.bitrate.Value;
            }
            if (launchOptions.disableAutoStart)
            {
                startOnPlay = false;
            }
        }

        private void PollForSuccessfulEndpoint()
        {
            if (!streaming || !rememberSuccessfulEndpoint || savedEndpointThisSession || Time.unscaledTime < nextPersistencePollTime)
            {
                return;
            }

            nextPersistencePollTime = Time.unscaledTime + statsPollIntervalSeconds;
            if (!UnitySenderPluginBindings.UnitySender_GetStats(out var stats))
            {
                return;
            }

            if (stats.posePacketsReceived == 0)
            {
                return;
            }

            if (!UnitySenderPluginBindings.TryGetLastPoseSenderIpv4(out var detectedHost))
            {
                return;
            }

            cachedPoseSenderHost = detectedHost;
            if (!UnitySenderRuntimeEndpointStore.SaveSavedEndpoint(detectedHost, ClampPort(videoPort), ClampPort(posePort)))
            {
                return;
            }

            savedEndpointThisSession = true;
            Debug.Log(
                $"UnitySenderController saved last known good endpoint from pose source. " +
                $"configuredTarget={targetHost}:{videoPort} learnedTarget={detectedHost}:{videoPort} posePort={posePort} " +
                $"path={UnitySenderRuntimeEndpointStore.GetConfigPath()}");
        }

        private void UpdateRuntimeDiagnostics()
        {
            if (Time.unscaledTime < nextDiagnosticsPollTime && hasCachedStats)
            {
                return;
            }

            nextDiagnosticsPollTime = Time.unscaledTime + statsPollIntervalSeconds;
            if (UnitySenderPluginBindings.UnitySender_GetStats(out cachedStats))
            {
                hasCachedStats = true;
            }

            if (UnitySenderPluginBindings.TryGetLastPoseSenderIpv4(out var detectedHost))
            {
                cachedPoseSenderHost = detectedHost;
                if (IsAutoTargetHost())
                {
                    cachedActiveTargetHost = detectedHost;
                }
            }
            else if (string.IsNullOrEmpty(cachedActiveTargetHost))
            {
                cachedActiveTargetHost = targetHost;
            }
        }

        private void OnGUI()
        {
            if (!Application.isPlaying || !showDebugOverlay || !debugOverlayVisible)
            {
                return;
            }

            const float width = 420.0f;
            const float padding = 12.0f;
            GUILayout.BeginArea(new Rect(padding, padding, width, 260.0f), GUI.skin.box);
            GUILayout.Label("Unity Sender Debug");
            GUILayout.Label($"Lifecycle: {lastLifecycleMessage}");
            GUILayout.Label($"Source: {configSourceSummary}");
            GUILayout.Label($"Configured target: {targetHost}:{videoPort}");
            GUILayout.Label($"Active target: {cachedActiveTargetHost}:{videoPort}");
            GUILayout.Label($"Pose port: {posePort}");
            GUILayout.Label($"Streaming: {streaming}  AutoRetry: {autoStartRetryPending}");
            GUILayout.Label($"Render event: {(renderEventFunc != IntPtr.Zero ? "ready" : "null")} / id={renderEventId}");
            GUILayout.Label($"Capture texture: {(captureTexture != null ? $"{captureTexture.width}x{captureTexture.height}" : "null")}");
            GUILayout.Label($"Last pose sender: {(string.IsNullOrEmpty(cachedPoseSenderHost) ? "<none>" : cachedPoseSenderHost)}");

            if (hasCachedStats)
            {
                GUILayout.Space(6.0f);
                GUILayout.Label($"Device ready: {cachedStats.unityDeviceReady != 0}");
                GUILayout.Label($"Source texture ready: {cachedStats.sourceTextureReady != 0}");
                GUILayout.Label($"Copied frame ready: {cachedStats.copiedFrameReady != 0}");
                GUILayout.Label($"Sender thread: {cachedStats.senderThreadRunning != 0}  Network thread: {cachedStats.networkThreadRunning != 0}");
                GUILayout.Label($"Source size: {cachedStats.sourceWidth}x{cachedStats.sourceHeight}");
                GUILayout.Label($"Render copies: {cachedStats.renderThreadCopyCount}");
                GUILayout.Label($"Pose packets: {cachedStats.posePacketsReceived}  Last pose seq: {cachedStats.lastPoseSequence}");
                GUILayout.Label($"Control packets: {cachedStats.controlPacketsReceived}");
            }
            else
            {
                GUILayout.Space(6.0f);
                GUILayout.Label("Stats: <unavailable>");
            }

            GUILayout.Label($"Toggle overlay: {toggleDebugOverlayKey}");
            GUILayout.EndArea();
        }

        private void ApplyPoseToCamera(UnitySenderPluginBindings.UnitySenderPose pose)
        {
            var convertedPosition = ConvertOpenXrPositionToUnity(pose);
            var convertedRotation = ConvertOpenXrRotationToUnity(pose);

            if (!originInitialized)
            {
                originPosition = transform.localPosition - convertedPosition;
                originRotation = transform.localRotation * Quaternion.Inverse(convertedRotation);
                originInitialized = true;
            }

            transform.localPosition = originPosition + convertedPosition;
            transform.localRotation = originRotation * convertedRotation;
        }

        // OpenXR pose data uses a right-handed basis with +Y up and -Z forward.
        // Unity uses a left-handed basis with +Y up and +Z forward. Mirroring Z
        // converts positions into Unity space.
        private static Vector3 ConvertOpenXrPositionToUnity(UnitySenderPluginBindings.UnitySenderPose pose)
        {
            return new Vector3(pose.positionX, pose.positionY, -pose.positionZ);
        }

        // Mirroring the Z basis converts the OpenXR quaternion into Unity space.
        // For a basis mirror S = diag(1, 1, -1), the equivalent quaternion mapping is:
        // q_unity = (-x, -y, z, w)
        private static Quaternion ConvertOpenXrRotationToUnity(UnitySenderPluginBindings.UnitySenderPose pose)
        {
            return new Quaternion(-pose.orientationX, -pose.orientationY, pose.orientationZ, pose.orientationW);
        }

        private static ushort ClampPort(int value)
        {
            return (ushort)Mathf.Clamp(value, 1, 65535);
        }

        private bool IsAutoTargetHost()
        {
            return string.IsNullOrWhiteSpace(targetHost) ||
                   string.Equals(targetHost, "auto", StringComparison.OrdinalIgnoreCase);
        }
    }
}
