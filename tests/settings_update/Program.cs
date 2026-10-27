using HiveDesk.Settings;

static class UpdateBridgeChecks
{
    private const string Version = "0.1.0-alpha.32";
    private static readonly TimeSpan Guard = TimeSpan.FromSeconds(5);
    private static string Reply(string status, string version = Version) =>
        $"HiveDesk-Update-Check-1\nstatus={status}\nversion={version}\n";

    private static void Require(bool value, string message)
    {
        if (!value) throw new InvalidOperationException(message);
    }

    private static void Rejected(string text)
    {
        try { UpdateBridge.Parse(text); }
        catch (InvalidOperationException) { return; }
        throw new InvalidOperationException("Malformed or untrusted helper response was accepted.");
    }

    private static async Task<TException> Fails<TException>(Task task) where TException : Exception
    {
        try { await task.WaitAsync(Guard); }
        catch (TimeoutException) when (!task.IsCompleted)
        {
            throw new InvalidOperationException("Update check exceeded the outer test timeout.");
        }
        catch (TException error) { return error; }
        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }

    private static void Protocol()
    {
        foreach (var (status, expected) in new[] {
            ("current", UpdateCheckStatus.Current),
            ("requested", UpdateCheckStatus.Requested),
            ("unavailable", UpdateCheckStatus.Unavailable)
        })
        {
            foreach (var newline in new[] { "\n", "\r\n" })
            {
                var parsed = UpdateBridge.Parse(Reply(status).Replace("\n", newline));
                Require(parsed == new UpdateCheckResult(expected, Version), "Verified status/version did not survive the helper protocol.");
            }
        }
        foreach (var version in new[] { "1.2.3", "1.2.3-beta.4", "1.2.3-rc.5", "1000000.0.0-alpha.0" })
            Require(UpdateBridge.Parse(Reply("current", version)).Version == version, "Valid native updater version was rejected.");

        foreach (var response in new[] {
            "", "garbage", Reply("current").TrimEnd('\n'),
            Reply("current").Replace("HiveDesk-Update-Check-1", "HiveDesk-Update-Check-2"),
            Reply("CURRENT"), Reply("installed"), Reply("unknown"),
            Reply("current") + "status=requested\n", Reply("current") + "\n",
            $"HiveDesk-Update-Check-1\nversion={Version}\nstatus=current\n",
            Reply("current").Replace("version=", "version ="),
            Reply("current").Replace("\n", "\r"),
            Reply("requested", "../setup.exe"), Reply("current", "..\\HiveDeskUpdater.exe"),
            Reply("current", "0.1.0-alpha.32\0"), Reply("current", "0.1.0-alpha.32\nstatus=requested"),
            Reply("current", "0.1.0-alpha.032"), Reply("current", "01.2.3"),
            Reply("current", "0.1.0-alpha.1000001"), Reply("current", "1000001.2.3"),
            Reply("current", "v0.1.0-alpha.32"), Reply("current", "0.1.0-alpha.32 "),
            Reply("current", new string('9', 600))
        }) Rejected(response);
    }

    private static async Task AsyncCompletion()
    {
        var response = new TaskCompletionSource<UpdateProcessResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        int calls = 0;
        var bridge = new UpdateBridge(false, token => {
            ++calls;
            Require(token.CanBeCanceled, "Helper did not receive a bounded cancellation token.");
            return response.Task;
        });
        var checking = bridge.Check();
        Require(calls == 1 && !checking.IsCompleted, "Check blocked or completed before the asynchronous helper.");
        response.SetResult(new(0, Reply("requested"), ""));
        Require(await checking.WaitAsync(Guard) == new UpdateCheckResult(UpdateCheckStatus.Requested, Version), "Asynchronous helper result was lost.");

        var denied = new UpdateBridge(false, _ => Task.FromResult(new UpdateProcessResult(1, Reply("current"), "signature failed")));
        await Fails<InvalidOperationException>(denied.Check());
        var malformed = new UpdateBridge(false, _ => Task.FromResult(new UpdateProcessResult(0, Reply("installed"), "")));
        await Fails<InvalidOperationException>(malformed.Check());
        var offline = new UpdateBridge(false, _ => Task.FromException<UpdateProcessResult>(new IOException("offline")));
        await Fails<IOException>(offline.Check());
    }

    private static async Task DeadlineAndCancellation()
    {
        var ended = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        CancellationToken observed = default;
        var slow = new UpdateBridge(false, async token => {
            observed = token;
            try { await Task.Delay(Timeout.InfiniteTimeSpan, token); }
            finally { ended.TrySetResult(); }
            throw new InvalidOperationException("Canceled helper resumed normally.");
        }, TimeSpan.FromMilliseconds(50));
        await Fails<TimeoutException>(slow.Check());
        await ended.Task.WaitAsync(Guard);
        Require(observed.IsCancellationRequested, "Timeout did not cancel the helper work.");

        // A stuck helper must not keep Settings waiting after its own deadline.
        var stuckResult = new TaskCompletionSource<UpdateProcessResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        var stuck = new UpdateBridge(false, _ => stuckResult.Task, TimeSpan.FromMilliseconds(50));
        try { await Fails<TimeoutException>(stuck.Check()); }
        finally { stuckResult.TrySetResult(new(0, Reply("current"), "")); }

        using var caller = new CancellationTokenSource();
        var canceledWork = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var cancellable = new UpdateBridge(false, async token => {
            try { await Task.Delay(Timeout.InfiniteTimeSpan, token); }
            finally { canceledWork.TrySetResult(); }
            throw new InvalidOperationException("Canceled helper resumed normally.");
        });
        var pending = cancellable.Check(caller.Token);
        caller.Cancel();
        await Fails<OperationCanceledException>(pending);
        await canceledWork.Task.WaitAsync(Guard);
    }

    private static async Task MissingHelper()
    {
        var helper = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "HiveDeskUpdater.exe"));
        Require(!File.Exists(helper), "Isolated test output unexpectedly contains a real updater; refusing to launch it.");
        var error = await Fails<InvalidOperationException>(new UpdateBridge(false).Check());
        Require(error.Message.Contains("missing", StringComparison.OrdinalIgnoreCase), "Missing packaged helper was not explained.");
    }

    internal static async Task<int> Main()
    {
        try
        {
            Protocol();
            await AsyncCompletion();
            await DeadlineAndCancellation();
            await MissingHelper();
            Console.WriteLine("PASS settings update bridge: LF/CRLF verified states, strict protocol/version rejection, async completion, failed helper, timeout/stuck-runner bound, caller cancellation, missing helper; no network, installed updater or UI interaction.");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine("FAIL settings update bridge: " + error);
            return 1;
        }
    }
}
