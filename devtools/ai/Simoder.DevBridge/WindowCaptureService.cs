using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Windows.Automation;
using Simoder.DevBridge.Core;

namespace Simoder.DevBridge;

/// <summary>Captures allowlisted windows and performs UIA-first, input-fallback actions.</summary>
internal sealed class WindowCaptureService
{
    private const uint PrintWindowRenderFullContent = 0x00000002;
    private readonly ProcessWindowService windows_;
    private readonly WindowsGraphicsCaptureService graphicsCapture_ = new();
    private readonly Dictionary<string, CaptureSnapshot> captures_ = new(StringComparer.Ordinal);
    private readonly object gate_ = new();

    /// <summary>Creates a capture service bound to the strict process/window allowlist.</summary>
    public WindowCaptureService(ProcessWindowService windows)
    {
        windows_ = windows;
    }

    /// <summary>Captures one allowlisted target and records a ten-second input authorization.</summary>
    public CaptureSnapshot Capture(string target)
    {
        var window = windows_.SelectTargetWindow(target);
        var accessibleControls = ReadAccessibleControls(new IntPtr(window.Handle));
        var wgcBytes = graphicsCapture_.TryCapturePngAsync(
                new IntPtr(window.Handle), TimeSpan.FromMilliseconds(1500))
            .GetAwaiter().GetResult();
        var pngBytes = wgcBytes is not null && HasExpectedDimensions(wgcBytes, window.Width, window.Height)
            ? wgcBytes
            : CaptureFallbackPng(window);
        var encoded = Convert.ToBase64String(pngBytes);

        var capture = new CaptureSnapshot(
            Guid.NewGuid().ToString("N"), DateTimeOffset.UtcNow, window,
            encoded, accessibleControls);
        lock (gate_)
        {
            captures_[capture.CaptureId] = capture;
            foreach (var staleId in captures_.Where(pair =>
                         DateTimeOffset.UtcNow - pair.Value.CapturedAtUtc > TimeSpan.FromMinutes(1))
                         .Select(pair => pair.Key).ToArray())
            {
                captures_.Remove(staleId);
            }

            while (captures_.Count > 16)
            {
                captures_.Remove(captures_.OrderBy(pair => pair.Value.CapturedAtUtc).First().Key);
            }
        }

        return capture;
    }

    /// <summary>Gets an existing capture or rejects an unknown identifier.</summary>
    public CaptureSnapshot GetCapture(string captureId)
    {
        lock (gate_)
        {
            return captures_.TryGetValue(captureId, out var capture)
                ? capture
                : throw new KeyNotFoundException("The capture id is unknown or expired.");
        }
    }

    /// <summary>Returns the most recent capture and its image for diagnostics.</summary>
    public CaptureSnapshot? GetLastCapture()
    {
        lock (gate_)
        {
            return captures_.Values.OrderByDescending(capture => capture.CapturedAtUtc)
                .FirstOrDefault();
        }
    }

    /// <summary>Performs one click after revalidating capture age, window identity, and geometry.</summary>
    public string Click(ClickRequest request)
    {
        var capture = GetCapture(request.CaptureId);
        var currentWindow = windows_.FindWindow(capture.Window.Handle)
            ?? throw new InvalidOperationException("The captured window no longer exists.");
        var (screenX, screenY) = CaptureGuard.GetAuthorizedScreenPoint(
            capture, currentWindow, DateTimeOffset.UtcNow, request.NormalizedX, request.NormalizedY);

        EnsureForeground(currentWindow);
        if (TryInvokeAccessibleControl(currentWindow, screenX, screenY))
        {
            return "uia_invoke";
        }

        if (!SetCursorPos(screenX, screenY))
        {
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "SetCursorPos failed.");
        }

        var inputs = new[]
        {
            NativeInput.Mouse(0x0002),
            NativeInput.Mouse(0x0004),
        };
        EnsureForeground(currentWindow);
        if (SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<NativeInput>()) != inputs.Length)
        {
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "SendInput click failed.");
        }

        return "send_input";
    }

    /// <summary>Sends one allowlisted key to one allowlisted target window.</summary>
    public void SendKey(SendKeyRequest request)
    {
        var window = windows_.SelectTargetWindow(request.Target);
        EnsureForeground(window);
        var virtualKey = request.Key switch
        {
            AllowedKey.Escape => (ushort)0x1B,
            AllowedKey.Enter => (ushort)0x0D,
            AllowedKey.Space => (ushort)0x20,
            _ => throw new ArgumentOutOfRangeException(nameof(request), "The requested key is not allowlisted."),
        };
        var inputs = new[]
        {
            NativeInput.Keyboard(virtualKey, 0),
            NativeInput.Keyboard(virtualKey, 0x0002),
        };
        if (SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<NativeInput>()) != inputs.Length)
        {
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "SendInput key press failed.");
        }
    }

    /// <summary>Captures the target client area using PrintWindow and a foreground screen-copy fallback.</summary>
    private static byte[] CaptureFallbackPng(WindowSnapshot window)
    {
        var bitmap = new Bitmap(window.Width, window.Height, PixelFormat.Format32bppArgb);
        var printWindowSucceeded = false;
        using (var graphics = Graphics.FromImage(bitmap))
        {
            var deviceContext = graphics.GetHdc();
            try
            {
                printWindowSucceeded = PrintWindow(
                    new IntPtr(window.Handle), deviceContext, PrintWindowRenderFullContent);
            }
            finally
            {
                graphics.ReleaseHdc(deviceContext);
            }
        }

        if (printWindowSucceeded)
        {
            return EncodeBitmap(bitmap);
        }

        if (!window.IsForeground)
        {
            bitmap.Dispose();
            throw new InvalidOperationException("PrintWindow failed and the allowlisted target is not foreground; activate it and capture again.");
        }

        using (var graphics = Graphics.FromImage(bitmap))
        {
            graphics.CopyFromScreen(window.Left, window.Top, 0, 0, bitmap.Size, CopyPixelOperation.SourceCopy);
        }
        return EncodeBitmap(bitmap);
    }

    /// <summary>Encodes and disposes one fallback bitmap as PNG.</summary>
    private static byte[] EncodeBitmap(Bitmap bitmap)
    {
        using (bitmap)
        using (var stream = new MemoryStream())
        {
            bitmap.Save(stream, ImageFormat.Png);
            return stream.ToArray();
        }
    }

    /// <summary>Checks that the frame pixel grid matches the geometry used for normalized clicks.</summary>
    private static bool HasExpectedDimensions(byte[] pngBytes, int expectedWidth, int expectedHeight)
    {
        try
        {
            using var stream = new MemoryStream(pngBytes, writable: false);
            using var image = Image.FromStream(stream, useEmbeddedColorManagement: false, validateImageData: true);
            return image.Width == expectedWidth && image.Height == expectedHeight;
        }
        catch (ArgumentException)
        {
            return false;
        }
    }

    /// <summary>Returns a bounded breadth-first UI Automation snapshot.</summary>
    private static IReadOnlyList<AccessibleControlSnapshot> ReadAccessibleControls(IntPtr windowHandle)
    {
        var controls = new List<AccessibleControlSnapshot>();
        try
        {
            var root = AutomationElement.FromHandle(windowHandle);
            var walker = TreeWalker.ControlViewWalker;
            var queue = new Queue<(AutomationElement Element, int Depth)>();
            queue.Enqueue((root, 0));
            while (queue.Count > 0 && controls.Count < 64)
            {
                var (element, depth) = queue.Dequeue();
                if (depth > 0)
                {
                    var rectangle = element.Current.BoundingRectangle;
                    controls.Add(new AccessibleControlSnapshot(
                        element.Current.Name ?? string.Empty,
                        element.Current.AutomationId ?? string.Empty,
                        element.Current.ControlType?.ProgrammaticName ?? string.Empty,
                        (int)rectangle.Left, (int)rectangle.Top,
                        Math.Max(0, (int)rectangle.Width), Math.Max(0, (int)rectangle.Height),
                        element.Current.IsEnabled));
                }

                if (depth >= 4)
                {
                    continue;
                }

                for (var child = walker.GetFirstChild(element); child is not null; child = walker.GetNextSibling(child))
                {
                    queue.Enqueue((child, depth + 1));
                }
            }
        }
        catch (Exception exception) when (exception is ElementNotAvailableException or InvalidOperationException or COMException)
        {
            return controls;
        }
        return controls;
    }

    /// <summary>Invokes an accessible button at the click point when it belongs to the expected process.</summary>
    private static bool TryInvokeAccessibleControl(WindowSnapshot window, int screenX, int screenY)
    {
        try
        {
            var element = AutomationElement.FromPoint(new System.Windows.Point(screenX, screenY));
            if (element.Current.ProcessId != window.ProcessId || !element.Current.IsEnabled ||
                !element.TryGetCurrentPattern(InvokePattern.Pattern, out var pattern))
            {
                return false;
            }

            ((InvokePattern)pattern).Invoke();
            return true;
        }
        catch (Exception exception) when (exception is ElementNotAvailableException or InvalidOperationException or COMException)
        {
            return false;
        }
    }

    /// <summary>Verifies that input cannot silently activate or escape the already-active target.</summary>
    private static void EnsureForeground(WindowSnapshot window)
    {
        var handle = new IntPtr(window.Handle);
        if (!window.IsAllowed || !window.IsForeground || GetForegroundWindow() != handle)
        {
            throw new InvalidOperationException("The allowlisted target must already be the foreground window.");
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeInput
    {
        public uint Type;
        public NativeInputUnion Data;

        /// <summary>Creates one mouse input frame.</summary>
        public static NativeInput Mouse(uint flags) => new()
        {
            Type = 0,
            Data = new NativeInputUnion { Mouse = new NativeMouseInput { Flags = flags } },
        };

        /// <summary>Creates one keyboard input frame.</summary>
        public static NativeInput Keyboard(ushort virtualKey, uint flags) => new()
        {
            Type = 1,
            Data = new NativeInputUnion { Keyboard = new NativeKeyboardInput { VirtualKey = virtualKey, Flags = flags } },
        };
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct NativeInputUnion
    {
        [FieldOffset(0)] public NativeMouseInput Mouse;
        [FieldOffset(0)] public NativeKeyboardInput Keyboard;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeMouseInput
    {
        public int X;
        public int Y;
        public uint MouseData;
        public uint Flags;
        public uint Time;
        public IntPtr ExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeKeyboardInput
    {
        public ushort VirtualKey;
        public ushort ScanCode;
        public uint Flags;
        public uint Time;
        public IntPtr ExtraInfo;
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PrintWindow(IntPtr windowHandle, IntPtr deviceContext, uint flags);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint inputCount, NativeInput[] inputs, int size);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

}
