using System;

namespace VideoTest.UnityIntegration
{
    internal struct UnitySenderLaunchOptions
    {
        public string targetHost;
        public int? videoPort;
        public int? posePort;
        public int? encodeWidth;
        public int? encodeHeight;
        public int? fps;
        public int? bitrate;
        public bool disableAutoStart;
        public bool ignoreSavedEndpoint;
        public bool resetSavedEndpoint;

        public bool HasAnyOverrides =>
            !string.IsNullOrWhiteSpace(targetHost) ||
            videoPort.HasValue ||
            posePort.HasValue ||
            encodeWidth.HasValue ||
            encodeHeight.HasValue ||
            fps.HasValue ||
            bitrate.HasValue ||
            disableAutoStart;

        public static UnitySenderLaunchOptions Parse(string[] args)
        {
            var options = new UnitySenderLaunchOptions();
            if (args == null || args.Length == 0)
            {
                return options;
            }

            for (var i = 0; i < args.Length; ++i)
            {
                switch (args[i])
                {
                    case "-vt-host":
                    case "--vt-host":
                        if (TryReadNextValue(args, ref i, out var host))
                        {
                            options.targetHost = host;
                        }
                        break;

                    case "-vt-video-port":
                    case "--vt-video-port":
                        if (TryReadNextInt(args, ref i, out var videoPort))
                        {
                            options.videoPort = videoPort;
                        }
                        break;

                    case "-vt-pose-port":
                    case "--vt-pose-port":
                        if (TryReadNextInt(args, ref i, out var posePort))
                        {
                            options.posePort = posePort;
                        }
                        break;

                    case "-vt-width":
                    case "--vt-width":
                        if (TryReadNextInt(args, ref i, out var width))
                        {
                            options.encodeWidth = width;
                        }
                        break;

                    case "-vt-height":
                    case "--vt-height":
                        if (TryReadNextInt(args, ref i, out var height))
                        {
                            options.encodeHeight = height;
                        }
                        break;

                    case "-vt-fps":
                    case "--vt-fps":
                        if (TryReadNextInt(args, ref i, out var fps))
                        {
                            options.fps = fps;
                        }
                        break;

                    case "-vt-bitrate":
                    case "--vt-bitrate":
                        if (TryReadNextInt(args, ref i, out var bitrate))
                        {
                            options.bitrate = bitrate;
                        }
                        break;

                    case "-vt-no-autostart":
                    case "--vt-no-autostart":
                        options.disableAutoStart = true;
                        break;

                    case "-vt-ignore-saved-endpoint":
                    case "--vt-ignore-saved-endpoint":
                        options.ignoreSavedEndpoint = true;
                        break;

                    case "-vt-reset-saved-endpoint":
                    case "--vt-reset-saved-endpoint":
                        options.resetSavedEndpoint = true;
                        break;
                }
            }

            return options;
        }

        private static bool TryReadNextValue(string[] args, ref int index, out string value)
        {
            if (index + 1 >= args.Length)
            {
                value = string.Empty;
                return false;
            }

            value = args[index + 1];
            index += 1;
            return !string.IsNullOrWhiteSpace(value);
        }

        private static bool TryReadNextInt(string[] args, ref int index, out int value)
        {
            value = 0;
            if (!TryReadNextValue(args, ref index, out var rawValue))
            {
                return false;
            }

            return int.TryParse(rawValue, out value);
        }
    }
}
