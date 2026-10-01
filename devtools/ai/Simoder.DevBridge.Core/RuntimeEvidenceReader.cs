using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Simoder.DevBridge.Core;

/// <summary>@summary Reads bounded Simoder evidence from fixed installation paths.</summary>
public static class RuntimeEvidenceReader
{
    /// <summary>@summary Returns explicit availability/freshness and preserves historical resource semantics.</summary>
    public static JsonElement Read(string gameRoot, IReadOnlySet<int> allowedPids, DateTimeOffset now)
    {
        try
        {
            var path = FixedPath(gameRoot, "runtime-status.json");
            if (!File.Exists(path)) return Result(false, false, "Runtime snapshot unavailable", null, now);
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (stream.Length > 1024 * 1024) throw new InvalidDataException("Runtime snapshot exceeds 1 MiB");
            using var document = JsonDocument.Parse(stream, new JsonDocumentOptions { MaxDepth = 32 });
            var root = document.RootElement;
            if (root.GetProperty("schemaVersion").GetInt32() != 1) throw new InvalidDataException("Unsupported snapshot schema");
            var pid = root.GetProperty("pid").GetInt32();
            var timestamp = DateTimeOffset.FromUnixTimeMilliseconds(root.GetProperty("generatedAtUnixMs").GetInt64());
            var age = now - timestamp;
            var fresh = age >= TimeSpan.Zero && age <= TimeSpan.FromSeconds(5) && allowedPids.Contains(pid)
                && root.GetProperty("consistentGeneration").GetBoolean();
            return Result(true, fresh, fresh ? "Historical loader evidence; gameplay effects unverified" : "Snapshot stale, inconsistent, or process unverified", root, now);
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException or
            InvalidOperationException or KeyNotFoundException or ArgumentOutOfRangeException or FormatException)
        {
            return Result(false, false, exception.Message, null, now);
        }
    }

    /// <summary>@summary Reads a fixed log tail and optionally filters one validated mod ID.</summary>
    public static IReadOnlyList<string> ReadLogs(string gameRoot, string? modId, int limit)
    {
        if (limit is < 1 or > 200) throw new ArgumentOutOfRangeException(nameof(limit));
        if (modId is not null && (modId.Length > 128 || !Regex.IsMatch(modId, @"^[a-z0-9][a-z0-9._-]*$")))
            throw new ArgumentException("Invalid mod ID", nameof(modId));
        var path = FixedPath(gameRoot, "sc13modloader.log");
        if (!File.Exists(path)) return [];
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        var truncated = stream.Length > 65536;
        stream.Seek(Math.Max(0, stream.Length - 65536), SeekOrigin.Begin);
        using var reader = new StreamReader(stream, Encoding.UTF8);
        var lines = reader.ReadToEnd().Split('\n');
        return lines.Skip(truncated ? 1 : 0).Where(line => line.Length != 0 &&
            (modId is null || line.Contains($"[MOD] [{modId}]", StringComparison.Ordinal)))
            .TakeLast(limit).ToArray();
    }

    /// <summary>@summary Rejects reparse-point paths for the fixed runtime evidence files.</summary>
    private static string FixedPath(string gameRoot, string name)
    {
        var root = Path.GetFullPath(gameRoot);
        var path = Path.Combine(root, "simoder", "logs", name);
        for (var current = path; current.Length >= root.Length; current = Path.GetDirectoryName(current) ?? "")
        {
            if ((File.Exists(current) || Directory.Exists(current)) &&
                (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Runtime evidence path uses a reparse point");
            if (current.Equals(root, StringComparison.OrdinalIgnoreCase)) break;
        }
        return path;
    }

    /// <summary>@summary Creates a JSON envelope that never labels unavailable evidence as current.</summary>
    private static JsonElement Result(bool available, bool fresh, string message, JsonElement? snapshot, DateTimeOffset now) =>
        JsonSerializer.SerializeToElement(new { available, fresh, message, snapshot, readAtUtc = now }, JsonDefaults.Options);
}
