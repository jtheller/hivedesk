using System.Diagnostics;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;

namespace HiveDesk.Settings;

internal enum UpdateCheckStatus { Current, Requested, Unavailable }
internal sealed record UpdateCheckResult(UpdateCheckStatus Status, string Version);
internal sealed record UpdateProcessResult(int ExitCode, string Output, string Error);

// The ordinary-user UI only checks the signed release and requests an earlier
// service poll. Download, verification and idle installation remain in the OTA worker.
internal sealed class UpdateBridge(bool preview, Func<CancellationToken, Task<UpdateProcessResult>>? run = null, TimeSpan? timeout = null)
{
    internal static string CurrentVersion => typeof(UpdateBridge).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion.Split('+')[0]
        ?? typeof(UpdateBridge).Assembly.GetName().Version?.ToString() ?? "unknown";

    internal async Task<UpdateCheckResult> Check(CancellationToken cancellation = default)
    {
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        deadline.CancelAfter(timeout ?? TimeSpan.FromSeconds(90));
        try
        {
            UpdateProcessResult result;
            if (run != null) result = await run(deadline.Token).WaitAsync(deadline.Token).ConfigureAwait(false);
            else if (preview)
            {
                await Task.Delay(150, deadline.Token).ConfigureAwait(false);
                return new(UpdateCheckStatus.Current, CurrentVersion);
            }
            else result = await RunUpdater(deadline.Token).ConfigureAwait(false);
            if (result.ExitCode != 0)
                throw new InvalidOperationException("Could not check for updates. Check your connection and try again.");
            return Parse(result.Output);
        }
        catch (OperationCanceledException) when (!cancellation.IsCancellationRequested)
        {
            throw new TimeoutException("The update check timed out. Try again.");
        }
    }

    internal static UpdateCheckResult Parse(string text)
    {
        if (text.Length > 512) throw new InvalidOperationException("Invalid update response.");
        var lines = text.Replace("\r\n", "\n").Split('\n');
        if (lines.Length != 4 || lines[0] != "HiveDesk-Update-Check-1" || lines[3] != "" ||
            !lines[2].StartsWith("version=", StringComparison.Ordinal))
            throw new InvalidOperationException("Invalid update response.");
        var version = lines[2][8..];
        const string number = "(0|[1-9][0-9]{0,6})";
        if (!Regex.IsMatch(version, "\\A" + number + "\\." + number + "\\." + number + "(-(alpha|beta|rc)\\." + number + ")?\\z", RegexOptions.CultureInvariant) ||
            Regex.Matches(version, "[0-9]+").Any(part => uint.Parse(part.Value) > 1000000))
            throw new InvalidOperationException("Invalid update version.");
        var status = lines[1] switch
        {
            "status=current" => UpdateCheckStatus.Current,
            "status=requested" => UpdateCheckStatus.Requested,
            "status=unavailable" => UpdateCheckStatus.Unavailable,
            _ => throw new InvalidOperationException("Invalid update status.")
        };
        return new(status, version);
    }

    private static async Task<UpdateProcessResult> RunUpdater(CancellationToken cancellation)
    {
        var executable = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "HiveDeskUpdater.exe"));
        if (!File.Exists(executable)) throw new InvalidOperationException("The update helper is missing. Reinstall HiveDesk to restore it.");
        using var process = new Process { StartInfo = new ProcessStartInfo(executable, "--check-now")
        {
            UseShellExecute = false, CreateNoWindow = true,
            WorkingDirectory = Path.GetDirectoryName(executable),
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
        } };
        cancellation.ThrowIfCancellationRequested();
        if (!process.Start()) throw new InvalidOperationException("Could not start the update check.");
        // Close can end the UI dispatcher immediately. Kill this helper in the
        // cancellation callback itself instead of waiting for a UI continuation.
        using var stopHelper = cancellation.Register(() =>
        {
            try { if (!process.HasExited) process.Kill(); }
            catch (InvalidOperationException) { }
            catch (System.ComponentModel.Win32Exception) { }
        });
        try
        {
            var output = process.StandardOutput.ReadToEndAsync(cancellation);
            var error = process.StandardError.ReadToEndAsync(cancellation);
            await process.WaitForExitAsync(cancellation).ConfigureAwait(false);
            return new(process.ExitCode, await output.ConfigureAwait(false), await error.ConfigureAwait(false));
        }
        finally
        {
            // This is the transient read/request helper, never the SYSTEM
            // updater. Closing Settings cannot cancel a download or installation.
            if (!process.HasExited)
            {
                try { process.Kill(); await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(3)).ConfigureAwait(false); }
                catch (InvalidOperationException) { }
                catch (System.ComponentModel.Win32Exception) { }
                catch (TimeoutException) { }
            }
        }
    }
}
