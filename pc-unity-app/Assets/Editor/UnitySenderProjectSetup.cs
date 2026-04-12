using System;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.SceneManagement;
using VideoTest.UnityIntegration;

namespace VideoTest.UnityIntegration.Editor
{
    public static class UnitySenderProjectSetup
    {
        private const string SampleScenePath = "Assets/Scenes/SampleScene.unity";

        [MenuItem("VideoTest/Apply Unity Sender Setup")]
        public static void ApplyAll()
        {
            ApplyRecommendedProjectSettings();
            InstallPluginBinary();
            ConfigurePluginImporter();
            SetupSampleScene();
            AssetDatabase.Refresh();
            Debug.Log("VideoTest Unity sender setup applied.");
        }

        [MenuItem("VideoTest/Apply Recommended Project Settings")]
        public static void ApplyRecommendedProjectSettings()
        {
            EditorUserBuildSettings.SwitchActiveBuildTarget(BuildTargetGroup.Standalone, BuildTarget.StandaloneWindows64);

            PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.StandaloneWindows64, false);
            PlayerSettings.SetGraphicsAPIs(BuildTarget.StandaloneWindows64, new[] { GraphicsDeviceType.Direct3D11 });

            PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.StandaloneWindows, false);
            PlayerSettings.SetGraphicsAPIs(BuildTarget.StandaloneWindows, new[] { GraphicsDeviceType.Direct3D11 });

            PlayerSettings.graphicsJobs = false;
            EditorBuildSettings.scenes = new[]
            {
                new EditorBuildSettingsScene(SampleScenePath, true)
            };

            AssetDatabase.SaveAssets();
            Debug.Log("Applied Unity sender project settings: D3D11 only, Graphics Jobs off, SampleScene added to build.");
        }

        [MenuItem("VideoTest/Install Local Native Plugin Binary")]
        public static void InstallPluginBinary()
        {
            var projectRoot = Directory.GetParent(Application.dataPath)?.FullName;
            if (string.IsNullOrEmpty(projectRoot))
            {
                Debug.LogError("Unable to resolve the Unity project root.");
                return;
            }

            var workspaceRoot = Directory.GetParent(projectRoot)?.FullName;
            if (string.IsNullOrEmpty(workspaceRoot))
            {
                Debug.LogError("Unable to resolve the workspace root.");
                return;
            }

            var sourceDll = Path.Combine(workspaceRoot, "windows-native", "build", "Debug", "unity_sender_plugin.dll");
            var sourcePdb = Path.Combine(workspaceRoot, "windows-native", "build", "Debug", "unity_sender_plugin.pdb");
            var pluginDirectory = Path.Combine(Application.dataPath, "Plugins", "x86_64");
            Directory.CreateDirectory(pluginDirectory);

            if (!File.Exists(sourceDll))
            {
                Debug.LogError($"Native plugin DLL not found: {sourceDll}");
                return;
            }

            File.Copy(sourceDll, Path.Combine(pluginDirectory, "unity_sender_plugin.dll"), true);
            if (File.Exists(sourcePdb))
            {
                File.Copy(sourcePdb, Path.Combine(pluginDirectory, "unity_sender_plugin.pdb"), true);
            }

            AssetDatabase.Refresh();
            ConfigurePluginImporter();
            Debug.Log($"Installed local native plugin binary from {sourceDll}");
        }

        [MenuItem("VideoTest/Configure Native Plugin Importer")]
        public static void ConfigurePluginImporter()
        {
            ConfigurePluginImporterAtPath("Assets/Plugins/x86_64/unity_sender_plugin.dll");
            ConfigurePluginImporterAtPath("Assets/Plugins/x86_64/unity_sender_plugin.pdb");
            AssetDatabase.WriteImportSettingsIfDirty("Assets/Plugins/x86_64/unity_sender_plugin.dll");
            AssetDatabase.Refresh();
        }

        [MenuItem("VideoTest/Validate Native Plugin Load In Editor")]
        public static void ValidateNativePluginLoadInEditor()
        {
            try
            {
                var isRunning = UnitySenderPluginBindings.UnitySender_IsRunning();
                Debug.Log($"Unity sender native plugin loaded successfully in Editor. isRunning={isRunning}");
            }
            catch (DllNotFoundException ex)
            {
                Debug.LogError($"Unity sender native plugin failed to load in Editor: {ex.Message}");
                throw;
            }
            catch (EntryPointNotFoundException ex)
            {
                Debug.LogError($"Unity sender native plugin entry point mismatch in Editor: {ex.Message}");
                throw;
            }
            catch (Exception ex)
            {
                Debug.LogError($"Unity sender native plugin validation failed in Editor: {ex}");
                throw;
            }
        }

        [MenuItem("VideoTest/Setup Sample Scene")]
        public static void SetupSampleScene()
        {
            var scene = EditorSceneManager.OpenScene(SampleScenePath, OpenSceneMode.Single);

            var camera = Camera.main;
            if (camera == null)
            {
                var cameraObject = new GameObject("Main Camera");
                camera = cameraObject.AddComponent<Camera>();
                camera.tag = "MainCamera";
            }

            camera.transform.position = new Vector3(0.0f, 1.6f, -3.0f);
            camera.transform.rotation = Quaternion.Euler(10.0f, 0.0f, 0.0f);
            var senderController = camera.GetComponent<UnitySenderController>();
            if (senderController == null)
            {
                senderController = camera.gameObject.AddComponent<UnitySenderController>();
            }
            ApplyControllerDefaults(senderController);

            EnsureDirectionalLight();
            EnsurePrimitive("VT_Ground", PrimitiveType.Plane, Vector3.zero, Vector3.one * 3.0f, new Color(0.82f, 0.82f, 0.82f));
            EnsurePrimitive("VT_CubeNear", PrimitiveType.Cube, new Vector3(-0.7f, 1.0f, 2.5f), Vector3.one * 0.5f, new Color(0.88f, 0.32f, 0.24f));
            EnsurePrimitive("VT_CubeFar", PrimitiveType.Cube, new Vector3(1.8f, 1.2f, 6.5f), Vector3.one * 1.0f, new Color(0.22f, 0.64f, 0.91f));
            EnsurePrimitive("VT_SphereMid", PrimitiveType.Sphere, new Vector3(0.2f, 1.3f, 4.5f), Vector3.one * 0.8f, new Color(0.92f, 0.78f, 0.18f));
            EnsurePrimitive("VT_CapsuleSide", PrimitiveType.Capsule, new Vector3(-2.0f, 1.0f, 5.2f), Vector3.one * 0.8f, new Color(0.32f, 0.82f, 0.42f));

            EditorSceneManager.MarkSceneDirty(scene);
            EditorSceneManager.SaveScene(scene);
            Debug.Log("Configured SampleScene with a camera, light, and basic parallax geometry.");
        }

        private static void ApplyControllerDefaults(UnitySenderController controller)
        {
            var serializedObject = new SerializedObject(controller);
            serializedObject.FindProperty("targetHost").stringValue = "auto";
            serializedObject.FindProperty("videoPort").intValue = 25674;
            serializedObject.FindProperty("posePort").intValue = 25672;
            serializedObject.FindProperty("encodeWidth").intValue = 1280;
            serializedObject.FindProperty("encodeHeight").intValue = 720;
            serializedObject.FindProperty("fps").intValue = 15;
            serializedObject.FindProperty("bitrate").intValue = 4000000;
            serializedObject.FindProperty("startOnPlay").boolValue = true;
            serializedObject.FindProperty("applyPoseToCamera").boolValue = true;
            serializedObject.FindProperty("recenterKey").enumValueIndex = (int)KeyCode.R;
            serializedObject.FindProperty("showLocalPreview").boolValue = true;
            serializedObject.FindProperty("previewCameraDepthOffset").floatValue = 1.0f;
            serializedObject.FindProperty("showDebugOverlay").boolValue = true;
            serializedObject.FindProperty("toggleDebugOverlayKey").enumValueIndex = (int)KeyCode.F1;
            serializedObject.FindProperty("allowCommandLineOverrides").boolValue = true;
            serializedObject.FindProperty("useSavedEndpoint").boolValue = true;
            serializedObject.FindProperty("rememberSuccessfulEndpoint").boolValue = true;
            serializedObject.FindProperty("statsPollIntervalSeconds").floatValue = 0.5f;
            serializedObject.FindProperty("retryAutoStartWhenUsingAutoHost").boolValue = true;
            serializedObject.FindProperty("autoStartRetryIntervalSeconds").floatValue = 1.0f;
            serializedObject.ApplyModifiedPropertiesWithoutUndo();
        }

        private static void EnsureDirectionalLight()
        {
            foreach (var light in UnityEngine.Object.FindObjectsOfType<Light>())
            {
                if (light.type == LightType.Directional)
                {
                    light.transform.rotation = Quaternion.Euler(50.0f, -35.0f, 0.0f);
                    return;
                }
            }

            var lightObject = new GameObject("Directional Light");
            var lightComponent = lightObject.AddComponent<Light>();
            lightComponent.type = LightType.Directional;
            lightObject.transform.rotation = Quaternion.Euler(50.0f, -35.0f, 0.0f);
        }

        private static void EnsurePrimitive(string objectName,
                                            PrimitiveType primitiveType,
                                            Vector3 position,
                                            Vector3 scale,
                                            Color color)
        {
            var existing = GameObject.Find(objectName);
            var gameObject = existing != null ? existing : GameObject.CreatePrimitive(primitiveType);
            gameObject.name = objectName;
            gameObject.transform.position = position;
            gameObject.transform.localScale = scale;

            var renderer = gameObject.GetComponent<Renderer>();
            if (renderer != null)
            {
                var material = renderer.sharedMaterial;
                if (material == null)
                {
                    material = new Material(Shader.Find("Standard"));
                    renderer.sharedMaterial = material;
                }
                material.color = color;
            }
        }

        private static void ConfigurePluginImporterAtPath(string assetPath)
        {
            if (!(AssetImporter.GetAtPath(assetPath) is PluginImporter pluginImporter))
            {
                return;
            }

            pluginImporter.SetCompatibleWithAnyPlatform(false);
            pluginImporter.SetCompatibleWithEditor(assetPath.EndsWith(".dll"));
            pluginImporter.SetCompatibleWithPlatform(BuildTarget.StandaloneWindows64, assetPath.EndsWith(".dll"));
            pluginImporter.SetCompatibleWithPlatform(BuildTarget.StandaloneWindows, false);
            pluginImporter.SetCompatibleWithPlatform(BuildTarget.StandaloneLinux64, false);
            pluginImporter.SetCompatibleWithPlatform(BuildTarget.StandaloneOSX, false);
            pluginImporter.SaveAndReimport();
        }
    }
}
