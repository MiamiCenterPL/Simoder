namespace Simoder.DevBridge.Core;

/// <summary>Protects input operations from stale or geometrically changed captures.</summary>
public static class CaptureGuard
{
    /// <summary>Defines how long a capture can authorize an input action.</summary>
    public static readonly TimeSpan MaximumAge = TimeSpan.FromSeconds(10);

    /// <summary>Validates capture freshness, geometry, and normalized coordinates.</summary>
    /// <exception cref="InvalidOperationException">Thrown when the capture can no longer authorize input.</exception>
    public static void Validate(
        CaptureSnapshot capture,
        WindowSnapshot currentWindow,
        DateTimeOffset nowUtc,
        double normalizedX,
        double normalizedY)
    {
        ArgumentNullException.ThrowIfNull(capture);
        ArgumentNullException.ThrowIfNull(currentWindow);

        if (nowUtc - capture.CapturedAtUtc > MaximumAge)
        {
            throw new InvalidOperationException("The capture is stale; capture the window again before sending input.");
        }

        if (!double.IsFinite(normalizedX) || !double.IsFinite(normalizedY) ||
            normalizedX is < 0 or > 1 || normalizedY is < 0 or > 1)
        {
            throw new InvalidOperationException("Normalized click coordinates must be finite values between 0 and 1.");
        }

        var expected = capture.Window;
        if (expected.Handle != currentWindow.Handle ||
            expected.Left != currentWindow.Left || expected.Top != currentWindow.Top ||
            expected.Width != currentWindow.Width || expected.Height != currentWindow.Height ||
            expected.ClientLeft != currentWindow.ClientLeft || expected.ClientTop != currentWindow.ClientTop ||
            expected.ClientWidth != currentWindow.ClientWidth || expected.ClientHeight != currentWindow.ClientHeight)
        {
            throw new InvalidOperationException("The target window changed after capture; capture it again before sending input.");
        }

        if (!currentWindow.IsAllowed || currentWindow.Width <= 0 || currentWindow.Height <= 0)
        {
            throw new InvalidOperationException("The capture does not identify an allowlisted usable window.");
        }

        if (!currentWindow.IsForeground)
        {
            throw new InvalidOperationException("The captured target is no longer the active foreground window.");
        }
    }

    /// <summary>Returns the validated screen point represented by outer-window normalized coordinates.</summary>
    /// <exception cref="InvalidOperationException">Thrown when the point is outside the unchanged client area.</exception>
    public static (int X, int Y) GetAuthorizedScreenPoint(
        CaptureSnapshot capture,
        WindowSnapshot currentWindow,
        DateTimeOffset nowUtc,
        double normalizedX,
        double normalizedY)
    {
        Validate(capture, currentWindow, nowUtc, normalizedX, normalizedY);
        var screenX = currentWindow.Left + Math.Clamp(
            (int)Math.Round(normalizedX * Math.Max(0, currentWindow.Width - 1)), 0, currentWindow.Width - 1);
        var screenY = currentWindow.Top + Math.Clamp(
            (int)Math.Round(normalizedY * Math.Max(0, currentWindow.Height - 1)), 0, currentWindow.Height - 1);
        if (screenX < currentWindow.ClientLeft || screenY < currentWindow.ClientTop ||
            screenX >= currentWindow.ClientLeft + currentWindow.ClientWidth ||
            screenY >= currentWindow.ClientTop + currentWindow.ClientHeight)
        {
            throw new InvalidOperationException("The requested point is outside the validated client area.");
        }

        return (screenX, screenY);
    }
}
