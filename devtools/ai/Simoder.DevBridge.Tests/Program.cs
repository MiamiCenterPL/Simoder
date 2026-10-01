using System.Diagnostics;
using ModelContextProtocol.Client;
using ModelContextProtocol.Protocol;
using Simoder.DevBridge;
using Simoder.DevBridge.Core;
using Simoder.McpServer;

/// <summary>Runs deterministic DevBridge unit and MCP stdio integration tests without a game process.</summary>
internal static class Program
{
    private static int failures_;

    /// <summary>Runs the complete test collection and returns a process-compatible status.</summary>
    private static async Task<int> Main()
    {
        Run("continue target is normalized", TestContinueTarget);
        Run("play target requires exact names", TestPlayTarget);
        Run("capture guard accepts unchanged fresh window", TestFreshCapture);
        Run("capture guard rejects stale capture", TestStaleCapture);
        Run("capture guard rejects changed geometry", TestChangedGeometry);
        Run("capture guard rejects an inactive window", TestInactiveWindow);
        Run("capture guard rejects out-of-range coordinates", TestInvalidCoordinates);
        Run("capture guard rejects window chrome", TestWindowChromeCoordinates);
        Run("client authorization requires exact path and session", TestClientAuthorization);
        await RunAsync("Named Pipe transport accepts a same-user framed exchange",
            TestNamedPipeTransportAsync).ConfigureAwait(false);
        Run("Authenticode verifier rejects unsigned code and accepts installed EA", TestAuthenticodeVerifier);
        Run("process allowlist accepts only exact installed targets", TestInstalledProcessAllowlist);
        await RunAsync("Windows Graphics Capture returns a real HWND frame",
            TestWindowsGraphicsCaptureAsync).ConfigureAwait(false);
        Run("state machine detects bounded Infinite Loading", TestInfiniteLoadingState);
        Run("state machine preserves startup grace and terminal errors", TestStartupGraceAndFailure);
        Run("capture result contains native MCP image", TestCaptureImageResult);
        Run("diagnostics preserve the last capture as a native MCP image", TestDiagnosticImageResult);
        await RunAsync("MCP stdio handshake advertises exact tools", TestMcpToolDiscoveryAsync).ConfigureAwait(false);
        await RunAsync("Codex registration is idempotent and reversible", TestCodexRegistrationAsync).ConfigureAwait(false);

        Console.WriteLine(failures_ == 0
            ? "All Simoder DevBridge tests passed."
            : $"{failures_} Simoder DevBridge test(s) failed.");
        return failures_ == 0 ? 0 : 1;
    }

    /// <summary>Checks Continue normalization and rejection of unused names.</summary>
    private static void TestContinueTarget()
    {
        var target = LaunchTargetValidator.Validate(new LaunchTarget(LaunchMode.Continue));
        Assert(target.Mode == LaunchMode.Continue && target.RegionName is null && target.CityName is null,
            "Continue target was not normalized.");
        ExpectThrows<ArgumentException>(() =>
            LaunchTargetValidator.Validate(new LaunchTarget(LaunchMode.Continue, "Region", null)));
    }

    /// <summary>Checks Play requirements and whitespace normalization.</summary>
    private static void TestPlayTarget()
    {
        ExpectThrows<ArgumentException>(() =>
            LaunchTargetValidator.Validate(new LaunchTarget(LaunchMode.Play, "Region", null)));
        var target = LaunchTargetValidator.Validate(new LaunchTarget(LaunchMode.Play, " Region ", " City "));
        Assert(target.RegionName == "Region" && target.CityName == "City", "Play names were not normalized.");
    }

    /// <summary>Checks successful validation of a fresh unchanged capture.</summary>
    private static void TestFreshCapture()
    {
        var now = DateTimeOffset.UtcNow;
        var window = CreateWindow();
        var capture = new CaptureSnapshot("capture", now, window, string.Empty, []);
        CaptureGuard.Validate(capture, window, now.AddSeconds(1), 0.5, 0.5);
    }

    /// <summary>Checks stale capture rejection.</summary>
    private static void TestStaleCapture()
    {
        var now = DateTimeOffset.UtcNow;
        var window = CreateWindow();
        var capture = new CaptureSnapshot("capture", now, window, string.Empty, []);
        ExpectThrows<InvalidOperationException>(() =>
            CaptureGuard.Validate(capture, window, now.Add(CaptureGuard.MaximumAge).AddMilliseconds(1), 0.5, 0.5));
    }

    /// <summary>Checks geometry generation rejection.</summary>
    private static void TestChangedGeometry()
    {
        var now = DateTimeOffset.UtcNow;
        var window = CreateWindow();
        var changed = window with { Width = window.Width + 1 };
        var capture = new CaptureSnapshot("capture", now, window, string.Empty, []);
        ExpectThrows<InvalidOperationException>(() =>
            CaptureGuard.Validate(capture, changed, now, 0.5, 0.5));
    }

    /// <summary>Checks that a capture cannot authorize input after its window loses foreground focus.</summary>
    private static void TestInactiveWindow()
    {
        var now = DateTimeOffset.UtcNow;
        var window = CreateWindow();
        var capture = new CaptureSnapshot("capture", now, window, string.Empty, []);
        ExpectThrows<InvalidOperationException>(() =>
            CaptureGuard.Validate(capture, window with { IsForeground = false }, now, 0.5, 0.5));
    }

    /// <summary>Checks non-finite and out-of-range coordinate rejection.</summary>
    private static void TestInvalidCoordinates()
    {
        var now = DateTimeOffset.UtcNow;
        var window = CreateWindow();
        var capture = new CaptureSnapshot("capture", now, window, string.Empty, []);
        ExpectThrows<InvalidOperationException>(() =>
            CaptureGuard.Validate(capture, window, now, double.NaN, 0.5));
        ExpectThrows<InvalidOperationException>(() =>
            CaptureGuard.Validate(capture, window, now, 1.1, 0.5));
    }

    /// <summary>Checks that coordinates over title-bar chrome cannot authorize desktop input.</summary>
    private static void TestWindowChromeCoordinates()
    {
        var now = DateTimeOffset.UtcNow;
        var window = CreateWindow();
        var capture = new CaptureSnapshot("capture", now, window, string.Empty, []);
        ExpectThrows<InvalidOperationException>(() =>
            CaptureGuard.GetAuthorizedScreenPoint(capture, window, now, 0.5, 0.01));
        var point = CaptureGuard.GetAuthorizedScreenPoint(capture, window, now, 0.5, 0.5);
        Assert(point.X >= window.ClientLeft && point.Y >= window.ClientTop,
            "A client-area point was not mapped inside the client area.");
    }

    /// <summary>Checks both executable-path and Windows-session client restrictions.</summary>
    private static void TestClientAuthorization()
    {
        var expected = Path.GetFullPath("Simoder.McpServer.exe");
        Assert(ClientAuthorization.IsAllowed(expected, expected.ToUpperInvariant(), 4, 4),
            "Equivalent path and session were rejected.");
        Assert(!ClientAuthorization.IsAllowed(expected, Path.GetFullPath("notepad.exe"), 4, 4),
            "Foreign executable path was accepted.");
        Assert(!ClientAuthorization.IsAllowed(expected, expected, 4, 5),
            "Foreign Windows session was accepted.");
    }

    /// <summary>Checks the same-user-only pipe option and newline-delimited broker framing end to end.</summary>
    private static async Task TestNamedPipeTransportAsync()
    {
        var pipeName = $"Simoder.DevBridge.Test.{Guid.NewGuid():N}";
        var serverTask = Task.Run(async () =>
        {
            await using var server = new System.IO.Pipes.NamedPipeServerStream(
                pipeName,
                System.IO.Pipes.PipeDirection.InOut,
                1,
                System.IO.Pipes.PipeTransmissionMode.Byte,
                System.IO.Pipes.PipeOptions.Asynchronous | System.IO.Pipes.PipeOptions.CurrentUserOnly);
            await server.WaitForConnectionAsync().ConfigureAwait(false);
            using var reader = new StreamReader(server, new System.Text.UTF8Encoding(false), leaveOpen: true);
            await using var writer = new StreamWriter(server, new System.Text.UTF8Encoding(false), leaveOpen: true)
            {
                AutoFlush = true,
            };
            var request = System.Text.Json.JsonSerializer.Deserialize<BrokerRequest>(
                await reader.ReadLineAsync().ConfigureAwait(false) ?? string.Empty,
                JsonDefaults.Options) ?? throw new InvalidDataException("The test pipe request was empty.");
            var response = new BrokerResponse(
                request.RequestId,
                true,
                System.Text.Json.JsonSerializer.SerializeToElement(new { status = "ready" }, JsonDefaults.Options),
                null,
                null);
            await writer.WriteLineAsync(
                System.Text.Json.JsonSerializer.Serialize(response, JsonDefaults.Options)).ConfigureAwait(false);
        });

        var client = new BrokerPipeClient(pipeName);
        var response = await client.CallAsync<object, System.Text.Json.JsonElement>(
            BrokerCommand.Ping, new { }, TimeSpan.FromSeconds(5), CancellationToken.None).ConfigureAwait(false);
        Assert(response.GetProperty("status").GetString() == "ready",
            "The same-user named-pipe response was not decoded correctly.");
        await serverTask.WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false);
    }

    /// <summary>Checks publisher verification against unsigned test code and an installed EA binary when available.</summary>
    private static void TestAuthenticodeVerifier()
    {
        Assert(!ExecutableTrustVerifier.IsTrustedElectronicArtsBinary(Environment.ProcessPath!),
            "Unsigned Simoder test code was accepted as an Electronic Arts binary.");
        var eaDesktop = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            "Electronic Arts", "EA Desktop", "EA Desktop", "EADesktop.exe");
        if (File.Exists(eaDesktop))
        {
            Assert(ExecutableTrustVerifier.IsTrustedElectronicArtsBinary(eaDesktop),
                "The installed, valid Electronic Arts EADesktop.exe signature was rejected.");
        }
    }

    /// <summary>Checks exact SimCity and EA paths while rejecting unrelated or root-level lookalikes.</summary>
    private static void TestInstalledProcessAllowlist()
    {
        const string gameRoot = @"C:\Program Files\EA Games\SimCity";
        if (!Directory.Exists(gameRoot))
        {
            return;
        }

        var allowlist = new ProcessWindowService(gameRoot);
        var baseGame = Path.Combine(gameRoot, "SimCity", "SimCity.exe");
        var patchedGame = Path.Combine(gameRoot, "SimCityUserData", "Patches", "UpdatedApp", "SimCity.exe");
        Assert(allowlist.IsAllowedExecutable("SimCity", baseGame), "The exact signed base SimCity path was rejected.");
        Assert(allowlist.IsAllowedExecutable("SimCity", patchedGame), "The exact signed patched SimCity path was rejected.");
        Assert(!allowlist.IsAllowedExecutable("SimCity", Path.Combine(gameRoot, "SimCity.exe")),
            "A root-level SimCity lookalike path was accepted.");

        var eaDesktop = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            "Electronic Arts", "EA Desktop", "EA Desktop", "EADesktop.exe");
        if (File.Exists(eaDesktop))
        {
            Assert(allowlist.IsAllowedExecutable("EADesktop", eaDesktop), "The exact signed EA App path was rejected.");
        }
        Assert(!allowlist.IsAllowedExecutable("EADesktop", Environment.ProcessPath!),
            "An unrelated executable was accepted as EA App.");
    }

    /// <summary>Exercises the real HWND WGC path against a controlled visible window.</summary>
    private static async Task TestWindowsGraphicsCaptureAsync()
    {
        var ready = new TaskCompletionSource<(IntPtr Handle, Action Close)>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var windowThread = new Thread(() =>
        {
            using var form = new System.Windows.Forms.Form
            {
                Text = "Simoder WGC Integration Test",
                Width = 640,
                Height = 360,
                StartPosition = System.Windows.Forms.FormStartPosition.Manual,
                Left = 80,
                Top = 80,
            };
            form.Shown += (_, _) => ready.TrySetResult((form.Handle, () => form.BeginInvoke(form.Close)));
            System.Windows.Forms.Application.Run(form);
        })
        {
            IsBackground = true,
            Name = "Simoder WGC test window",
        };
        windowThread.SetApartmentState(ApartmentState.STA);
        windowThread.Start();
        var testWindow = await ready.Task.WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false);
        var captureService = new WindowsGraphicsCaptureService();
        try
        {
            var png = await captureService
                .TryCapturePngAsync(testWindow.Handle, TimeSpan.FromSeconds(5)).ConfigureAwait(false);
            Assert(png is { Length: > 8 } && png.AsSpan(0, 8).SequenceEqual(
                new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }),
                $"Windows Graphics Capture did not return a valid PNG: {captureService.LastFailure}");
        }
        finally
        {
            testWindow.Close();
            Assert(windowThread.Join(TimeSpan.FromSeconds(5)), "The WGC test window thread did not stop.");
        }
    }

    /// <summary>Checks transition into diagnostic-only Infinite Loading suspicion.</summary>
    private static void TestInfiniteLoadingState()
    {
        var now = DateTimeOffset.UtcNow;
        var coordinator = new StartupSessionCoordinator();
        _ = coordinator.Start(new LaunchTarget(LaunchMode.Continue), now);
        var game = new ProcessSnapshot(42, "SimCity", "C:\\Game\\SimCity.exe", 1, true);
        _ = coordinator.Confirm(StartupStage.CityLoading, 0.95, "Loading spinner visible", [game], [], now);
        var state = coordinator.Observe([game], [], now.AddMinutes(2).AddSeconds(1));
        Assert(state.Stage == StartupStage.SuspectedInfiniteLoading, "Infinite Loading threshold did not transition.");
        Assert(state.HumanAction?.Contains("will not restart", StringComparison.OrdinalIgnoreCase) == true,
            "Infinite Loading state did not preserve the non-restart safety contract.");
    }

    /// <summary>Checks launch-process grace and explicit transition to the terminal error stage.</summary>
    private static void TestStartupGraceAndFailure()
    {
        var now = DateTimeOffset.UtcNow;
        var coordinator = new StartupSessionCoordinator();
        _ = coordinator.Start(new LaunchTarget(LaunchMode.Continue), now);
        var graceState = coordinator.Observe([], [], now.AddSeconds(5));
        Assert(graceState.Stage == StartupStage.EaStarting,
            "The startup process grace interval transitioned prematurely.");
        var failed = coordinator.Fail("EA launch URI failed", [], [], now.AddSeconds(6));
        Assert(failed.Stage == StartupStage.Failed && failed.HumanAction == "EA launch URI failed",
            "The explicit startup failure was not preserved as a terminal state.");
    }

    /// <summary>Checks that screenshots are native MCP images and base64 is absent from structured metadata.</summary>
    private static void TestCaptureImageResult()
    {
        var png = Convert.ToBase64String(
        [
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        ]);
        var capture = new CaptureSnapshot("capture", DateTimeOffset.UtcNow, CreateWindow(), png, []);
        var result = CaptureResultFactory.Create(capture);
        Assert(result.Content.OfType<ImageContentBlock>().Single().MimeType == "image/png",
            "Capture result did not contain one PNG image block.");
        Assert(result.StructuredContent?.ToString().Contains(png, StringComparison.Ordinal) == false,
            "Structured capture metadata leaked the base64 image payload.");
    }

    /// <summary>Checks that diagnostic screenshots are returned natively without structured base64.</summary>
    private static void TestDiagnosticImageResult()
    {
        var png = Convert.ToBase64String(
        [
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        ]);
        var capture = new CaptureSnapshot("diagnostic-capture", DateTimeOffset.UtcNow, CreateWindow(), png, []);
        var state = new SessionState(null, null, StartupStage.Idle, StageSource.Internal, 1, null,
            DateTimeOffset.UtcNow, [], [], []);
        var diagnostics = new DiagnosticSnapshot(state, [], [], capture, DateTimeOffset.UtcNow);
        var result = DiagnosticsResultFactory.Create(diagnostics);
        Assert(result.Content.OfType<ImageContentBlock>().Single().MimeType == "image/png",
            "Diagnostic result did not contain the last PNG image block.");
        Assert(result.StructuredContent?.ToString().Contains(png, StringComparison.Ordinal) == false,
            "Structured diagnostic metadata leaked the base64 image payload.");
    }

    /// <summary>Uses the official client SDK to verify stdio framing and exact tool discovery.</summary>
    private static async Task TestMcpToolDiscoveryAsync()
    {
        var installedServerPath = Environment.GetEnvironmentVariable("SIMODER_TEST_MCP_SERVER");
        var serverPath = string.IsNullOrWhiteSpace(installedServerPath)
            ? Path.Combine(AppContext.BaseDirectory, "Simoder.McpServer.dll")
            : Path.GetFullPath(installedServerPath);
        Assert(File.Exists(serverPath), $"MCP server assembly was not copied beside tests: {serverPath}");
        var transport = new StdioClientTransport(new StdioClientTransportOptions
        {
            Name = "Simoder test server",
            Command = string.IsNullOrWhiteSpace(installedServerPath) ? "dotnet" : serverPath,
            Arguments = string.IsNullOrWhiteSpace(installedServerPath) ? [serverPath] : [],
        });
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(20));
        await using var client = await McpClient.CreateAsync(transport, cancellationToken: timeout.Token)
            .ConfigureAwait(false);
        var actual = (await client.ListToolsAsync(cancellationToken: timeout.Token).ConfigureAwait(false))
            .Select(tool => tool.Name).OrderBy(name => name).ToArray();
        var expected = new[]
        {
            "simcity_capture_window",
            "simcity_click",
            "simcity_confirm_stage",
            "simcity_get_diagnostics",
            "simcity_get_startup_state",
            "simcity_send_key",
            "simcity_start_session",
            "simcity_wait_for_stage",
        };
        Assert(actual.SequenceEqual(expected),
            $"Unexpected MCP tool set: {string.Join(", ", actual)}");
    }

    /// <summary>Checks project-scoped registration, replacement, and managed-only removal.</summary>
    private static async Task TestCodexRegistrationAsync()
    {
        var temporaryRoot = Path.Combine(Path.GetTempPath(), "SimoderRegistrationTests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporaryRoot);
        try
        {
            var register = Path.Combine(AppContext.BaseDirectory, "Register-Simoder-MCP.ps1");
            var unregister = Path.Combine(AppContext.BaseDirectory, "Unregister-Simoder-MCP.ps1");
            var server = Path.Combine(AppContext.BaseDirectory, "Simoder.McpServer.exe");
            Assert(File.Exists(register) && File.Exists(unregister) && File.Exists(server),
                "Registration test payload is incomplete.");
            await RunPowerShellAsync(register, temporaryRoot).ConfigureAwait(false);
            await RunPowerShellAsync(register, temporaryRoot).ConfigureAwait(false);

            var configPath = Path.Combine(temporaryRoot, ".codex", "config.toml");
            var config = await File.ReadAllTextAsync(configPath).ConfigureAwait(false);
            Assert(CountOccurrences(config, "# BEGIN SIMODER MCP") == 1,
                "Idempotent registration wrote multiple managed blocks.");
            Assert(config.Contains("default_tools_approval_mode = \"writes\"", StringComparison.Ordinal),
                "Registration did not configure write-tool approvals.");

            await RunPowerShellAsync(unregister, temporaryRoot).ConfigureAwait(false);
            config = await File.ReadAllTextAsync(configPath).ConfigureAwait(false);
            Assert(!config.Contains("BEGIN SIMODER MCP", StringComparison.Ordinal),
                "Unregistration left the managed block behind.");
        }
        finally
        {
            var normalizedTemporaryRoot = Path.GetFullPath(temporaryRoot);
            var normalizedSystemTemp = Path.GetFullPath(Path.GetTempPath()).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            if (normalizedTemporaryRoot.StartsWith(normalizedSystemTemp, StringComparison.OrdinalIgnoreCase) &&
                Directory.Exists(normalizedTemporaryRoot))
            {
                Directory.Delete(normalizedTemporaryRoot, true);
            }
        }
    }

    /// <summary>Runs one registration script with literal, argument-list-safe values.</summary>
    private static async Task RunPowerShellAsync(string scriptPath, string projectPath)
    {
        using var process = Process.Start(new ProcessStartInfo
        {
            FileName = "powershell.exe",
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            ArgumentList =
            {
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                scriptPath,
                "-Scope",
                "Project",
                "-ProjectPath",
                projectPath,
            },
        }) ?? throw new InvalidOperationException("PowerShell registration test did not start.");
        var standardOutput = await process.StandardOutput.ReadToEndAsync().ConfigureAwait(false);
        var standardError = await process.StandardError.ReadToEndAsync().ConfigureAwait(false);
        await process.WaitForExitAsync().ConfigureAwait(false);
        Assert(process.ExitCode == 0,
            $"Registration script failed with {process.ExitCode}: {standardOutput} {standardError}");
    }

    /// <summary>Counts non-overlapping ordinal occurrences.</summary>
    private static int CountOccurrences(string value, string needle)
    {
        var count = 0;
        var offset = 0;
        while ((offset = value.IndexOf(needle, offset, StringComparison.Ordinal)) >= 0)
        {
            ++count;
            offset += needle.Length;
        }
        return count;
    }

    /// <summary>Creates one synthetic allowlisted window.</summary>
    private static WindowSnapshot CreateWindow() =>
        new(123, 42, "SimCity", "SimCity", 100, 200, 1280, 720,
            108, 231, 1264, 681, true, true);

    /// <summary>Runs one synchronous test and records an actionable failure.</summary>
    private static void Run(string name, Action test)
    {
        try
        {
            test();
            Console.WriteLine($"[PASS] {name}");
        }
        catch (Exception exception)
        {
            ++failures_;
            Console.Error.WriteLine($"[FAIL] {name}: {exception}");
        }
    }

    /// <summary>Runs one asynchronous test and records an actionable failure.</summary>
    private static async Task RunAsync(string name, Func<Task> test)
    {
        try
        {
            await test().ConfigureAwait(false);
            Console.WriteLine($"[PASS] {name}");
        }
        catch (Exception exception)
        {
            ++failures_;
            Console.Error.WriteLine($"[FAIL] {name}: {exception}");
        }
    }

    /// <summary>Fails when a condition is false.</summary>
    private static void Assert(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    /// <summary>Fails unless an action throws the expected exception type.</summary>
    private static void ExpectThrows<TException>(Action action)
        where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException)
        {
            return;
        }
        throw new InvalidOperationException($"Expected {typeof(TException).Name} was not thrown.");
    }
}
