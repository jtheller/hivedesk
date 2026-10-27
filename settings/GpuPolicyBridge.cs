using System.Diagnostics;
using System.Text;
using System.Text.RegularExpressions;

namespace HiveDesk.Settings;

internal enum GpuPolicyState { Enabled, Disabled, Unavailable, Conflict }
internal enum GpuPolicyCommand { Status, Enable, Restore }
internal sealed record GpuPolicyResult(GpuPolicyState State, bool RestartRequired, string Code, string Message, bool Succeeded);
internal sealed record GpuPolicyProcessResult(int ExitCode, string Output, string Error);

// This preference is a driver application profile, not part of settings.ini.
// Always display the helper's verified readback instead of an optimistic toggle.
internal sealed class GpuPolicyBridge(bool preview,
    Func<GpuPolicyCommand, CancellationToken, Task<GpuPolicyProcessResult>>? run = null, TimeSpan? timeout = null)
{
    private bool demoEnabled;
    internal Task<GpuPolicyResult> Read(CancellationToken cancellation = default) => Invoke(GpuPolicyCommand.Status, cancellation);
    internal Task<GpuPolicyResult> Set(bool enabled, CancellationToken cancellation = default) =>
        Invoke(enabled ? GpuPolicyCommand.Enable : GpuPolicyCommand.Restore, cancellation);

    private async Task<GpuPolicyResult> Invoke(GpuPolicyCommand command, CancellationToken cancellation)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        deadline.CancelAfter(timeout ?? TimeSpan.FromSeconds(command == GpuPolicyCommand.Status ? 30 : 180));
        try
        {
            // Preview must never reach a real helper, including through injection.
            if (preview)
            {
                await Task.Delay(100, deadline.Token).ConfigureAwait(false);
                bool changed = command != GpuPolicyCommand.Status && demoEnabled != (command == GpuPolicyCommand.Enable);
                if (command != GpuPolicyCommand.Status) demoEnabled = command == GpuPolicyCommand.Enable;
                return new(demoEnabled ? GpuPolicyState.Enabled : GpuPolicyState.Disabled, changed, "preview", "Preview GPU policy.", true);
            }
            var result = await (run != null ? run(command, deadline.Token) : RunHelper(HelperPath, command, deadline.Token))
                .WaitAsync(deadline.Token).ConfigureAwait(false);
            return Parse(result.Output, result.ExitCode);
        }
        catch (OperationCanceledException) when (!cancellation.IsCancellationRequested)
        {
            throw new TimeoutException("The GPU policy request timed out. Its current state will be checked again.");
        }
    }

    internal static string HelperPath => Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "HiveDeskGpuPolicy.exe"));

    internal static GpuPolicyResult Parse(string text, int exitCode = 0)
    {
        if (text.Length > 2048) throw new InvalidOperationException("Invalid GPU policy response.");
        var lines = text.Replace("\r\n", "\n").Split('\n');
        if (lines.Length != 6 || lines[0] != "HiveDesk-Gpu-Policy-1" || lines[5] != "" ||
            !lines[3].StartsWith("code=", StringComparison.Ordinal) || !lines[4].StartsWith("message=", StringComparison.Ordinal))
            throw new InvalidOperationException("Invalid GPU policy response.");
        var state = lines[1] switch
        {
            "status=enabled" => GpuPolicyState.Enabled,
            "status=disabled" => GpuPolicyState.Disabled,
            "status=unavailable" => GpuPolicyState.Unavailable,
            "status=conflict" => GpuPolicyState.Conflict,
            _ => throw new InvalidOperationException("Invalid GPU policy status.")
        };
        bool restart = lines[2] switch
        {
            "restart=0" => false,
            "restart=1" => true,
            _ => throw new InvalidOperationException("Invalid GPU policy restart state.")
        };
        string code = lines[3][5..], message = lines[4][8..];
        if (!Regex.IsMatch(code, "\\A[a-z][a-z0-9_-]{0,63}\\z", RegexOptions.CultureInvariant) ||
            message.Length is 0 or > 1024 || message.Any(char.IsControl) ||
            (restart && (exitCode != 0 || state is GpuPolicyState.Unavailable or GpuPolicyState.Conflict)))
            throw new InvalidOperationException("Invalid GPU policy details.");
        return new(state, restart, code, message, exitCode == 0);
    }

    internal static async Task<GpuPolicyProcessResult> RunHelper(string executable, GpuPolicyCommand command, CancellationToken cancellation)
    {
        if (!File.Exists(executable)) throw new InvalidOperationException("The GPU policy helper is missing. Reinstall HiveDesk to restore it.");
        using var process = new Process { StartInfo = new ProcessStartInfo(executable)
        {
            UseShellExecute = false, CreateNoWindow = true,
            WorkingDirectory = Path.GetDirectoryName(executable),
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
        } };
        process.StartInfo.ArgumentList.Add(command switch
        {
            GpuPolicyCommand.Status => "--status",
            GpuPolicyCommand.Enable => "--enable",
            GpuPolicyCommand.Restore => "--restore",
            _ => throw new ArgumentOutOfRangeException(nameof(command))
        });
        cancellation.ThrowIfCancellationRequested();
        if (!process.Start()) throw new InvalidOperationException("Could not start the GPU policy helper.");
        // A write transaction must finish even if Settings closes. Only the
        // read-only status helper is safe to terminate during cancellation.
        using var stopRead = cancellation.Register(() =>
        {
            if (command != GpuPolicyCommand.Status) return;
            try { if (!process.HasExited) process.Kill(); }
            catch (InvalidOperationException) { }
            catch (System.ComponentModel.Win32Exception) { }
        });
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        // For mutations, the bounded caller may stop waiting, but this worker
        // keeps its pipe handles alive until the helper commits or rolls back.
        await process.WaitForExitAsync(command == GpuPolicyCommand.Status ? cancellation : CancellationToken.None).ConfigureAwait(false);
        return new(process.ExitCode, await output.ConfigureAwait(false), await error.ConfigureAwait(false));
    }
}
