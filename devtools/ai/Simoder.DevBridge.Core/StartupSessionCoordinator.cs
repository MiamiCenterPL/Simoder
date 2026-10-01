namespace Simoder.DevBridge.Core;

/// <summary>Maintains the bounded, auditable startup state machine.</summary>
public sealed class StartupSessionCoordinator
{
    private static readonly TimeSpan InfiniteLoadingThreshold = TimeSpan.FromMinutes(2);
    private readonly object gate_ = new();
    private readonly List<TimelineEvent> timeline_ = [];
    private string? sessionId_;
    private LaunchTarget? target_;
    private StartupStage stage_ = StartupStage.Idle;
    private StageSource source_ = StageSource.Internal;
    private double confidence_ = 1;
    private string? humanAction_;
    private DateTimeOffset updatedAtUtc_ = DateTimeOffset.UtcNow;

    /// <summary>Starts a new workflow after validating its explicit destination.</summary>
    public SessionState Start(LaunchTarget requestedTarget, DateTimeOffset nowUtc)
    {
        lock (gate_)
        {
            target_ = LaunchTargetValidator.Validate(requestedTarget);
            sessionId_ = Guid.NewGuid().ToString("N");
            timeline_.Clear();
            TransitionLocked(StartupStage.EaStarting, StageSource.Internal, 1, null,
                "Startup session created and delegated to the EA App.", nowUtc);
            return SnapshotLocked([], []);
        }
    }

    /// <summary>Records a native process/window observation without overriding AI-confirmed visual stages.</summary>
    public SessionState Observe(
        IReadOnlyList<ProcessSnapshot> processes,
        IReadOnlyList<WindowSnapshot> windows,
        DateTimeOffset nowUtc)
    {
        lock (gate_)
        {
            if (sessionId_ is null)
            {
                return SnapshotLocked(processes, windows);
            }

            var simCityRunning = processes.Any(process =>
                process.IsAllowed && process.Name.Equals("SimCity", StringComparison.OrdinalIgnoreCase));
            var eaRunning = processes.Any(process =>
                process.IsAllowed && (process.Name.Equals("EADesktop", StringComparison.OrdinalIgnoreCase) ||
                                      process.Name.Equals("EALaunchHelper", StringComparison.OrdinalIgnoreCase)));

            if (!simCityRunning && !eaRunning && stage_ is not StartupStage.Failed &&
                (stage_ != StartupStage.EaStarting || nowUtc - updatedAtUtc_ >= TimeSpan.FromSeconds(15)))
            {
                TransitionLocked(StartupStage.HumanActionRequired, StageSource.NativeObservation, 0.9,
                    "EA App and SimCity are not running. Start or repair EA App, then retry.",
                    "No allowlisted EA or SimCity process is running.", nowUtc);
            }
            else if (simCityRunning && stage_ is StartupStage.EaStarting or StartupStage.EaAdministratorNotice or StartupStage.AwaitingUac or StartupStage.LauncherPlay)
            {
                TransitionLocked(StartupStage.GameStarting, StageSource.NativeObservation, 0.95, null,
                    "SimCity process detected.", nowUtc);
            }
            else if (stage_ == StartupStage.CityLoading && nowUtc - updatedAtUtc_ >= InfiniteLoadingThreshold)
            {
                TransitionLocked(StartupStage.SuspectedInfiniteLoading, StageSource.Timeout, 0.8,
                    "Loading has not reached an AI-confirmed in-city state for two minutes. Inspect diagnostics; the broker will not restart the game.",
                    "City loading exceeded the diagnostic threshold.", nowUtc);
            }

            return SnapshotLocked(processes, windows);
        }
    }

    /// <summary>Confirms one visually observed stage and records its confidence.</summary>
    public SessionState Confirm(
        StartupStage stage,
        double confidence,
        string? note,
        IReadOnlyList<ProcessSnapshot> processes,
        IReadOnlyList<WindowSnapshot> windows,
        DateTimeOffset nowUtc)
    {
        if (!double.IsFinite(confidence) || confidence is < 0 or > 1)
        {
            throw new ArgumentOutOfRangeException(nameof(confidence), "Confidence must be between 0 and 1.");
        }

        if (stage is StartupStage.Idle or StartupStage.Failed)
        {
            throw new ArgumentException("AI confirmation cannot set idle or failed stages.", nameof(stage));
        }

        lock (gate_)
        {
            if (sessionId_ is null)
            {
                throw new InvalidOperationException("Start a session before confirming a visual stage.");
            }

            var humanAction = stage == StartupStage.AwaitingUac
                ? "Approve the Windows UAC prompt manually. The broker never interacts with the secure desktop."
                : stage == StartupStage.HumanActionRequired ? note : null;
            TransitionLocked(stage, StageSource.AiConfirmed, confidence, humanAction,
                string.IsNullOrWhiteSpace(note) ? $"AI confirmed stage {stage}." : note.Trim(), nowUtc);
            return SnapshotLocked(processes, windows);
        }
    }

    /// <summary>Returns the current immutable state without changing it.</summary>
    public SessionState Snapshot(
        IReadOnlyList<ProcessSnapshot> processes,
        IReadOnlyList<WindowSnapshot> windows)
    {
        lock (gate_)
        {
            return SnapshotLocked(processes, windows);
        }
    }

    /// <summary>Records a terminal broker startup failure with an actionable human-facing reason.</summary>
    public SessionState Fail(
        string reason,
        IReadOnlyList<ProcessSnapshot> processes,
        IReadOnlyList<WindowSnapshot> windows,
        DateTimeOffset nowUtc)
    {
        if (string.IsNullOrWhiteSpace(reason))
        {
            throw new ArgumentException("A failure reason is required.", nameof(reason));
        }

        lock (gate_)
        {
            if (sessionId_ is null)
            {
                throw new InvalidOperationException("Start a session before recording its failure.");
            }

            var normalizedReason = reason.Trim();
            TransitionLocked(StartupStage.Failed, StageSource.Internal, 1,
                normalizedReason, $"Startup failed: {normalizedReason}", nowUtc);
            return SnapshotLocked(processes, windows);
        }
    }

    /// <summary>Applies one state transition and writes a bounded timeline event.</summary>
    private void TransitionLocked(
        StartupStage stage,
        StageSource source,
        double confidence,
        string? humanAction,
        string message,
        DateTimeOffset nowUtc)
    {
        stage_ = stage;
        source_ = source;
        confidence_ = confidence;
        humanAction_ = humanAction;
        updatedAtUtc_ = nowUtc;
        timeline_.Add(new TimelineEvent(nowUtc, stage, source, message));
        if (timeline_.Count > 128)
        {
            timeline_.RemoveRange(0, timeline_.Count - 128);
        }
    }

    /// <summary>Builds a defensive copy of the public state.</summary>
    private SessionState SnapshotLocked(
        IReadOnlyList<ProcessSnapshot> processes,
        IReadOnlyList<WindowSnapshot> windows) =>
        new(sessionId_, target_, stage_, source_, confidence_, humanAction_, updatedAtUtc_,
            processes.ToArray(), windows.ToArray(), timeline_.ToArray());
}
