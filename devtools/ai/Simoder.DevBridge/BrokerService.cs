using System.Diagnostics;
using System.Text.Json;
using Simoder.DevBridge.Core;

namespace Simoder.DevBridge;

/// <summary>Implements the complete elevated broker command surface.</summary>
internal sealed class BrokerService
{
    private const string SimCityLaunchUri = "origin2://game/launch/?offerIds=71654,71480,71630,71650,71573,71631,71652,71572,71632,1004769,1004768,1004771,1004770,1008749,1008760,1008761,1008762,1008763,1008764,1008776,1008777,1008778,1008779,1015233,1015232,1015226&title=SimCity%u2122%u003a%u0020Complete%u0020Edition&cmdParams=";
    private readonly string gameRoot_;
    private readonly BrokerLogger logger_;
    private readonly ProcessWindowService processWindows_;
    private readonly WindowCaptureService captures_;
    private readonly StartupSessionCoordinator coordinator_ = new();

    /// <summary>Creates a broker service rooted in one explicit SimCity installation.</summary>
    public BrokerService(string gameRoot, BrokerLogger logger)
    {
        gameRoot_ = gameRoot;
        logger_ = logger;
        processWindows_ = new ProcessWindowService(gameRoot);
        captures_ = new WindowCaptureService(processWindows_);
    }

    /// <summary>Dispatches one validated protocol command.</summary>
    public async Task<object> ExecuteAsync(BrokerRequest request, CancellationToken cancellationToken)
    {
        return request.Command switch
        {
            BrokerCommand.Ping => new { status = "ready", elevated = true, pipe = PipeNaming.GetCurrentPipeName() },
            BrokerCommand.StartSession => StartSession(Deserialize<StartSessionRequest>(request.Payload)),
            BrokerCommand.GetState => ObserveState(),
            BrokerCommand.CaptureWindow => captures_.Capture(Deserialize<CaptureWindowRequest>(request.Payload).Target),
            BrokerCommand.Click => Click(Deserialize<ClickRequest>(request.Payload)),
            BrokerCommand.SendKey => SendKey(Deserialize<SendKeyRequest>(request.Payload)),
            BrokerCommand.ConfirmStage => ConfirmStage(Deserialize<ConfirmStageRequest>(request.Payload)),
            BrokerCommand.WaitForStage => await WaitForStageAsync(
                Deserialize<WaitForStageRequest>(request.Payload), cancellationToken).ConfigureAwait(false),
            BrokerCommand.GetDiagnostics => GetDiagnostics(),
            BrokerCommand.GetRuntimeEvidence => RuntimeEvidenceReader.Read(gameRoot_,
                processWindows_.GetProcesses().Where(process => process.IsAllowed &&
                    process.Name.Equals("SimCity", StringComparison.OrdinalIgnoreCase))
                    .Select(process => process.ProcessId).ToHashSet(), DateTimeOffset.UtcNow),
            BrokerCommand.GetModLogs => GetModLogs(Deserialize<ModLogsRequest>(request.Payload)),
            _ => throw new ArgumentOutOfRangeException(nameof(request), "Unknown broker command."),
        };
    }

    /// <summary>Starts the watcher and delegates the game request to the installed EA App.</summary>
    private SessionState StartSession(StartSessionRequest request)
    {
        var state = coordinator_.Start(request.Target, DateTimeOffset.UtcNow);
        try
        {
            StartWatcherIfNeeded();
            logger_.Write($"Starting EA workflow session {state.SessionId} in {request.Target.Mode} mode.");
            using var launchProcess = Process.Start(new ProcessStartInfo
            {
                FileName = SimCityLaunchUri,
                UseShellExecute = true,
            }) ?? throw new InvalidOperationException("Windows did not accept the EA App launch URI.");
            return ObserveState();
        }
        catch (Exception exception) when (exception is FileNotFoundException or InvalidOperationException or
                                          System.ComponentModel.Win32Exception)
        {
            logger_.Write($"EA workflow startup failed: {exception.Message}");
            var processes = processWindows_.GetProcesses();
            var windows = processWindows_.GetWindows(processes);
            return coordinator_.Fail(exception.Message, processes, windows, DateTimeOffset.UtcNow);
        }
    }

    /// <summary>@summary Reads only the fixed Simoder log with optional mod filtering.</summary>
    private object GetModLogs(ModLogsRequest request) => new
    {
        lines = RuntimeEvidenceReader.ReadLogs(gameRoot_, request.ModId, request.Limit),
        modId = request.ModId,
        readAtUtc = DateTimeOffset.UtcNow,
    };

    /// <summary>Starts one elevated Simoder watcher as a child of the already-elevated broker.</summary>
    private void StartWatcherIfNeeded()
    {
        var runtimeRoot = Path.Combine(gameRoot_, "simoder");
        var watcherPath = Path.GetFullPath(Path.Combine(runtimeRoot, "simoder.exe"));
        var loaderPath = Path.GetFullPath(Path.Combine(runtimeRoot, "sc13modloader.dll"));
        if (!File.Exists(watcherPath) || !File.Exists(loaderPath))
        {
            throw new FileNotFoundException("The installed Simoder watcher or loader DLL is missing.");
        }

        var alreadyRunning = processWindows_.GetProcesses().Any(process =>
            process.IsAllowed && process.Name.Equals("simoder", StringComparison.OrdinalIgnoreCase));
        if (alreadyRunning)
        {
            logger_.Write("Reusing the existing elevated Simoder watcher.");
            return;
        }

        var watcher = Process.Start(new ProcessStartInfo
        {
            FileName = watcherPath,
            Arguments = $"--watch-attach 4294967295 \"{loaderPath}\"",
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden,
            WorkingDirectory = runtimeRoot,
        }) ?? throw new InvalidOperationException("The Simoder watcher could not be started.");
        logger_.Write($"Started elevated watcher PID {watcher.Id}.");
        watcher.Dispose();
    }

    /// <summary>Reconciles native process/window facts with the workflow state machine.</summary>
    private SessionState ObserveState()
    {
        var processes = processWindows_.GetProcesses();
        var windows = processWindows_.GetWindows(processes);
        return coordinator_.Observe(processes, windows, DateTimeOffset.UtcNow);
    }

    /// <summary>Performs one coordinate-bound click and reports its chosen input path.</summary>
    private object Click(ClickRequest request)
    {
        var mechanism = captures_.Click(request);
        logger_.Write($"Authorized click used {mechanism} for capture {request.CaptureId}.");
        return new { accepted = true, mechanism };
    }

    /// <summary>Sends one allowlisted key without accepting arbitrary text or virtual-key values.</summary>
    private object SendKey(SendKeyRequest request)
    {
        captures_.SendKey(request);
        logger_.Write($"Authorized {request.Key} key for target {request.Target}.");
        return new { accepted = true, key = request.Key, target = request.Target };
    }

    /// <summary>Confirms an AI-observed stage only when its evidence capture remains current.</summary>
    private SessionState ConfirmStage(ConfirmStageRequest request)
    {
        var capture = captures_.GetCapture(request.CaptureId);
        var currentWindow = processWindows_.FindWindow(capture.Window.Handle)
            ?? throw new InvalidOperationException("The evidence window no longer exists.");
        CaptureGuard.Validate(capture, currentWindow, DateTimeOffset.UtcNow, 0.5, 0.5);
        var processes = processWindows_.GetProcesses();
        var windows = processWindows_.GetWindows(processes);
        logger_.Write($"AI confirmed stage {request.Stage} from capture {request.CaptureId} with confidence {request.Confidence:F2}.");
        return coordinator_.Confirm(request.Stage, request.Confidence, request.Note,
            processes, windows, DateTimeOffset.UtcNow);
    }

    /// <summary>Waits for a target stage while repeatedly updating native observations.</summary>
    private async Task<SessionState> WaitForStageAsync(WaitForStageRequest request, CancellationToken cancellationToken)
    {
        if (request.TimeoutSeconds is < 1 or > 300)
        {
            throw new ArgumentOutOfRangeException(nameof(request), "timeoutSeconds must be between 1 and 300.");
        }

        var deadline = DateTimeOffset.UtcNow.AddSeconds(request.TimeoutSeconds);
        SessionState state;
        do
        {
            state = ObserveState();
            if (state.Stage == request.Stage || state.Stage is StartupStage.Failed or StartupStage.HumanActionRequired or StartupStage.SuspectedInfiniteLoading)
            {
                return state;
            }

            await Task.Delay(TimeSpan.FromMilliseconds(250), cancellationToken).ConfigureAwait(false);
        }
        while (DateTimeOffset.UtcNow < deadline);

        return state;
    }

    /// <summary>Collects bounded process, overlay, focus, and log evidence without mutating the game.</summary>
    private DiagnosticSnapshot GetDiagnostics()
    {
        var state = ObserveState();
        return new DiagnosticSnapshot(
            state,
            ReadOverlayModules(state.Processes),
            ReadRecentLogs(),
            captures_.GetLastCapture(),
            DateTimeOffset.UtcNow);
    }

    /// <summary>Lists only module names relevant to common overlay and graphics-hook diagnostics.</summary>
    private static IReadOnlyList<string> ReadOverlayModules(IReadOnlyList<ProcessSnapshot> processes)
    {
        var simCity = processes.FirstOrDefault(process => process.IsAllowed &&
            process.Name.Equals("SimCity", StringComparison.OrdinalIgnoreCase));
        if (simCity is null)
        {
            return [];
        }

        var modules = new List<string>();
        try
        {
            using var process = Process.GetProcessById(simCity.ProcessId);
            foreach (ProcessModule module in process.Modules)
            {
                var name = module.ModuleName;
                if (name.Contains("overlay", StringComparison.OrdinalIgnoreCase) ||
                    name.Contains("discord", StringComparison.OrdinalIgnoreCase) ||
                    name.Contains("nv", StringComparison.OrdinalIgnoreCase) ||
                    name.Contains("igo", StringComparison.OrdinalIgnoreCase) ||
                    name.Contains("hook", StringComparison.OrdinalIgnoreCase))
                {
                    modules.Add(name);
                }
            }
        }
        catch (Exception exception) when (exception is InvalidOperationException or System.ComponentModel.Win32Exception or NotSupportedException)
        {
            modules.Add($"module_enumeration_failed:{exception.GetType().Name}");
        }
        return modules.Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(name => name).Take(128).ToArray();
    }

    /// <summary>Reads bounded tails from Simoder and EA logs relevant to startup analysis.</summary>
    private IReadOnlyList<string> ReadRecentLogs()
    {
        var paths = new List<string>
        {
            Path.Combine(gameRoot_, "simoder", "logs", "sc13modloader.log"),
            Path.Combine(gameRoot_, "simoder", "DevTools", "logs", "devbridge.log"),
        };
        var eaLogs = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Electronic Arts", "EA Desktop", "Logs");
        foreach (var name in new[] { "EALaunchHelper.log", "EALauncher.log" })
        {
            paths.Add(Path.Combine(eaLogs, name));
        }

        var output = new List<string>();
        foreach (var path in paths.Where(File.Exists))
        {
            try
            {
                var lines = File.ReadLines(path).TakeLast(80);
                output.AddRange(lines.Select(line => $"[{Path.GetFileName(path)}] {line}"));
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
            {
                output.Add($"[{Path.GetFileName(path)}] read_failed:{exception.GetType().Name}");
            }
        }
        return output.TakeLast(240).ToArray();
    }

    /// <summary>Deserializes one protocol payload using the shared strict settings.</summary>
    private static T Deserialize<T>(JsonElement payload) =>
        payload.Deserialize<T>(JsonDefaults.Options)
        ?? throw new ArgumentException($"The broker payload for {typeof(T).Name} is missing or invalid.");
}
