using System.Text;

namespace Simoder.DevBridge;

/// <summary>Writes bounded diagnostic messages to the optional DevTools log directory.</summary>
internal sealed class BrokerLogger
{
    private readonly object gate_ = new();
    private readonly string logPath_;

    /// <summary>Creates a logger and ensures its private directory exists.</summary>
    public BrokerLogger(string logDirectory)
    {
        Directory.CreateDirectory(logDirectory);
        logPath_ = Path.Combine(logDirectory, "devbridge.log");
    }

    /// <summary>Appends one timestamped UTF-8 log entry.</summary>
    public void Write(string message)
    {
        var safeMessage = message.Replace("\r", " ", StringComparison.Ordinal)
            .Replace("\n", " ", StringComparison.Ordinal);
        var line = $"{DateTimeOffset.UtcNow:O} {safeMessage}{Environment.NewLine}";
        lock (gate_)
        {
            File.AppendAllText(logPath_, line, new UTF8Encoding(false));
        }
    }
}
