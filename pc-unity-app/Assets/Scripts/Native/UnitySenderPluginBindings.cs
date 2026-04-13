using System;
using System.Runtime.InteropServices;
using System.Text;

namespace VideoTest.UnityIntegration
{
    public static class UnitySenderPluginBindings
    {
        private const string DllName = "unity_sender_plugin";

        [StructLayout(LayoutKind.Sequential)]
        public struct UnitySenderPose
        {
            public float positionX;
            public float positionY;
            public float positionZ;
            public float orientationX;
            public float orientationY;
            public float orientationZ;
            public float orientationW;
            public uint trackingFlags;
            public uint sequence;
            public ulong timestampUs;
            public ulong packetsReceived;
            public ulong sequenceGaps;
            public int hasPose;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct UnitySenderStats
        {
            public int unityDeviceReady;
            public int sourceTextureReady;
            public int copiedFrameReady;
            public int networkThreadRunning;
            public int senderThreadRunning;
            public uint sourceWidth;
            public uint sourceHeight;
            public ulong renderThreadCopyCount;
            public ulong posePacketsReceived;
            public ulong controlPacketsReceived;
            public uint lastPoseSequence;
            public uint configuredViewCount;
            public uint latestFramePairId;
        }

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool UnitySender_Configure(
            [MarshalAs(UnmanagedType.LPStr)] string targetHost,
            ushort videoPort,
            ushort posePort,
            int fps,
            uint bitrate,
            ushort width,
            ushort height);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void UnitySender_SetTexture(IntPtr textureHandle);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void UnitySender_SetTextureForView(int viewId, IntPtr textureHandle);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool UnitySender_Start();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void UnitySender_Stop();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool UnitySender_IsRunning();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool UnitySender_GetLatestPose(out UnitySenderPose pose);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool UnitySender_GetStats(out UnitySenderStats stats);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        [return: MarshalAs(UnmanagedType.I1)]
        private static extern bool UnitySender_GetLastPoseSenderIpv4(StringBuilder outBuffer, UIntPtr bufferSize);

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int UnitySender_GetCopyTextureEventId();

        [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr UnitySender_GetRenderEventFunc();

        public static bool TryGetLastPoseSenderIpv4(out string host)
        {
            var buffer = new StringBuilder(64);
            if (UnitySender_GetLastPoseSenderIpv4(buffer, (UIntPtr)buffer.Capacity))
            {
                host = buffer.ToString();
                return !string.IsNullOrWhiteSpace(host);
            }

            host = string.Empty;
            return false;
        }
    }
}
