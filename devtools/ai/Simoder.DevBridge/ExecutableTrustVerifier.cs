using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;

namespace Simoder.DevBridge;

/// <summary>Validates Authenticode trust and expected EA publisher identity.</summary>
public static class ExecutableTrustVerifier
{
    private static readonly Guid ActionGenericVerifyV2 = new("00AAC56B-CD44-11D0-8CC2-00C04FC295EE");

    /// <summary>Returns true only for a trusted Authenticode file published by Electronic Arts.</summary>
    public static bool IsTrustedElectronicArtsBinary(string executablePath)
    {
        if (!File.Exists(executablePath) || !VerifyAuthenticode(executablePath))
        {
            return false;
        }

        try
        {
            using var certificate = new X509Certificate2(X509Certificate.CreateFromSignedFile(executablePath));
            return certificate.Subject.Contains("Electronic Arts", StringComparison.OrdinalIgnoreCase) ||
                   certificate.GetNameInfo(X509NameType.SimpleName, false)
                       .Contains("Electronic Arts", StringComparison.OrdinalIgnoreCase);
        }
        catch (CryptographicException)
        {
            return false;
        }
    }

    /// <summary>Runs the Windows Authenticode trust provider through explicitly owned native structures.</summary>
    private static bool VerifyAuthenticode(string executablePath)
    {
        var actionPointer = IntPtr.Zero;
        var pathPointer = IntPtr.Zero;
        var fileInfoPointer = IntPtr.Zero;
        var trustDataPointer = IntPtr.Zero;
        try
        {
            actionPointer = Marshal.AllocHGlobal(Marshal.SizeOf<Guid>());
            Marshal.StructureToPtr(ActionGenericVerifyV2, actionPointer, false);
            pathPointer = Marshal.StringToHGlobalUni(executablePath);

            var fileInfo = new WinTrustFileInfo
            {
                StructSize = (uint)Marshal.SizeOf<WinTrustFileInfo>(),
                FilePath = pathPointer,
                FileHandle = IntPtr.Zero,
                KnownSubject = IntPtr.Zero,
            };
            fileInfoPointer = Marshal.AllocHGlobal(Marshal.SizeOf<WinTrustFileInfo>());
            Marshal.StructureToPtr(fileInfo, fileInfoPointer, false);

            var trustData = new WinTrustData
            {
                StructSize = (uint)Marshal.SizeOf<WinTrustData>(),
                PolicyCallbackData = IntPtr.Zero,
                SipClientData = IntPtr.Zero,
                UiChoice = 2,
                RevocationChecks = 0,
                UnionChoice = 1,
                FileInfo = fileInfoPointer,
                StateAction = 0,
                StateData = IntPtr.Zero,
                UrlReference = IntPtr.Zero,
                ProviderFlags = 0x00001000,
                UiContext = 0,
            };
            trustDataPointer = Marshal.AllocHGlobal(Marshal.SizeOf<WinTrustData>());
            Marshal.StructureToPtr(trustData, trustDataPointer, false);
            return WinVerifyTrust(IntPtr.Zero, actionPointer, trustDataPointer) == 0;
        }
        finally
        {
            if (trustDataPointer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(trustDataPointer);
            }
            if (fileInfoPointer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(fileInfoPointer);
            }
            if (pathPointer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(pathPointer);
            }
            if (actionPointer != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(actionPointer);
            }
        }
    }

    [DllImport("wintrust.dll", ExactSpelling = true, PreserveSig = true)]
    private static extern int WinVerifyTrust(IntPtr windowHandle, IntPtr actionId, IntPtr trustData);

    [StructLayout(LayoutKind.Sequential)]
    private struct WinTrustFileInfo
    {
        public uint StructSize;
        public IntPtr FilePath;
        public IntPtr FileHandle;
        public IntPtr KnownSubject;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WinTrustData
    {
        public uint StructSize;
        public IntPtr PolicyCallbackData;
        public IntPtr SipClientData;
        public uint UiChoice;
        public uint RevocationChecks;
        public uint UnionChoice;
        public IntPtr FileInfo;
        public uint StateAction;
        public IntPtr StateData;
        public IntPtr UrlReference;
        public uint ProviderFlags;
        public uint UiContext;
    }
}
