using System.ComponentModel;
using System.IO.Pipes;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;
using Trainer.Shared;

namespace Trainer.Plugin;

// These checks restrict the transport to the local account/session. They do not
// authenticate an unsigned desktop executable or isolate hostile same-user code.
internal sealed class PipePeerGuard : IDisposable
{
    private const uint QueryLimitedInformation = 0x1000;
    private const uint Synchronize = 0x100000;
    private const uint WaitTimeout = 258;
    private const uint RejectRemoteClients = 0x8;
    private readonly SafeProcessHandle _process;
    internal uint ProcessId { get; }
    internal uint SessionId { get; }

    private PipePeerGuard(SafeProcessHandle process, uint processId, uint sessionId)
    { _process = process; ProcessId = processId; SessionId = sessionId; }

    internal static NamedPipeServerStream CreateServer(string name)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("Local trainer pipes require Windows.");
        if (string.IsNullOrWhiteSpace(name) || name.IndexOfAny(new[] { '\\', '/' }) >= 0)
            throw new ArgumentException("Invalid local pipe name.", nameof(name));
        using var identity = WindowsIdentity.GetCurrent();
        string user = identity.User?.Value ?? throw new IOException("无法确定本机通信用户。");
        // The desktop's CurrentUserOnly implementation compares token Owner.
        // Keep that owner for compatibility, but grant access to actual User only.
        string owner = identity.Owner?.Value ?? user;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptor($"O:{owner}D:P(A;;GA;;;{user})", 1,
                out var descriptor, out _)) throw new Win32Exception(Marshal.GetLastWin32Error());
        try
        {
            var attributes = new SecurityAttributes
            { Length = Marshal.SizeOf<SecurityAttributes>(), Descriptor = descriptor, InheritHandle = false };
            // PIPE_REJECT_REMOTE_CLIENTS belongs to dwPipeMode, not PipeOptions
            // (which .NET passes to dwOpenMode). Refuse name squatting as well.
            var handle = CreateNamedPipe(@"\\.\pipe\" + name, 0x3 | 0x40000000 | 0x80000,
                RejectRemoteClients, 1, 4096, 4096, 0, ref attributes);
            if (handle.IsInvalid)
            {
                int error = Marshal.GetLastWin32Error(); handle.Dispose();
                throw new Win32Exception(error);
            }
            try { return new NamedPipeServerStream(PipeDirection.InOut, true, false, handle); }
            catch { handle.Dispose(); throw; }
        }
        finally { LocalFree(descriptor); }
    }

    internal static PipePeerGuard Verify(NamedPipeServerStream pipe)
    {
        if (!GetNamedPipeClientProcessId(pipe.SafePipeHandle, out uint processId) || processId == 0
            || !GetNamedPipeClientSessionId(pipe.SafePipeHandle, out uint sessionId)
            || !ProcessIdToSessionId((uint)Environment.ProcessId, out uint ownSession)
            || sessionId != ownSession
            || !ProcessIdToSessionId(processId, out uint processSession) || processSession != sessionId)
            throw new IOException("通信客户端不属于当前本机会话。");
        var process = OpenProcess(QueryLimitedInformation | Synchronize, false, processId);
        if (process.IsInvalid)
        { process.Dispose(); throw new IOException("无法验证通信客户端进程。"); }
        var peer = new PipePeerGuard(process, processId, sessionId);
        try { peer.EnsureAlive(); return peer; }
        catch { peer.Dispose(); throw; }
    }

    internal void EnsureAlive()
    {
        // Hold the original process handle throughout the connection, so PID
        // recycling cannot turn a dead client into an accepted live one.
        if (_process.IsClosed || WaitForSingleObject(_process, 0) != WaitTimeout)
            throw new IOException("通信客户端已退出，请重新连接。");
    }

    internal static BackupProof ReadLocalBackupProof(string? path)
    {
        string proofPath = RequireLocalFile(path);
        const int maximumBytes = 1024 * 1024;
        using var file = new FileStream(proofPath, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (file.Length > maximumBytes) throw new InvalidDataException("备份凭据过大。");
        var bytes = new byte[maximumBytes + 1];
        int count = 0, read;
        while (count < bytes.Length && (read = file.Read(bytes, count, bytes.Length - count)) != 0) count += read;
        if (count > maximumBytes) throw new InvalidDataException("备份凭据过大。");
        int start = count >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
        var proof = JsonSerializer.Deserialize<BackupProof>(bytes.AsSpan(start, count - start), PipeProtocol.JsonOptions)
            ?? throw new InvalidDataException("备份凭据无效。");
        if (proof.Files is null || proof.Files.Count is < 1 or > 256) throw new InvalidDataException("备份凭据文件数量无效。");
        foreach (var entry in proof.Files)
        {
            RequireLocalPath(entry.OriginalPath);
            RequireLocalFile(entry.BackupPath);
        }
        return proof;
    }

    private static string RequireLocalFile(string? path)
    {
        // Reject UNC, device paths, alternate streams and mapped network drives
        // before doing any filesystem read. Descend from the local drive root:
        // querying the leaf first could already traverse an unchecked junction.
        string full = RequireLocalPath(path);
        string current = Path.GetPathRoot(full)!;
        RejectReparse(current);
        foreach (string segment in full.Substring(current.Length).Split(
                     new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar }, StringSplitOptions.RemoveEmptyEntries))
        {
            current = Path.Combine(current, segment);
            RejectReparse(current);
        }
        return full;
    }

    private static void RejectReparse(string path)
    {
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException("备份路径不能经过符号链接或目录联接。");
    }

    private static string RequireLocalPath(string? path)
    {
        if (string.IsNullOrWhiteSpace(path) || path.Length > 32767 || path.Length < 3
            || !((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))
            || path[1] != ':' || path[2] != '\\'
            || path.IndexOf(':', 2) >= 0)
            throw new InvalidDataException("只接受本机磁盘上的备份凭据。");
        string full = Path.GetFullPath(path);
        uint driveType = GetDriveType(Path.GetPathRoot(full)!);
        if (driveType is not (2 or 3 or 5 or 6))
            throw new InvalidDataException("只接受本机磁盘上的备份凭据。");
        return full;
    }

    public void Dispose() => _process.Dispose();

    [StructLayout(LayoutKind.Sequential)]
    private struct SecurityAttributes
    { internal int Length; internal IntPtr Descriptor; [MarshalAs(UnmanagedType.Bool)] internal bool InheritHandle; }
    [DllImport("kernel32.dll", EntryPoint = "CreateNamedPipeW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafePipeHandle CreateNamedPipe(string name, uint openMode, uint pipeMode, uint maxInstances,
        uint outBufferSize, uint inBufferSize, uint defaultTimeout, ref SecurityAttributes attributes);
    [DllImport("advapi32.dll", EntryPoint = "ConvertStringSecurityDescriptorToSecurityDescriptorW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ConvertStringSecurityDescriptorToSecurityDescriptor(string descriptor, uint revision, out IntPtr result, out uint size);
    [DllImport("kernel32.dll")] private static extern IntPtr LocalFree(IntPtr memory);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetNamedPipeClientProcessId(SafePipeHandle pipe, out uint processId);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetNamedPipeClientSessionId(SafePipeHandle pipe, out uint sessionId);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ProcessIdToSessionId(uint processId, out uint sessionId);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern SafeProcessHandle OpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inheritHandle, uint processId);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(SafeProcessHandle handle, uint milliseconds);
    [DllImport("kernel32.dll", EntryPoint = "GetDriveTypeW", CharSet = CharSet.Unicode)]
    private static extern uint GetDriveType(string rootPath);
}
