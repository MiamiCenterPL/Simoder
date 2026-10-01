using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using ModelContextProtocol.Server;
using Simoder.McpServer;

/// <summary>Hosts the non-elevated Simoder startup MCP server over stdio.</summary>
internal static class Program
{
    /// <summary>Builds and runs the MCP host while reserving stdout for protocol frames.</summary>
    private static async Task Main(string[] args)
    {
        var builder = Host.CreateApplicationBuilder(args);
        builder.Logging.ClearProviders();
        builder.Logging.AddConsole(options => options.LogToStandardErrorThreshold = LogLevel.Trace);
        builder.Services.AddSingleton<DevBridgeClient>();
        builder.Services
            .AddMcpServer(options =>
            {
                options.ServerInstructions = SimCityTools.ServerInstructions;
            })
            .WithStdioServerTransport()
            .WithToolsFromAssembly();
        await builder.Build().RunAsync().ConfigureAwait(false);
    }
}
