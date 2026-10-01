using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;
using Simoder.DevBridge.Core;

namespace Simoder.DevBridge;

/// <summary>Hosts a same-user, same-session, executable-verified named-pipe endpoint.</summary>
internal sealed class BrokerPipeServer
{
    private const int MaximumMessageBytes = 1024 * 1024;
    private readonly string pipeName_;
    private readonly BrokerService service_;
    private readonly BrokerLogger logger_;
    private readonly string expectedClientPath_;

    /// <summary>Creates the private server and pins its only accepted client executable.</summary>
    public BrokerPipeServer(string pipeName, BrokerService service, BrokerLogger logger)
    {
        pipeName_ = pipeName;
        service_ = service;
        logger_ = logger;
        expectedClientPath_ = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "Simoder.McpServer.exe"));
    }

    /// <summary>Accepts one request per connection until cancellation.</summary>
    public async Task RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            await using var pipe = new NamedPipeServerStream(
                pipeName_, PipeDirection.InOut, 1,
                PipeTransmissionMode.Byte,
                PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly,
                MaximumMessageBytes, 32 * 1024 * 1024);
            await pipe.WaitForConnectionAsync(cancellationToken).ConfigureAwait(false);
            await HandleConnectionAsync(pipe, cancellationToken).ConfigureAwait(false);
        }
    }

    /// <summary>Verifies the caller, dispatches one frame, and emits one response frame.</summary>
    private async Task HandleConnectionAsync(NamedPipeServerStream pipe, CancellationToken cancellationToken)
    {
        BrokerRequest? request = null;
        BrokerResponse response;
        try
        {
            VerifyClient(pipe.SafePipeHandle);
            var line = await ReadLineAsync(pipe, cancellationToken).ConfigureAwait(false);
            request = JsonSerializer.Deserialize<BrokerRequest>(line, JsonDefaults.Options)
                ?? throw new InvalidDataException("The request frame is empty.");
            var result = await service_.ExecuteAsync(request, cancellationToken).ConfigureAwait(false);
            response = new BrokerResponse(request.RequestId, true,
                JsonSerializer.SerializeToElement(result, result.GetType(), JsonDefaults.Options), null, null);
        }
        catch (Exception exception)
        {
            var errorCode = exception switch
            {
                UnauthorizedAccessException => "unauthorized_client",
                ArgumentException => "invalid_argument",
                InvalidOperationException => "invalid_state",
                KeyNotFoundException => "unknown_capture",
                OperationCanceledException => "cancelled",
                _ => "broker_error",
            };
            logger_.Write($"Broker request failed ({errorCode}): {exception.Message}");
            response = new BrokerResponse(request?.RequestId ?? string.Empty, false, null, errorCode, exception.Message);
        }

        await WriteLineAsync(pipe, JsonSerializer.Serialize(response, JsonDefaults.Options), cancellationToken)
            .ConfigureAwait(false);
    }

    /// <summary>Rejects callers outside the current session or outside the installed MCP executable path.</summary>
    private void VerifyClient(SafePipeHandle pipeHandle)
    {
        if (!GetNamedPipeClientProcessId(pipeHandle, out var processId))
        {
            throw new UnauthorizedAccessException("The named-pipe client process id is unavailable.");
        }

        using var client = Process.GetProcessById(unchecked((int)processId));
        var clientPath = client.MainModule?.FileName
            ?? throw new UnauthorizedAccessException("The named-pipe client executable path is unavailable.");
        if (!ClientAuthorization.IsAllowed(
                expectedClientPath_, clientPath,
                Process.GetCurrentProcess().SessionId, client.SessionId))
        {
            throw new UnauthorizedAccessException("Only the sibling Simoder.McpServer process in this Windows session may use DevBridge.");
        }
    }

    /// <summary>Reads one bounded UTF-8 line from the client.</summary>
    private static async Task<string> ReadLineAsync(Stream stream, CancellationToken cancellationToken)
    {
        using var buffer = new MemoryStream();
        var singleByte = new byte[1];
        while (buffer.Length < MaximumMessageBytes)
        {
            var read = await stream.ReadAsync(singleByte, cancellationToken).ConfigureAwait(false);
            if (read == 0 || singleByte[0] == (byte)'\n')
            {
                break;
            }
            buffer.WriteByte(singleByte[0]);
        }

        if (buffer.Length == MaximumMessageBytes)
        {
            throw new InvalidDataException("The broker request exceeded the 1 MiB protocol limit.");
        }
        return Encoding.UTF8.GetString(buffer.ToArray());
    }

    /// <summary>Writes one UTF-8 response line and flushes it immediately.</summary>
    private static async Task WriteLineAsync(Stream stream, string value, CancellationToken cancellationToken)
    {
        var bytes = Encoding.UTF8.GetBytes(value + "\n");
        await stream.WriteAsync(bytes, cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetNamedPipeClientProcessId(SafePipeHandle pipeHandle, out uint clientProcessId);
}
