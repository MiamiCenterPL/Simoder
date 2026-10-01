using System.Text.Json;
using System.Text.Json.Serialization;

namespace Simoder.DevBridge.Core;

/// <summary>Identifies the requested path through the SimCity front end.</summary>
[JsonConverter(typeof(JsonStringEnumConverter<LaunchMode>))]
public enum LaunchMode
{
    Continue,
    Play,
}

/// <summary>Describes every externally meaningful startup stage.</summary>
[JsonConverter(typeof(JsonStringEnumConverter<StartupStage>))]
public enum StartupStage
{
    Idle,
    EaStarting,
    EaAdministratorNotice,
    AwaitingUac,
    LauncherPlay,
    GameStarting,
    Intro,
    Welcome,
    MainMenu,
    SaveSelection,
    CityLoading,
    InCity,
    HumanActionRequired,
    SuspectedInfiniteLoading,
    Failed,
}

/// <summary>Identifies how the current stage was determined.</summary>
[JsonConverter(typeof(JsonStringEnumConverter<StageSource>))]
public enum StageSource
{
    NativeObservation,
    AiConfirmed,
    Timeout,
    Internal,
}

/// <summary>Lists the only keyboard keys accepted by the broker.</summary>
[JsonConverter(typeof(JsonStringEnumConverter<AllowedKey>))]
public enum AllowedKey
{
    Escape,
    Enter,
    Space,
}

/// <summary>Lists commands carried by the private named-pipe protocol.</summary>
[JsonConverter(typeof(JsonStringEnumConverter<BrokerCommand>))]
public enum BrokerCommand
{
    Ping,
    StartSession,
    GetState,
    CaptureWindow,
    Click,
    SendKey,
    ConfirmStage,
    WaitForStage,
    GetDiagnostics,
    GetRuntimeEvidence,
    GetModLogs,
}

/// <summary>Defines the explicit destination selected for a startup session.</summary>
public sealed record LaunchTarget(
    LaunchMode Mode,
    string? RegionName = null,
    string? CityName = null);

/// <summary>Describes a process observed by the elevated broker.</summary>
public sealed record ProcessSnapshot(
    int ProcessId,
    string Name,
    string ExecutablePath,
    int SessionId,
    bool IsAllowed);

/// <summary>Describes a top-level window owned by an allowlisted process.</summary>
public sealed record WindowSnapshot(
    long Handle,
    int ProcessId,
    string ProcessName,
    string Title,
    int Left,
    int Top,
    int Width,
    int Height,
    int ClientLeft,
    int ClientTop,
    int ClientWidth,
    int ClientHeight,
    bool IsForeground,
    bool IsAllowed);

/// <summary>Describes an accessible control discovered inside a target window.</summary>
public sealed record AccessibleControlSnapshot(
    string Name,
    string AutomationId,
    string ControlType,
    int Left,
    int Top,
    int Width,
    int Height,
    bool IsEnabled);

/// <summary>Represents an immutable, short-lived window capture.</summary>
public sealed record CaptureSnapshot(
    string CaptureId,
    DateTimeOffset CapturedAtUtc,
    WindowSnapshot Window,
    string PngBase64,
    IReadOnlyList<AccessibleControlSnapshot> AccessibleControls);

/// <summary>Records a transition or noteworthy observation for diagnostics.</summary>
public sealed record TimelineEvent(
    DateTimeOffset TimestampUtc,
    StartupStage Stage,
    StageSource Source,
    string Message);

/// <summary>Represents the complete public state of one startup workflow.</summary>
public sealed record SessionState(
    string? SessionId,
    LaunchTarget? Target,
    StartupStage Stage,
    StageSource Source,
    double Confidence,
    string? HumanAction,
    DateTimeOffset UpdatedAtUtc,
    IReadOnlyList<ProcessSnapshot> Processes,
    IReadOnlyList<WindowSnapshot> Windows,
    IReadOnlyList<TimelineEvent> Timeline);

/// <summary>Contains a bounded diagnostic snapshot without changing external state.</summary>
public sealed record DiagnosticSnapshot(
    SessionState State,
    IReadOnlyList<string> OverlayModules,
    IReadOnlyList<string> RecentLogLines,
    CaptureSnapshot? LastCapture,
    DateTimeOffset GeneratedAtUtc);

/// <summary>Requests creation of a startup session.</summary>
public sealed record StartSessionRequest(LaunchTarget Target);
/// <summary>@summary Selects a bounded fixed-log tail without accepting filesystem paths.</summary>
public sealed record ModLogsRequest(string? ModId, int Limit = 80);

/// <summary>Requests capture of an allowlisted EA or SimCity window.</summary>
public sealed record CaptureWindowRequest(string Target = "auto");

/// <summary>Requests one coordinate-bound click against a fresh capture.</summary>
public sealed record ClickRequest(string CaptureId, double NormalizedX, double NormalizedY);

/// <summary>Requests one allowlisted key press against an allowlisted active window.</summary>
public sealed record SendKeyRequest(string Target, AllowedKey Key);

/// <summary>Associates an AI-observed stage with the exact capture used to infer it.</summary>
public sealed record ConfirmStageRequest(string CaptureId, StartupStage Stage, double Confidence, string? Note);

/// <summary>Waits for one stage with a bounded timeout.</summary>
public sealed record WaitForStageRequest(StartupStage Stage, int TimeoutSeconds);

/// <summary>Provides one request frame for the private named-pipe protocol.</summary>
public sealed record BrokerRequest(string RequestId, BrokerCommand Command, JsonElement Payload);

/// <summary>Provides one response frame for the private named-pipe protocol.</summary>
public sealed record BrokerResponse(string RequestId, bool Success, JsonElement? Data, string? ErrorCode, string? ErrorMessage);

/// <summary>Centralizes strict JSON settings shared by both ends of the private protocol.</summary>
public static class JsonDefaults
{
    /// <summary>Gets deterministic camel-case serialization options.</summary>
    public static JsonSerializerOptions Options { get; } = CreateOptions();

    /// <summary>Creates the immutable application-wide JSON settings.</summary>
    private static JsonSerializerOptions CreateOptions()
    {
        var options = new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            PropertyNameCaseInsensitive = true,
            WriteIndented = false,
        };
        options.Converters.Add(new JsonStringEnumConverter(JsonNamingPolicy.CamelCase));
        return options;
    }
}
