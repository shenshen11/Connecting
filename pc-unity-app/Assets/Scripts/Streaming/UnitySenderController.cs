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
        private enum CaptureViewMode
        {
            Mono = 0,
            StereoProjection = 1,
        }

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
        [SerializeField] private CaptureViewMode captureViewMode = CaptureViewMode.Mono;
        [SerializeField] private float stereoEyeSeparationMeters = 0.064f;
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
        private Camera leftEyeCaptureCamera;
        private Camera rightEyeCaptureCamera;
        private RenderTexture captureTexture;
        private RenderTexture leftCaptureTexture;
        private RenderTexture rightCaptureTexture;
        private RenderTexture bridgeCaptureTexture;
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
        private const string LeftEyeCameraName = "UnitySenderLeftEyeCamera";
        private const string RightEyeCameraName = "UnitySenderRightEyeCamera";

        private bool IsStereoProjectionCaptureMode()
        {
            return captureViewMode == CaptureViewMode.StereoProjection;
        }

        private static string CaptureViewModeConfigName(CaptureViewMode mode)
        {
            return mode == CaptureViewMode.StereoProjection ? "stereo_projection" : "mono";
        }

        private static bool TryParseCaptureViewMode(string rawMode, out CaptureViewMode mode)
        {
            mode = CaptureViewMode.Mono;
            if (string.IsNullOrWhiteSpace(rawMode))
            {
                return false;
            }

            switch (rawMode.Trim().ToLowerInvariant())
            {
                case "mono":
                case "quad_mono":
                case "projection_mono":
                    mode = CaptureViewMode.Mono;
                    return true;

                case "stereo_projection":
                case "projection_stereo":
                    mode = CaptureViewMode.StereoProjection;
                    return true;

                default:
                    return false;
            }
        }

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

            if (IsStereoProjectionCaptureMode())
            {
                EnsureStereoCaptureSetup();
            }
            else
            {
                ReleaseStereoCaptureResources();
                EnsureCaptureTexture();
            }
            EnsureLocalPreviewCamera();
            SyncLocalPreviewCamera();

            renderEventFunc = UnitySenderPluginBindings.UnitySender_GetRenderEventFunc();
            renderEventId = UnitySenderPluginBindings.UnitySender_GetCopyTextureEventId();
            if (renderEventFunc == IntPtr.Zero || renderEventId < 0)
            {
                Debug.LogError("UnitySenderController failed to get the native render-event callback.");
                return;
            }

            var primaryTexturePtr = IntPtr.Zero;
            var secondaryTexturePtr = IntPtr.Zero;
            if (IsStereoProjectionCaptureMode())
            {
                primaryTexturePtr = leftCaptureTexture != null ? leftCaptureTexture.GetNativeTexturePtr() : IntPtr.Zero;
                secondaryTexturePtr = rightCaptureTexture != null ? rightCaptureTexture.GetNativeTexturePtr() : IntPtr.Zero;
                if (primaryTexturePtr == IntPtr.Zero || secondaryTexturePtr == IntPtr.Zero)
                {
                    Debug.LogError("UnitySenderController received invalid stereo texture pointers.");
                    return;
                }

                UnitySenderPluginBindings.UnitySender_SetTextureForView(0, primaryTexturePtr);
                UnitySenderPluginBindings.UnitySender_SetTextureForView(1, secondaryTexturePtr);
            }
            else
            {
                primaryTexturePtr = captureTexture != null ? captureTexture.GetNativeTexturePtr() : IntPtr.Zero;
                if (primaryTexturePtr == IntPtr.Zero)
                {
                    Debug.LogError("UnitySenderController received an invalid mono texture pointer.");
                    return;
                }

                UnitySenderPluginBindings.UnitySender_SetTexture(primaryTexturePtr);
                UnitySenderPluginBindings.UnitySender_SetTextureForView(1, IntPtr.Zero);
            }
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
            lastLifecycleMessage = $"Streaming {captureViewMode} to {activeTargetHost}:{videoPort}";
            Debug.Log(
                $"UnitySenderController started. source={configSourceSummary} " +
                $"target={activeTargetHost}:{videoPort} posePort={posePort} size={encodeWidth}x{encodeHeight} fps={fps} bitrate={bitrate} mode={captureViewMode}");
        }

        [ContextMenu("Stop Streaming")]
        public void StopStreaming()
        {
            if (streaming)
            {
                UnitySenderPluginBindings.UnitySender_Stop();
                streaming = false;
            }

            UnitySenderPluginBindings.UnitySender_SetTextureForView(0, IntPtr.Zero);
            UnitySenderPluginBindings.UnitySender_SetTextureForView(1, IntPtr.Zero);

            if (captureCamera != null)
            {
                captureCamera.targetTexture = null;
            }

            ReleaseMonoCaptureResources();
            ReleaseStereoCaptureResources();

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
            EnsureRenderTexture(ref captureTexture, "UnitySenderCaptureTexture", encodeWidth, encodeHeight, 24);
            captureCamera.targetTexture = captureTexture;
        }

        private void EnsureStereoCaptureSetup()
        {
            EnsureRenderTexture(ref leftCaptureTexture, "UnitySenderLeftEyeCaptureTexture", encodeWidth, encodeHeight, 24);
            EnsureRenderTexture(ref rightCaptureTexture, "UnitySenderRightEyeCaptureTexture", encodeWidth, encodeHeight, 24);

            if (showLocalPreview)
            {
                ReleaseRenderTexture(ref bridgeCaptureTexture);
            }
            else
            {
                EnsureRenderTexture(ref bridgeCaptureTexture, "UnitySenderBridgeCaptureTexture", 1, 1, 0);
            }

            leftEyeCaptureCamera = GetOrCreateChildCamera(leftEyeCaptureCamera, LeftEyeCameraName);
            rightEyeCaptureCamera = GetOrCreateChildCamera(rightEyeCaptureCamera, RightEyeCameraName);
            SyncStereoCaptureCameras();
        }

        private void EnsureLocalPreviewCamera()
        {
            if (IsStereoProjectionCaptureMode())
            {
                if (previewCamera != null)
                {
                    previewCamera.enabled = false;
                }

                captureCamera.enabled = true;
                return;
            }

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
            if (IsStereoProjectionCaptureMode())
            {
                SyncStereoCaptureCameras();
                if (captureCamera == null)
                {
                    return;
                }

                captureCamera.enabled = true;
                captureCamera.targetTexture = showLocalPreview ? null : bridgeCaptureTexture;
                if (previewCamera != null)
                {
                    previewCamera.enabled = false;
                }
                return;
            }

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

        private void SyncStereoCaptureCameras()
        {
            if (!IsStereoProjectionCaptureMode() || captureCamera == null)
            {
                return;
            }

            ConfigureStereoEyeCamera(leftEyeCaptureCamera, leftCaptureTexture, -0.5f * stereoEyeSeparationMeters, captureCamera.depth - 2.0f);
            ConfigureStereoEyeCamera(rightEyeCaptureCamera, rightCaptureTexture, 0.5f * stereoEyeSeparationMeters, captureCamera.depth - 1.0f);
        }

        private void ConfigureStereoEyeCamera(Camera eyeCamera, RenderTexture targetTexture, float horizontalOffset, float depth)
        {
            if (eyeCamera == null || targetTexture == null || captureCamera == null)
            {
                return;
            }

            eyeCamera.CopyFrom(captureCamera);
            eyeCamera.targetTexture = targetTexture;
            eyeCamera.depth = depth;
            eyeCamera.stereoTargetEye = StereoTargetEyeMask.None;
            eyeCamera.enabled = true;
            eyeCamera.transform.localPosition = new Vector3(horizontalOffset, 0.0f, 0.0f);
            eyeCamera.transform.localRotation = Quaternion.identity;
            eyeCamera.transform.localScale = Vector3.one;
        }

        private Camera GetOrCreateChildCamera(Camera existingCamera, string childName)
        {
            if (existingCamera == null)
            {
                var existingTransform = transform.Find(childName);
                if (existingTransform != null)
                {
                    existingCamera = existingTransform.GetComponent<Camera>();
                }
            }

            if (existingCamera != null)
            {
                return existingCamera;
            }

            var cameraObject = new GameObject(childName);
            cameraObject.transform.SetParent(transform, false);
            return cameraObject.AddComponent<Camera>();
        }

        private void EnsureRenderTexture(ref RenderTexture texture, string textureName, int width, int height, int depthBufferBits)
        {
            if (texture != null &&
                texture.width == width &&
                texture.height == height)
            {
                return;
            }

            ReleaseRenderTexture(ref texture);
            var captureFormat = GetCaptureTextureFormat();
            texture = new RenderTexture(
                width,
                height,
                depthBufferBits,
                captureFormat,
                RenderTextureReadWrite.Linear)
            {
                name = textureName,
                useMipMap = false,
                autoGenerateMips = false,
                antiAliasing = 1
            };
            texture.Create();
        }

        private RenderTextureFormat GetCaptureTextureFormat()
        {
            var captureFormat = SystemInfo.SupportsRenderTextureFormat(RenderTextureFormat.BGRA32)
                ? RenderTextureFormat.BGRA32
                : RenderTextureFormat.ARGB32;
            if (captureFormat != RenderTextureFormat.BGRA32)
            {
                Debug.LogWarning(
                    "UnitySenderController could not allocate a BGRA32 RenderTexture. " +
                    "The native NVENC path expects BGRA input, so streaming may fail until a conversion path is added.");
            }

            return captureFormat;
        }

        private void ReleaseMonoCaptureResources()
        {
            ReleaseRenderTexture(ref captureTexture);
        }

        private void ReleaseStereoCaptureResources()
        {
            if (leftEyeCaptureCamera != null)
            {
                leftEyeCaptureCamera.targetTexture = null;
                leftEyeCaptureCamera.enabled = false;
            }

            if (rightEyeCaptureCamera != null)
            {
                rightEyeCaptureCamera.targetTexture = null;
                rightEyeCaptureCamera.enabled = false;
            }

            ReleaseRenderTexture(ref leftCaptureTexture);
            ReleaseRenderTexture(ref rightCaptureTexture);
            ReleaseRenderTexture(ref bridgeCaptureTexture);
        }

        private void ReleaseRenderTexture(ref RenderTexture texture)
        {
            if (texture == null)
            {
                return;
            }

            texture.Release();
            Destroy(texture);
            texture = null;
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
                if (TryParseCaptureViewMode(savedEndpoint.captureViewMode, out var persistedCaptureViewMode))
                {
                    captureViewMode = persistedCaptureViewMode;
                    sourceParts.Add("saved_capture_mode");
                }
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
            stereoEyeSeparationMeters = Mathf.Max(stereoEyeSeparationMeters, 0.0f);
            statsPollIntervalSeconds = Mathf.Max(statsPollIntervalSeconds, 0.1f);
            autoStartRetryIntervalSeconds = Mathf.Max(autoStartRetryIntervalSeconds, 0.1f);

            configSourceSummary = string.Join("+", sourceParts);
            cachedActiveTargetHost = targetHost;
            lastLifecycleMessage = "Configured";
            Debug.Log(
                $"UnitySenderController config resolved. source={configSourceSummary} " +
                $"target={targetHost}:{videoPort} posePort={posePort} size={encodeWidth}x{encodeHeight} fps={fps} bitrate={bitrate} mode={captureViewMode} " +
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
            if (!string.IsNullOrWhiteSpace(launchOptions.captureViewMode) &&
                TryParseCaptureViewMode(launchOptions.captureViewMode, out var parsedCaptureViewMode))
            {
                captureViewMode = parsedCaptureViewMode;
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
            if (!UnitySenderRuntimeEndpointStore.SaveSavedEndpoint(detectedHost,
                                                                   ClampPort(videoPort),
                                                                   ClampPort(posePort),
                                                                   CaptureViewModeConfigName(captureViewMode)))
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
            GUILayout.Label($"Capture mode: {captureViewMode}  Stereo IPD: {stereoEyeSeparationMeters:0.000}m");
            GUILayout.Label($"Streaming: {streaming}  AutoRetry: {autoStartRetryPending}");
            GUILayout.Label($"Render event: {(renderEventFunc != IntPtr.Zero ? "ready" : "null")} / id={renderEventId}");
            if (IsStereoProjectionCaptureMode())
            {
                GUILayout.Label($"Stereo textures: L={(leftCaptureTexture != null ? $"{leftCaptureTexture.width}x{leftCaptureTexture.height}" : "null")} " +
                                $"R={(rightCaptureTexture != null ? $"{rightCaptureTexture.width}x{rightCaptureTexture.height}" : "null")}");
            }
            else
            {
                GUILayout.Label($"Capture texture: {(captureTexture != null ? $"{captureTexture.width}x{captureTexture.height}" : "null")}");
            }
            GUILayout.Label($"Last pose sender: {(string.IsNullOrEmpty(cachedPoseSenderHost) ? "<none>" : cachedPoseSenderHost)}");

            if (hasCachedStats)
            {
                GUILayout.Space(6.0f);
                GUILayout.Label($"Device ready: {cachedStats.unityDeviceReady != 0}");
                GUILayout.Label($"Source texture ready: {cachedStats.sourceTextureReady != 0}");
                GUILayout.Label($"Copied frame ready: {cachedStats.copiedFrameReady != 0}");
                GUILayout.Label($"Sender thread: {cachedStats.senderThreadRunning != 0}  Network thread: {cachedStats.networkThreadRunning != 0}");
                GUILayout.Label($"Source size: {cachedStats.sourceWidth}x{cachedStats.sourceHeight}");
                GUILayout.Label($"Configured views: {cachedStats.configuredViewCount}  Latest pair: {cachedStats.latestFramePairId}");
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
