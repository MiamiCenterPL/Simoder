using System.Diagnostics;
using System.Text.Json;
using Simoder.DevBridge.Core;

namespace Simoder.McpServer;

/// <summary>Connects the MCP tool layer to the elevated, same-user DevBridge process.</summary>
public sealed class DevBridgeClient
{
    private readonly BrokerPipeClient pipeClient_ = new();
    private readonly SemaphoreSlim brokerLaunchGate_ = new(1, 1);
    private readonly string gameRoot_;
    private readonly string brokerPath_;

    /// <summary>Resolves the installed sibling broker and game root.</summary>
    public DevBridgeClient()
    {
        brokerPath_ = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "Simoder.DevBridge.exe"));
        gameRoot_ = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", ".."));
    }

    /// <summary>Starts or reuses the elevated broker and creates a startup session.</summary>
    public async Task<SessionState> StartSessionAsync(LaunchTarget target, CancellationToken cancellationToken)
    {
        await EnsureBrokerAsync(cancellationToken).ConfigureAwait(false);
        return await CallAsync<StartSessionRequest, SessionState>(
            BrokerCommand.StartSession, new StartSessionRequest(target), TimeSpan.FromSeconds(30), cancellationToken)
            .ConfigureAwait(false);
    }

    /// <summary>Reads current state without causing a UAC prompt.</summary>
    public Task<SessionState> GetStateAsync(CancellationToken cancellationToken) =>
        CallAsync<object, SessionState>(BrokerCommand.GetState, new { }, TimeSpan.FromSeconds(5), cancellationToken);

    /// <summary>Captures an allowlisted target without causing a UAC prompt.</summary>
    public Task<CaptureSnapshot> CaptureAsync(string target, CancellationToken cancellationToken) =>
        CallAsync<CaptureWindowRequest, CaptureSnapshot>(BrokerCommand.CaptureWindow,
            new CaptureWindowRequest(target), TimeSpan.FromSeconds(15), cancellationToken);

    /// <summary>Sends one capture-bound click.</summary>
    public Task<JsonElement> ClickAsync(ClickRequest request, CancellationToken cancellationToken) =>
        CallAsync<ClickRequest, JsonElement>(BrokerCommand.Click, request, TimeSpan.FromSeconds(10), cancellationToken);

    /// <summary>Sends one allowlisted key.</summary>
    public Task<JsonElement> SendKeyAsync(SendKeyRequest request, CancellationToken cancellationToken) =>
        CallAsync<SendKeyRequest, JsonElement>(BrokerCommand.SendKey, request, TimeSpan.FromSeconds(10), cancellationToken);

    /// <summary>Records an AI-confirmed stage backed by a fresh capture.</summary>
    public Task<SessionState> ConfirmStageAsync(ConfirmStageRequest request, CancellationToken cancellationToken) =>
        CallAsync<ConfirmStageRequest, SessionState>(BrokerCommand.ConfirmStage, request, TimeSpan.FromSeconds(10), cancellationToken);

    /// <summary>Waits for one stage with a server-side bounded timeout.</summary>
    public Task<SessionState> WaitForStageAsync(WaitForStageRequest request, CancellationToken cancellationToken) =>
        CallAsync<WaitForStageRequest, SessionState>(BrokerCommand.WaitForStage, request,
            TimeSpan.FromSeconds(Math.Clamp(request.TimeoutSeconds + 5, 6, 305)), cancellationToken);

    /// <summary>Reads a bounded diagnostic bundle without restarting or terminating anything.</summary>
    public Task<DiagnosticSnapshot> GetDiagnosticsAsync(CancellationToken cancellationToken) =>
        CallAsync<object, DiagnosticSnapshot>(BrokerCommand.GetDiagnostics, new { }, TimeSpan.FromSeconds(15), cancellationToken);

    /// <summary>Calls the private pipe and gives disconnected read tools a clear recovery instruction.</summary>
    private async Task<TResponse> CallAsync<TRequest, TResponse>(
        BrokerCommand command,
        TRequest request,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        try
        {
            return await pipeClient_.CallAsync<TRequest, TResponse>(command, request, timeout, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (Exception exception) when (exception is TimeoutException or IOException)
        {
            throw new InvalidOperationException(
                "DevBridge is not running. Call simcity_start_session and approve its Windows UAC prompt first.", exception);
        }
    }

    /// <summary>Prompts once for the elevated broker and waits until its restricted pipe is ready.</summary>
    private async Task EnsureBrokerAsync(CancellationToken cancellationToken)
    {
        await brokerLaunchGate_.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (await IsBrokerReadyAsync(cancellationToken).ConfigureAwait(false))
            {
                return;
            }

            if (!File.Exists(brokerPath_))
            {
                throw new FileNotFoundException("The optional Simoder.DevBridge executable is not installed.", brokerPath_);
            }

            try
            {
                using var process = Process.Start(new ProcessStartInfo
                {
                    FileName = brokerPath_,
                    Arguments = $"--game-root \"{gameRoot_}\"",
                    UseShellExecute = true,
                    Verb = "runas",
                    WorkingDirectory = AppContext.BaseDirectory,
                    WindowStyle = ProcessWindowStyle.Hidden,
                }) ?? throw new InvalidOperationException("Windows did not start DevBridge.");
            }
            catch (System.ComponentModel.Win32Exception exception) when (exception.NativeErrorCode == 1223)
            {
                throw new InvalidOperationException(
                    "human_action_required: Windows UAC was cancelled; approve it manually to start DevBridge.", exception);
            }

            var deadline = DateTimeOffset.UtcNow.AddSeconds(45);
            while (DateTimeOffset.UtcNow < deadline)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (await IsBrokerReadyAsync(cancellationToken).ConfigureAwait(false))
                {
                    return;
                }
                await Task.Delay(TimeSpan.FromMilliseconds(250), cancellationToken).ConfigureAwait(false);
            }
            throw new TimeoutException("DevBridge did not become ready within 45 seconds after UAC approval.");
        }
        finally
        {
            brokerLaunchGate_.Release();
        }
    }

    /// <summary>Performs a short non-mutating readiness probe.</summary>
    private async Task<bool> IsBrokerReadyAsync(CancellationToken cancellationToken)
    {
        try
        {
            _ = await pipeClient_.CallAsync<object, JsonElement>(
                BrokerCommand.Ping, new { }, TimeSpan.FromMilliseconds(300), cancellationToken).ConfigureAwait(false);
            return true;
        }
        catch (Exception exception) when (exception is TimeoutException or IOException or OperationCanceledException)
        {
            return false;
        }
    }
}
