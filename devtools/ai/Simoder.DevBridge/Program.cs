using System.Security.Principal;
using Simoder.DevBridge;
using Simoder.DevBridge.Core;

/// <summary>Hosts the elevated, allowlisted Windows automation broker.</summary>
internal static class Program
{
    /// <summary>Runs the named-pipe broker until the process is stopped.</summary>
    private static async Task<int> Main(string[] args)
    {
        if (!OperatingSystem.IsWindows())
        {
            Console.Error.WriteLine("Simoder.DevBridge supports Windows only.");
            return 2;
        }

        using var identity = WindowsIdentity.GetCurrent();
        var principal = new WindowsPrincipal(identity);
        if (!principal.IsInRole(WindowsBuiltInRole.Administrator))
        {
            Console.Error.WriteLine("Simoder.DevBridge must be approved through Windows UAC.");
            return 3;
        }

        var gameRoot = ResolveGameRoot(args);
        var logger = new BrokerLogger(Path.Combine(gameRoot, "simoder", "DevTools", "logs"));
        try
        {
            var service = new BrokerService(gameRoot, logger);
            var server = new BrokerPipeServer(PipeNaming.GetCurrentPipeName(), service, logger);
            logger.Write("DevBridge started for the current user and Windows session.");
            await server.RunAsync(CancellationToken.None).ConfigureAwait(false);
            return 0;
        }
        catch (Exception exception)
        {
            logger.Write($"Fatal broker error: {exception}");
            return 1;
        }
    }

    /// <summary>Resolves the game root supplied by the non-elevated MCP launcher.</summary>
    private static string ResolveGameRoot(string[] args)
    {
        for (var index = 0; index + 1 < args.Length; ++index)
        {
            if (args[index].Equals("--game-root", StringComparison.OrdinalIgnoreCase))
            {
                var candidate = Path.GetFullPath(args[index + 1]);
                if (!File.Exists(Path.Combine(candidate, "SimCity", "SimCity.exe")) &&
                    !Directory.Exists(Path.Combine(candidate, "SimCityData")))
                {
                    throw new DirectoryNotFoundException($"The supplied game root is not a SimCity installation: {candidate}");
                }

                return candidate;
            }
        }

        return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", ".."));
    }
}
