using FastDrawingVisual.Controls;
using FastDrawingVisual.Rendering;
using FastDrawingVisual.Rendering.Backends;
using FastDrawingVisual.Rendering.Presentation;
using System;
using System.IO;
using System.Runtime.InteropServices;

namespace FastDrawingVisual
{
    /// <summary>
    /// Runtime renderer capability snapshot based on lightweight .NET probing.
    /// </summary>
    public static class RendererCapability
    {
        private static readonly Lazy<RendererCapabilityInfo> _current =
            new Lazy<RendererCapabilityInfo>(ProbeCurrent, isThreadSafe: true);

        public static RendererCapabilityInfo Current => _current.Value;

        public static bool Supports(RendererPreference preference)
            => Supports(preference, Current);

        internal static bool Supports(RendererPreference preference, RendererCapabilityInfo capability)
        {
            return preference switch
            {
                RendererPreference.Auto => true,
                RendererPreference.Skia => capability.CanUseSkia,
                RendererPreference.D3D9 => capability.CanUseNativeD3D9,
                RendererPreference.D3D11AirSpace => capability.CanUseDCompD3D11,
                RendererPreference.D3D11ShareD3D9 => capability.CanUseD3D11ShareD3D9,
                RendererPreference.Wpf => true,
                _ => false,
            };
        }

        private static RendererCapabilityInfo ProbeCurrent()
        {
            bool isWindows = RuntimeInformation.IsOSPlatform(OSPlatform.Windows);
            if (!isWindows)
            {
                return new RendererCapabilityInfo(
                    isWindows: false,
                    isWindows10OrGreater: false,
                    hasD3D9: false,
                    hasD3D11: false,
                    hasD3D12: false,
                    hasNativeD3D9Bridge: false,
                    hasNativeD3D11Bridge: false,
                    hasDCompPresentation: false);
            }

            var sysDir = Environment.GetFolderPath(Environment.SpecialFolder.System);
            bool hasD3D9 = File.Exists(Path.Combine(sysDir, "d3d9.dll"));
            bool hasD3D11 = File.Exists(Path.Combine(sysDir, "d3d11.dll"));
            bool hasD3D12 = File.Exists(Path.Combine(sysDir, "d3d12.dll"));
            bool isWindows10OrGreater = Environment.OSVersion.Version.Major >= 10;
            bool hasNativeD3D9Bridge = hasD3D9 && NativeD3D9BackendProbe.IsAvailable;
            bool hasNativeD3D11Bridge = hasD3D11 && NativeD3D11BackendProbe.IsAvailable;
            bool hasDCompPresentation = isWindows10OrGreater && DCompPresentationProbe.IsAvailable;

            return new RendererCapabilityInfo(
                isWindows,
                isWindows10OrGreater,
                hasD3D9,
                hasD3D11,
                hasD3D12,
                hasNativeD3D9Bridge,
                hasNativeD3D11Bridge,
                hasDCompPresentation);
        }
    }

    public sealed class RendererCapabilityInfo
    {
        public RendererCapabilityInfo(
            bool isWindows,
            bool isWindows10OrGreater,
            bool hasD3D9,
            bool hasD3D11,
            bool hasD3D12,
            bool hasNativeD3D9Bridge,
            bool hasNativeD3D11Bridge,
            bool hasDCompPresentation)
        {
            IsWindows = isWindows;
            IsWindows10OrGreater = isWindows10OrGreater;
            HasD3D9 = hasD3D9;
            HasD3D11 = hasD3D11;
            HasD3D12 = hasD3D12;
            HasNativeD3D9Bridge = hasNativeD3D9Bridge;
            HasNativeD3D11Bridge = hasNativeD3D11Bridge;
            HasDCompPresentation = hasDCompPresentation;
        }

        public bool IsWindows { get; }

        public bool IsWindows10OrGreater { get; }

        public bool HasD3D9 { get; }

        public bool HasD3D11 { get; }

        public bool HasD3D12 { get; }

        public bool HasNativeD3D9Bridge { get; }

        public bool HasNativeD3D11Bridge { get; }

        public bool HasDCompPresentation { get; }

        public bool CanUseSkia => IsWindows10OrGreater && HasD3D9 && HasD3D11 && HasD3D12;

        public bool CanUseNativeD3D9 => HasD3D9 && HasNativeD3D9Bridge;

        public bool CanUseDCompD3D11 => IsWindows10OrGreater && HasD3D11 && HasNativeD3D11Bridge && HasDCompPresentation;

        public bool CanUseD3D11ShareD3D9 => HasD3D9 && HasD3D11 && HasNativeD3D11Bridge;
    }

    internal static class NativeD3D9BackendProbe
    {
        private static readonly Lazy<bool> s_isAvailable = new(CheckAvailable, isThreadSafe: true);

        public static bool IsAvailable => s_isAvailable.Value;

        private static bool CheckAvailable()
        {
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                return false;

            var sysDir = Environment.GetFolderPath(Environment.SpecialFolder.System);
            if (!File.Exists(Path.Combine(sysDir, "d3d9.dll")))
                return false;

            var backendAssemblyPath = typeof(D3D9Backend).Assembly.Location;
            return !string.IsNullOrWhiteSpace(backendAssemblyPath) && File.Exists(backendAssemblyPath);
        }
    }

    internal static class NativeD3D11BackendProbe
    {
        private static readonly Lazy<bool> s_isAvailable = new(CheckAvailable, isThreadSafe: true);

        public static bool IsAvailable => s_isAvailable.Value;

        private static bool CheckAvailable()
        {
            if (!RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                return false;

            var sysDir = Environment.GetFolderPath(Environment.SpecialFolder.System);
            if (!File.Exists(Path.Combine(sysDir, "d3d11.dll")))
                return false;

            var backendAssemblyPath = typeof(D3D11SwapChainBackend).Assembly.Location;
            return !string.IsNullOrWhiteSpace(backendAssemblyPath) && File.Exists(backendAssemblyPath);
        }
    }
}
