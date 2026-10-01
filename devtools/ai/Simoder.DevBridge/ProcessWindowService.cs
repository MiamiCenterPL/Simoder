using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Simoder.DevBridge.Core;

namespace Simoder.DevBridge;

/// <summary>Discovers only EA, SimCity, and Simoder processes and validates their executable identity.</summary>
internal sealed class ProcessWindowService
{
    private static readonly HashSet<string> CandidateProcessNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "EADesktop",
        "EALaunchHelper",
        "SimCity",
        "simoder",
    };

    private readonly string gameRoot_;
    private readonly string runtimeRoot_;
    private readonly string devToolsAiRoot_;
    private readonly Dictionary<string, bool> trustCache_ = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Creates an allowlist rooted in the installed SimCity and Simoder directories.</summary>
    public ProcessWindowService(string gameRoot)
    {
        gameRoot_ = NormalizePath(gameRoot);
        runtimeRoot_ = NormalizePath(Path.Combine(gameRoot_, "simoder"));
        devToolsAiRoot_ = NormalizePath(Path.Combine(runtimeRoot_, "DevTools", "AI"));
    }

    /// <summary>Returns candidate process snapshots without exposing unrelated desktop processes.</summary>
    public IReadOnlyList<ProcessSnapshot> GetProcesses()
    {
        var snapshots = new List<ProcessSnapshot>();
        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                if (!CandidateProcessNames.Contains(process.ProcessName))
                {
                    continue;
                }

                var executablePath = TryGetExecutablePath(process);
                snapshots.Add(new ProcessSnapshot(
                    process.Id,
                    process.ProcessName,
                    executablePath ?? string.Empty,
                    TryGetSessionId(process),
                    executablePath is not null && IsAllowedExecutable(process.ProcessName, executablePath)));
            }
        }

        return snapshots.OrderBy(snapshot => snapshot.ProcessId).ToArray();
    }

    /// <summary>Returns visible top-level windows owned by the candidate process set.</summary>
    public IReadOnlyList<WindowSnapshot> GetWindows(IReadOnlyList<ProcessSnapshot> processes)
    {
        var byProcessId = processes.ToDictionary(process => process.ProcessId);
        var foreground = GetForegroundWindow();
        var windows = new List<WindowSnapshot>();

        EnumWindows((windowHandle, parameter) =>
        {
            if (!IsWindowVisible(windowHandle))
            {
                return true;
            }

            _ = parameter;
            _ = GetWindowThreadProcessId(windowHandle, out var processId);
            if (!byProcessId.TryGetValue(unchecked((int)processId), out var process))
            {
                return true;
            }

            if (!GetWindowRect(windowHandle, out var rectangle) ||
                !TryGetClientScreenRectangle(windowHandle, out var clientRectangle) ||
                rectangle.Width <= 0 || rectangle.Height <= 0 ||
                clientRectangle.Width <= 0 || clientRectangle.Height <= 0)
            {
                return true;
            }

            windows.Add(new WindowSnapshot(
                windowHandle.ToInt64(), process.ProcessId, process.Name,
                GetWindowTitle(windowHandle), rectangle.Left, rectangle.Top,
                rectangle.Width, rectangle.Height,
                clientRectangle.Left, clientRectangle.Top,
                clientRectangle.Width, clientRectangle.Height,
                windowHandle == foreground,
                process.IsAllowed));
            return true;
        }, IntPtr.Zero);

        return windows.OrderByDescending(window => window.IsForeground)
            .ThenByDescending(window => (long)window.Width * window.Height)
            .ToArray();
    }

    /// <summary>Finds the current version of a previously captured window.</summary>
    public WindowSnapshot? FindWindow(long handle)
    {
        var processes = GetProcesses();
        return GetWindows(processes).FirstOrDefault(window => window.Handle == handle);
    }

    /// <summary>Selects one allowlisted window for an explicit EA, SimCity, or automatic target.</summary>
    public WindowSnapshot SelectTargetWindow(string target)
    {
        var normalizedTarget = target.Trim().ToLowerInvariant();
        if (normalizedTarget is not ("auto" or "ea" or "simcity"))
        {
            throw new ArgumentException("Window target must be auto, ea, or simcity.", nameof(target));
        }

        var allowedWindows = GetWindows(GetProcesses()).Where(window => window.IsAllowed).ToArray();
        IEnumerable<WindowSnapshot> candidates = normalizedTarget switch
        {
            "ea" => allowedWindows.Where(window =>
                window.ProcessName.Equals("EADesktop", StringComparison.OrdinalIgnoreCase) ||
                window.ProcessName.Equals("EALaunchHelper", StringComparison.OrdinalIgnoreCase)),
            "simcity" => allowedWindows.Where(window =>
                window.ProcessName.Equals("SimCity", StringComparison.OrdinalIgnoreCase)),
            _ => allowedWindows.OrderByDescending(window =>
                window.ProcessName.Equals("SimCity", StringComparison.OrdinalIgnoreCase)),
        };

        return candidates.OrderByDescending(window => window.IsForeground)
            .ThenByDescending(window => (long)window.Width * window.Height)
            .FirstOrDefault()
            ?? throw new InvalidOperationException($"No allowlisted visible {normalizedTarget} window is available.");
    }

    /// <summary>Returns true only when the exact path, expected name, and publisher policy all match.</summary>
    public bool IsAllowedExecutable(string processName, string executablePath)
    {
        var normalizedPath = NormalizePath(executablePath);
        if (processName.Equals("simoder", StringComparison.OrdinalIgnoreCase))
        {
            return normalizedPath.Equals(NormalizePath(Path.Combine(runtimeRoot_, "simoder.exe")), StringComparison.OrdinalIgnoreCase);
        }

        if (processName.Equals("Simoder.DevBridge", StringComparison.OrdinalIgnoreCase) ||
            processName.Equals("Simoder.McpServer", StringComparison.OrdinalIgnoreCase))
        {
            return IsWithin(normalizedPath, devToolsAiRoot_);
        }

        if (processName.Equals("SimCity", StringComparison.OrdinalIgnoreCase))
        {
            var exactGamePaths = new[]
            {
                NormalizePath(Path.Combine(gameRoot_, "SimCity", "SimCity.exe")),
                NormalizePath(Path.Combine(gameRoot_, "SimCityUserData", "Patches", "UpdatedApp", "SimCity.exe")),
            };
            return exactGamePaths.Contains(normalizedPath, StringComparer.OrdinalIgnoreCase) &&
                   IsTrustedEaCached(normalizedPath);
        }

        if (!processName.Equals("EADesktop", StringComparison.OrdinalIgnoreCase) &&
            !processName.Equals("EALaunchHelper", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var fileName = Path.GetFileNameWithoutExtension(normalizedPath);
        if (!fileName.Equals(processName, StringComparison.OrdinalIgnoreCase) || !IsKnownEaDirectory(normalizedPath))
        {
            return false;
        }

        return IsTrustedEaCached(normalizedPath);
    }

    /// <summary>Checks the trusted publisher once per executable path.</summary>
    private bool IsTrustedEaCached(string path)
    {
        lock (trustCache_)
        {
            if (!trustCache_.TryGetValue(path, out var trusted))
            {
                trusted = ExecutableTrustVerifier.IsTrustedElectronicArtsBinary(path);
                trustCache_[path] = trusted;
            }

            return trusted;
        }
    }

    /// <summary>Checks the small set of supported EA installation roots.</summary>
    private static bool IsKnownEaDirectory(string executablePath)
    {
        var roots = new[]
        {
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        };
        return roots.Where(root => !string.IsNullOrWhiteSpace(root))
            .Select(root => NormalizePath(Path.Combine(root, "Electronic Arts")))
            .Any(root => IsWithin(executablePath, root));
    }

    /// <summary>Returns true when a path is strictly below an expected root.</summary>
    private static bool IsWithin(string path, string root) =>
        path.StartsWith(root.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);

    /// <summary>Obtains an executable path while tolerating protected and exiting processes.</summary>
    private static string? TryGetExecutablePath(Process process)
    {
        try
        {
            return process.MainModule?.FileName;
        }
        catch (Exception exception) when (exception is InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException)
        {
            return null;
        }
    }

    /// <summary>Obtains a Windows session id while tolerating exiting processes.</summary>
    private static int TryGetSessionId(Process process)
    {
        try
        {
            return process.SessionId;
        }
        catch (InvalidOperationException)
        {
            return -1;
        }
    }

    /// <summary>Returns a canonical absolute path without resolving symbolic links.</summary>
    private static string NormalizePath(string path) =>
        Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

    /// <summary>Reads a bounded top-level window title.</summary>
    private static string GetWindowTitle(IntPtr windowHandle)
    {
        var length = Math.Min(GetWindowTextLength(windowHandle), 4096);
        if (length <= 0)
        {
            return string.Empty;
        }

        var builder = new StringBuilder(length + 1);
        _ = GetWindowText(windowHandle, builder, builder.Capacity);
        return builder.ToString();
    }

    /// <summary>Converts the client rectangle into physical screen coordinates.</summary>
    private static bool TryGetClientScreenRectangle(IntPtr windowHandle, out NativeRectangle rectangle)
    {
        rectangle = default;
        if (!GetClientRect(windowHandle, out var clientRectangle))
        {
            return false;
        }

        var topLeft = new NativePoint { X = clientRectangle.Left, Y = clientRectangle.Top };
        var bottomRight = new NativePoint { X = clientRectangle.Right, Y = clientRectangle.Bottom };
        if (!ClientToScreen(windowHandle, ref topLeft) || !ClientToScreen(windowHandle, ref bottomRight))
        {
            return false;
        }

        rectangle = new NativeRectangle
        {
            Left = topLeft.X,
            Top = topLeft.Y,
            Right = bottomRight.X,
            Bottom = bottomRight.Y,
        };
        return true;
    }

    private delegate bool EnumWindowsCallback(IntPtr windowHandle, IntPtr parameter);

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRectangle
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
        public readonly int Width => Right - Left;
        public readonly int Height => Bottom - Top;
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(EnumWindowsCallback callback, IntPtr parameter);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(IntPtr windowHandle);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr windowHandle, out uint processId);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetWindowText(IntPtr windowHandle, StringBuilder text, int maximumCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetWindowTextLength(IntPtr windowHandle);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetClientRect(IntPtr windowHandle, out NativeRectangle rectangle);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(IntPtr windowHandle, out NativeRectangle rectangle);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ClientToScreen(IntPtr windowHandle, ref NativePoint point);
}
