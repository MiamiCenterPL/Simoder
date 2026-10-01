using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace Simoder.DevBridge.Core;

/// <summary>Calls the elevated broker through one request-per-connection named-pipe exchanges.</summary>
public sealed class BrokerPipeClient
{
    private readonly string pipeName_;

    /// <summary>Creates a client for the current user's broker instance.</summary>
    public BrokerPipeClient(string? pipeName = null)
    {
        pipeName_ = pipeName ?? PipeNaming.GetCurrentPipeName();
    }

    /// <summary>Sends one typed request and deserializes its successful response.</summary>
    public async Task<TResponse> CallAsync<TRequest, TResponse>(
        BrokerCommand command,
        TRequest payload,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        using var timeoutSource = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutSource.CancelAfter(timeout);
        await using var pipe = new NamedPipeClientStream(
            ".", pipeName_, PipeDirection.InOut, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        await pipe.ConnectAsync(timeoutSource.Token).ConfigureAwait(false);

        var payloadElement = JsonSerializer.SerializeToElement(payload, JsonDefaults.Options);
        var request = new BrokerRequest(Guid.NewGuid().ToString("N"), command, payloadElement);
        await WriteLineAsync(pipe, JsonSerializer.Serialize(request, JsonDefaults.Options), timeoutSource.Token)
            .ConfigureAwait(false);

        var responseLine = await ReadLineAsync(pipe, timeoutSource.Token).ConfigureAwait(false);
        var response = JsonSerializer.Deserialize<BrokerResponse>(responseLine, JsonDefaults.Options)
            ?? throw new InvalidDataException("The broker returned an empty response.");
        if (!response.Success)
        {
            throw new BrokerException(response.ErrorCode ?? "broker_error", response.ErrorMessage ?? "Broker request failed.");
        }

        if (response.Data is null)
        {
            throw new InvalidDataException("The broker response does not contain data.");
        }

        return response.Data.Value.Deserialize<TResponse>(JsonDefaults.Options)
            ?? throw new InvalidDataException("The broker response data has an unexpected shape.");
    }

    /// <summary>Sends one newline-delimited UTF-8 message without closing the pipe.</summary>
    private static async Task WriteLineAsync(Stream stream, string value, CancellationToken cancellationToken)
    {
        var bytes = Encoding.UTF8.GetBytes(value + "\n");
        await stream.WriteAsync(bytes, cancellationToken).ConfigureAwait(false);
        await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
    }

    /// <summary>Reads one bounded newline-delimited UTF-8 message.</summary>
    private static async Task<string> ReadLineAsync(Stream stream, CancellationToken cancellationToken)
    {
        const int maximumBytes = 32 * 1024 * 1024;
        using var buffer = new MemoryStream();
        var singleByte = new byte[1];
        while (buffer.Length < maximumBytes)
        {
            var read = await stream.ReadAsync(singleByte, cancellationToken).ConfigureAwait(false);
            if (read == 0 || singleByte[0] == (byte)'\n')
            {
                break;
            }

            buffer.WriteByte(singleByte[0]);
        }

        if (buffer.Length == maximumBytes)
        {
            throw new InvalidDataException("The broker response exceeded the 32 MiB protocol limit.");
        }

        return Encoding.UTF8.GetString(buffer.ToArray());
    }
}

/// <summary>Represents a structured broker refusal or operational failure.</summary>
public sealed class BrokerException : Exception
{
    /// <summary>Creates an exception with a stable machine-readable error code.</summary>
    public BrokerException(string errorCode, string message)
        : base(message)
    {
        ErrorCode = errorCode;
    }

    /// <summary>Gets the stable machine-readable error code.</summary>
    public string ErrorCode { get; }
}
