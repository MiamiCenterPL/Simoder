using System.Runtime.InteropServices;
using Windows.AI.MachineLearning;
using Windows.Graphics.Capture;
using Windows.Graphics.DirectX;
using Windows.Graphics.Imaging;
using Windows.Storage.Streams;
using WinRT;

namespace Simoder.DevBridge;

/// <summary>Acquires one HWND-bound frame through Windows.Graphics.Capture.</summary>
internal sealed class WindowsGraphicsCaptureService
{
    private static readonly Guid GraphicsCaptureItemGuid = new("79C3F95B-31F7-4EC2-A464-632EF5D30760");

    /// <summary>Gets the last bounded-capture failure for broker diagnostics.</summary>
    public string? LastFailure { get; private set; }

    /// <summary>Attempts a bounded WGC capture and returns null when the platform path is unavailable.</summary>
    public async Task<byte[]?> TryCapturePngAsync(IntPtr windowHandle, TimeSpan timeout)
    {
        LastFailure = null;
        if (!GraphicsCaptureSession.IsSupported())
        {
            LastFailure = "Windows Graphics Capture is not supported on this Windows installation.";
            return null;
        }

        try
        {
            var item = CreateItemForWindow(windowHandle);
            var learningDevice = new LearningModelDevice(LearningModelDeviceKind.DirectX);
            using var framePool = Direct3D11CaptureFramePool.CreateFreeThreaded(
                learningDevice.Direct3D11Device,
                DirectXPixelFormat.B8G8R8A8UIntNormalized,
                1,
                item.Size);
            using var session = framePool.CreateCaptureSession(item);
            var frameCompletion = new TaskCompletionSource<Direct3D11CaptureFrame>(
                TaskCreationOptions.RunContinuationsAsynchronously);

            void OnFrameArrived(Direct3D11CaptureFramePool sender, object arguments)
            {
                _ = arguments;
                var frame = sender.TryGetNextFrame();
                if (!frameCompletion.TrySetResult(frame))
                {
                    frame.Dispose();
                }
            }

            framePool.FrameArrived += OnFrameArrived;
            try
            {
                session.StartCapture();
                using var frame = await frameCompletion.Task.WaitAsync(timeout).ConfigureAwait(false);
                using var softwareBitmap = await SoftwareBitmap.CreateCopyFromSurfaceAsync(frame.Surface);
                return await EncodePngAsync(softwareBitmap).ConfigureAwait(false);
            }
            finally
            {
                framePool.FrameArrived -= OnFrameArrived;
            }
        }
        catch (Exception exception) when (exception is COMException or TimeoutException or InvalidOperationException or
                                          NotSupportedException or ArgumentException or ObjectDisposedException)
        {
            LastFailure = $"{exception.GetType().Name}: {exception.Message} (0x{exception.HResult:X8})";
            return null;
        }
    }

    /// <summary>Creates a capture item for one already-allowlisted top-level window.</summary>
    private static GraphicsCaptureItem CreateItemForWindow(IntPtr windowHandle)
    {
        var interop = GraphicsCaptureItem.As<IGraphicsCaptureItemInterop>();
        var itemPointer = interop.CreateForWindow(windowHandle, GraphicsCaptureItemGuid);
        try
        {
            return GraphicsCaptureItem.FromAbi(itemPointer);
        }
        finally
        {
            _ = Marshal.Release(itemPointer);
        }
    }

    /// <summary>Encodes one software bitmap into a detached PNG byte array.</summary>
    private static async Task<byte[]> EncodePngAsync(SoftwareBitmap bitmap)
    {
        using var stream = new InMemoryRandomAccessStream();
        var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
        encoder.SetSoftwareBitmap(bitmap);
        await encoder.FlushAsync();

        stream.Seek(0);
        if (stream.Size > int.MaxValue)
        {
            throw new InvalidOperationException("The captured PNG exceeds the supported in-memory size.");
        }
        using var reader = new DataReader(stream.GetInputStreamAt(0));
        _ = await reader.LoadAsync((uint)stream.Size);
        var bytes = new byte[(int)stream.Size];
        reader.ReadBytes(bytes);
        return bytes;
    }

    /// <summary>Defines the official Win32-to-WinRT GraphicsCaptureItem factory interface.</summary>
    [ComImport]
    [Guid("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IGraphicsCaptureItemInterop
    {
        /// <summary>Creates a GraphicsCaptureItem for one HWND.</summary>
        IntPtr CreateForWindow([In] IntPtr windowHandle, in Guid interfaceId);

        /// <summary>Creates a GraphicsCaptureItem for one monitor handle.</summary>
        IntPtr CreateForMonitor([In] IntPtr monitorHandle, in Guid interfaceId);
    }
}
