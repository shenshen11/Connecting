using System;
using System.IO;
using UnityEngine;

namespace VideoTest.UnityIntegration
{
    internal static class UnitySenderRuntimeEndpointStore
    {
        [Serializable]
        private sealed class PersistedEndpointRecord
        {
            public string targetHost = "127.0.0.1";
            public int videoPort = 25674;
            public int posePort = 25672;
        }

        internal struct SavedEndpoint
        {
            public string targetHost;
            public int videoPort;
            public int posePort;
        }

        public static string GetConfigPath()
        {
            return Path.Combine(Application.persistentDataPath, "VideoTestUnitySender", "last_successful_endpoint.json");
        }

        public static bool TryLoadSavedEndpoint(out SavedEndpoint endpoint)
        {
            endpoint = default;
            var path = GetConfigPath();
            if (!File.Exists(path))
            {
                return false;
            }

            try
            {
                var json = File.ReadAllText(path);
                var record = JsonUtility.FromJson<PersistedEndpointRecord>(json);
                if (record == null || string.IsNullOrWhiteSpace(record.targetHost))
                {
                    return false;
                }

                endpoint.targetHost = record.targetHost.Trim();
                endpoint.videoPort = record.videoPort;
                endpoint.posePort = record.posePort;
                return true;
            }
            catch (Exception ex)
            {
                Debug.LogWarning($"UnitySenderController failed to load saved endpoint from {path}: {ex.Message}");
                return false;
            }
        }

        public static bool SaveSavedEndpoint(string targetHost, int videoPort, int posePort)
        {
            if (string.IsNullOrWhiteSpace(targetHost))
            {
                return false;
            }

            var path = GetConfigPath();
            var directory = Path.GetDirectoryName(path);
            if (string.IsNullOrEmpty(directory))
            {
                return false;
            }

            try
            {
                Directory.CreateDirectory(directory);
                var record = new PersistedEndpointRecord
                {
                    targetHost = targetHost.Trim(),
                    videoPort = videoPort,
                    posePort = posePort
                };
                var json = JsonUtility.ToJson(record, true);
                File.WriteAllText(path, json);
                return true;
            }
            catch (Exception ex)
            {
                Debug.LogWarning($"UnitySenderController failed to save endpoint to {path}: {ex.Message}");
                return false;
            }
        }

        public static bool DeleteSavedEndpoint()
        {
            var path = GetConfigPath();
            if (!File.Exists(path))
            {
                return false;
            }

            try
            {
                File.Delete(path);
                return true;
            }
            catch (Exception ex)
            {
                Debug.LogWarning($"UnitySenderController failed to delete saved endpoint at {path}: {ex.Message}");
                return false;
            }
        }
    }
}
