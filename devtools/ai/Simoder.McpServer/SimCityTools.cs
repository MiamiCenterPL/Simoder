using System.ComponentModel;
using System.Text.Json;
using ModelContextProtocol.Protocol;
using ModelContextProtocol.Server;
using Simoder.DevBridge.Core;

namespace Simoder.McpServer;

/// <summary>Exposes the allowlisted SimCity startup workflow as MCP tools.</summary>
[McpServerToolType]
public sealed class SimCityTools
{
    /// <summary>Provides cross-tool safety and workflow guidance to MCP clients.</summary>
    public const string ServerInstructions =
        "Simoder DevBridge controls only verified EA App and SimCity windows. Start with simcity_start_session, approve UAC manually, then alternate state/capture with capture-bound click or allowlisted keys. Confirm visually inferred stages with the exact capture_id. Never claim the city loaded until in_city is confirmed. Suspected Infinite Loading is diagnostic only: do not terminate or restart the game.";

    private readonly DevBridgeClient client_;

    /// <summary>Creates the tool collection with its shared broker client.</summary>
    public SimCityTools(DevBridgeClient client)
    {
        client_ = client;
    }

    /// <summary>Starts a new EA-to-city workflow and may display one Windows UAC prompt.</summary>
    [McpServerTool(Name = "simcity_start_session", Destructive = false, ReadOnly = false, Idempotent = false, OpenWorld = false, UseStructuredContent = true)]
    [Description("Start an elevated Simoder startup session. mode is continue, or play with exact visible regionName and cityName. This may show a manual Windows UAC prompt.")]
    public Task<SessionState> StartSessionAsync(
        [Description("Startup mode: continue or play.")] LaunchMode mode,
        [Description("Exact visible region name; required only for play mode.")] string? regionName = null,
        [Description("Exact visible city name; required only for play mode.")] string? cityName = null,
        CancellationToken cancellationToken = default) =>
        client_.StartSessionAsync(new LaunchTarget(mode, regionName, cityName), cancellationToken);

    /// <summary>Returns process, window, stage, confidence, and timeline state.</summary>
    [McpServerTool(Name = "simcity_get_startup_state", Destructive = false, ReadOnly = true, Idempotent = true, OpenWorld = false, UseStructuredContent = true)]
    [Description("Read the current SimCity startup state without launching, clicking, typing, terminating, or restarting anything.")]
    public Task<SessionState> GetStartupStateAsync(CancellationToken cancellationToken = default) =>
        client_.GetStateAsync(cancellationToken);

    /// <summary>Captures one allowlisted EA or SimCity window.</summary>
    [McpServerTool(Name = "simcity_capture_window", Destructive = false, ReadOnly = true, Idempotent = true, OpenWorld = false, UseStructuredContent = true, OutputSchemaType = typeof(CaptureToolResultMetadata))]
    [Description("Capture an allowlisted EA or SimCity window. Returns a native PNG image block plus capture_id, geometry, and accessible controls; use the image and capture_id before clicking.")]
    public async Task<CallToolResult> CaptureWindowAsync(
        [Description("Target window: auto, ea, or simcity.")] string target = "auto",
        CancellationToken cancellationToken = default)
    {
        var capture = await client_.CaptureAsync(target, cancellationToken).ConfigureAwait(false);
        return CaptureResultFactory.Create(capture);
    }

    /// <summary>Clicks one normalized point from an exact fresh capture.</summary>
    [McpServerTool(Name = "simcity_click", Destructive = false, ReadOnly = false, Idempotent = false, OpenWorld = false, UseStructuredContent = true)]
    [Description("Click a normalized point in an allowlisted window using an exact capture_id no older than ten seconds. Rejects stale or changed windows.")]
    public Task<System.Text.Json.JsonElement> ClickAsync(
        [Description("Fresh identifier returned by simcity_capture_window.")] string captureId,
        [Description("Horizontal coordinate from 0.0 at left to 1.0 at right.")] double normalizedX,
        [Description("Vertical coordinate from 0.0 at top to 1.0 at bottom.")] double normalizedY,
        CancellationToken cancellationToken = default) =>
        client_.ClickAsync(new ClickRequest(captureId, normalizedX, normalizedY), cancellationToken);

    /// <summary>Sends one fixed allowlisted key to an allowlisted foreground target.</summary>
    [McpServerTool(Name = "simcity_send_key", Destructive = false, ReadOnly = false, Idempotent = false, OpenWorld = false, UseStructuredContent = true)]
    [Description("Send exactly one Escape, Enter, or Space key to an allowlisted EA or SimCity window. Arbitrary text and virtual keys are not accepted.")]
    public Task<System.Text.Json.JsonElement> SendKeyAsync(
        [Description("Target window: auto, ea, or simcity.")] string target,
        [Description("Allowlisted key: escape, enter, or space.")] AllowedKey key,
        CancellationToken cancellationToken = default) =>
        client_.SendKeyAsync(new SendKeyRequest(target, key), cancellationToken);

    /// <summary>Records a stage inferred by AI from one fresh screenshot.</summary>
    [McpServerTool(Name = "simcity_confirm_stage", Destructive = false, ReadOnly = true, Idempotent = true, OpenWorld = false, UseStructuredContent = true)]
    [Description("Confirm a visually observed startup stage using the exact fresh capture_id that supports the inference.")]
    public Task<SessionState> ConfirmStageAsync(
        [Description("Fresh identifier returned by simcity_capture_window.")] string captureId,
        [Description("Observed startup stage.")] StartupStage stage,
        [Description("Confidence between 0.0 and 1.0.")] double confidence,
        [Description("Short evidence note, including visible labels or spinner state.")] string? note = null,
        CancellationToken cancellationToken = default) =>
        client_.ConfirmStageAsync(new ConfirmStageRequest(captureId, stage, confidence, note), cancellationToken);

    /// <summary>Waits for a target or terminal diagnostic stage.</summary>
    [McpServerTool(Name = "simcity_wait_for_stage", Destructive = false, ReadOnly = true, Idempotent = true, OpenWorld = false, UseStructuredContent = true)]
    [Description("Wait between 1 and 300 seconds for a target stage, returning early for failure, required human action, or suspected Infinite Loading.")]
    public Task<SessionState> WaitForStageAsync(
        [Description("Startup stage to wait for.")] StartupStage stage,
        [Description("Bounded timeout in seconds, from 1 through 300.")] int timeoutSeconds,
        CancellationToken cancellationToken = default) =>
        client_.WaitForStageAsync(new WaitForStageRequest(stage, timeoutSeconds), cancellationToken);

    /// <summary>Returns bounded logs and overlay evidence without changing game state.</summary>
    [McpServerTool(Name = "simcity_get_diagnostics", Destructive = false, ReadOnly = true, Idempotent = true, OpenWorld = false, UseStructuredContent = true, OutputSchemaType = typeof(DiagnosticToolResultMetadata))]
    [Description("Collect bounded startup timeline, focus/window state, overlay module names, and recent EA/Simoder log tails. Never terminates or restarts the game.")]
    public async Task<CallToolResult> GetDiagnosticsAsync(CancellationToken cancellationToken = default)
    {
        var diagnostics = await client_.GetDiagnosticsAsync(cancellationToken).ConfigureAwait(false);
        return DiagnosticsResultFactory.Create(diagnostics);
    }
}

/// <summary>Describes the structured evidence paired with a native MCP image block.</summary>
public sealed record CaptureToolResultMetadata(
    string CaptureId,
    DateTimeOffset CapturedAtUtc,
    WindowSnapshot Window,
    IReadOnlyList<AccessibleControlSnapshot> AccessibleControls);

/// <summary>Describes diagnostics without duplicating the last screenshot as structured base64.</summary>
public sealed record DiagnosticToolResultMetadata(
    SessionState State,
    IReadOnlyList<string> OverlayModules,
    IReadOnlyList<string> RecentLogLines,
    CaptureToolResultMetadata? LastCapture,
    DateTimeOffset GeneratedAtUtc);

/// <summary>Converts the private broker capture into safe public MCP content.</summary>
public static class CaptureResultFactory
{
    /// <summary>Returns metadata, a concise text summary, and a native PNG content block.</summary>
    public static CallToolResult Create(CaptureSnapshot capture)
    {
        ArgumentNullException.ThrowIfNull(capture);
        var metadata = new CaptureToolResultMetadata(
            capture.CaptureId, capture.CapturedAtUtc, capture.Window, capture.AccessibleControls);
        var structured = JsonSerializer.SerializeToElement(metadata, JsonDefaults.Options);
        return new CallToolResult
        {
            Content =
            [
                new TextContentBlock
                {
                    Text = JsonSerializer.Serialize(metadata, JsonDefaults.Options),
                },
                ImageContentBlock.FromBytes(Convert.FromBase64String(capture.PngBase64), "image/png"),
            ],
            StructuredContent = structured,
            IsError = false,
        };
    }
}

/// <summary>Converts private diagnostic data into structured metadata and an optional native image.</summary>
public static class DiagnosticsResultFactory
{
    /// <summary>Returns bounded metadata plus the most recent PNG when one exists.</summary>
    public static CallToolResult Create(DiagnosticSnapshot diagnostics)
    {
        ArgumentNullException.ThrowIfNull(diagnostics);
        var lastCapture = diagnostics.LastCapture is null
            ? null
            : new CaptureToolResultMetadata(
                diagnostics.LastCapture.CaptureId,
                diagnostics.LastCapture.CapturedAtUtc,
                diagnostics.LastCapture.Window,
                diagnostics.LastCapture.AccessibleControls);
        var metadata = new DiagnosticToolResultMetadata(
            diagnostics.State,
            diagnostics.OverlayModules,
            diagnostics.RecentLogLines,
            lastCapture,
            diagnostics.GeneratedAtUtc);
        var content = new List<ContentBlock>
        {
            new TextContentBlock { Text = JsonSerializer.Serialize(metadata, JsonDefaults.Options) },
        };
        if (diagnostics.LastCapture is not null)
        {
            content.Add(ImageContentBlock.FromBytes(
                Convert.FromBase64String(diagnostics.LastCapture.PngBase64), "image/png"));
        }

        return new CallToolResult
        {
            Content = content,
            StructuredContent = JsonSerializer.SerializeToElement(metadata, JsonDefaults.Options),
            IsError = false,
        };
    }
}
