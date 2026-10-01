namespace Simoder.DevBridge.Core;

/// <summary>Evaluates named-pipe client identity without weakening Windows ACL enforcement.</summary>
public static class ClientAuthorization
{
    /// <summary>Returns true only for an exact executable path in the expected Windows session.</summary>
    public static bool IsAllowed(string expectedPath, string actualPath, int expectedSessionId, int actualSessionId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(expectedPath);
        ArgumentException.ThrowIfNullOrWhiteSpace(actualPath);
        return expectedSessionId == actualSessionId &&
               Path.GetFullPath(expectedPath).Equals(Path.GetFullPath(actualPath), StringComparison.OrdinalIgnoreCase);
    }
}
