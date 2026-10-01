using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;

namespace Simoder.DevBridge.Core;

/// <summary>Derives a non-secret per-user and per-Windows-session pipe name.</summary>
public static class PipeNaming
{
    /// <summary>Returns the deterministic pipe name used by the broker and MCP process.</summary>
    public static string GetCurrentPipeName()
    {
        using var identity = WindowsIdentity.GetCurrent();
        var sid = identity.User?.Value ?? throw new InvalidOperationException("The current Windows SID is unavailable.");
        var sessionId = Environment.ProcessId > 0
            ? System.Diagnostics.Process.GetCurrentProcess().SessionId
            : 0;
        var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(sid))).ToLowerInvariant()[..16];
        return $"Simoder.DevBridge.v1.{sessionId}.{hash}";
    }
}
