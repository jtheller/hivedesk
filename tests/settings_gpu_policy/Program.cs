using HiveDesk.Settings;

static class GpuPolicyChecks
{
    private static readonly TimeSpan Guard = TimeSpan.FromSeconds(5);
    private static string Reply(string state, bool restart = false, string code = "ok", string message = "Verified GPU policy.") =>
        $"HiveDesk-Gpu-Policy-1\nstatus={state}\nrestart={(restart ? 1 : 0)}\ncode={code}\nmessage={message}\n";
    private static void Require(bool value, string message) { if (!value) throw new InvalidOperationException(message); }
    private static async Task<T> Fails<T>(Task task) where T : Exception
    {
        try { await task.WaitAsync(Guard); }
        catch (TimeoutException) when (!task.IsCompleted) { throw new InvalidOperationException("Outer test deadline exceeded."); }
        catch (T error) { return error; }
        throw new InvalidOperationException($"Expected {typeof(T).Name}.");
    }
    private static void Rejected(string text, int exit = 0)
    {
        try { GpuPolicyBridge.Parse(text, exit); }
        catch (InvalidOperationException) { return; }
        throw new InvalidOperationException("Malformed GPU policy response accepted.");
    }
    private static void Protocol()
    {
        foreach (var (text, state) in new[] { ("enabled", GpuPolicyState.Enabled), ("disabled", GpuPolicyState.Disabled),
            ("unavailable", GpuPolicyState.Unavailable), ("conflict", GpuPolicyState.Conflict) })
            foreach (string newline in new[] { "\n", "\r\n" })
            {
                var result = GpuPolicyBridge.Parse(Reply(text).Replace("\n", newline));
                Require(result.State == state && result.Succeeded && !result.RestartRequired, "Verified status was lost.");
            }
        var rejected = GpuPolicyBridge.Parse(Reply("enabled", code: "access_denied", message: "Could not restore the original policy."), 1);
        Require(!rejected.Succeeded && rejected.State == GpuPolicyState.Enabled && rejected.Code == "access_denied", "Rejected write hid actual enabled state.");
        Require(GpuPolicyBridge.Parse(Reply("disabled", true)).RestartRequired, "Restore restart requirement was lost.");
        foreach (var value in new[] { "", "garbage", Reply("enabled").TrimEnd('\n'), Reply("enabled") + "\n",
            Reply("enabled") + "status=disabled\n", Reply("unknown"), Reply("ENABLED"),
            Reply("enabled").Replace("Policy-1", "Policy-2"), Reply("enabled").Replace("restart=0", "restart=2"),
            Reply("enabled").Replace("code=ok\nmessage=Verified GPU policy.", "message=Verified GPU policy.\ncode=ok"),
            Reply("enabled", code: "bad code"), Reply("enabled", code: "BadCode"), Reply("enabled", code: ""),
            Reply("enabled", code: new string('a', 65)), Reply("enabled", message: ""), Reply("enabled", message: "bad\0message"),
            Reply("enabled", message: "bad\tmessage"), Reply("enabled", message: "bad\nmessage"),
            Reply("enabled", message: new string('a', 1025)), new string('a', 2049),
            Reply("unavailable", true), Reply("conflict", true) }) Rejected(value);
        Rejected(Reply("enabled", true), 1);
    }
    private static async Task Behavior()
    {
        int calls = 0;
        var preview = new GpuPolicyBridge(true, (_, _) => { ++calls; throw new Exception("Preview reached a real runner."); });
        Require((await preview.Read()).State == GpuPolicyState.Disabled, "Preview baseline was changed.");
        Require((await preview.Set(true)).State == GpuPolicyState.Enabled, "Preview enable failed.");
        Require((await preview.Read()).State == GpuPolicyState.Enabled, "Preview lost its own state.");
        Require((await preview.Set(false)).State == GpuPolicyState.Disabled && calls == 0, "Preview called a helper.");

        var completion = new TaskCompletionSource<GpuPolicyProcessResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        GpuPolicyCommand requested = default;
        var bridge = new GpuPolicyBridge(false, (command, token) => { requested = command; Require(token.CanBeCanceled, "No bounded token."); return completion.Task; });
        var pending = bridge.Set(true);
        Require(requested == GpuPolicyCommand.Enable && !pending.IsCompleted, "Enable command blocked or completed optimistically.");
        completion.SetResult(new(0, Reply("enabled", true), ""));
        Require((await pending).RestartRequired, "Enable result lost restart state.");
        var failed = new GpuPolicyBridge(false, (_, _) => Task.FromResult(new GpuPolicyProcessResult(1, Reply("disabled", code: "denied", message: "Access denied."), "failure")));
        Require(!(await failed.Set(true)).Succeeded, "Nonzero helper exit accepted as a successful write.");
        var malformed = new GpuPolicyBridge(false, (_, _) => Task.FromResult(new GpuPolicyProcessResult(0, "bad output", "")));
        await Fails<InvalidOperationException>(malformed.Read());
        var stuck = new TaskCompletionSource<GpuPolicyProcessResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        try { await Fails<TimeoutException>(new GpuPolicyBridge(false, (_, _) => stuck.Task, TimeSpan.FromMilliseconds(40)).Read()); }
        finally { stuck.TrySetResult(new(0, Reply("disabled"), "")); }
        using var canceled = new CancellationTokenSource(); canceled.Cancel();
        await Fails<OperationCanceledException>(preview.Set(true, canceled.Token));
        Require(!File.Exists(GpuPolicyBridge.HelperPath), "Isolated test output unexpectedly contains the real GPU helper.");
        await Fails<InvalidOperationException>(new GpuPolicyBridge(false).Read());
    }
    private static async Task Processes()
    {
        string executable = Environment.ProcessPath ?? throw new InvalidOperationException("Missing fixture executable.");
        Require(Path.GetFileName(executable) == "HiveDesk.Settings.GpuPolicyCheck.exe", "Refusing to run an unexpected helper fixture.");
        foreach (var command in Enum.GetValues<GpuPolicyCommand>())
        {
            var result = await GpuPolicyBridge.RunHelper(executable, command, default).WaitAsync(Guard);
            var parsed = GpuPolicyBridge.Parse(result.Output, result.ExitCode);
            Require(parsed.State == (command == GpuPolicyCommand.Enable ? GpuPolicyState.Enabled : GpuPolicyState.Disabled), "Helper command argument was lost.");
            Require(parsed.RestartRequired == (command != GpuPolicyCommand.Status), "Process restart state was lost.");
        }
        Environment.SetEnvironmentVariable("HIVEDESK_GPU_FIXTURE_DELAY", "200");
        try
        {
            using var readCancellation = new CancellationTokenSource(TimeSpan.FromMilliseconds(40));
            await Fails<OperationCanceledException>(GpuPolicyBridge.RunHelper(executable, GpuPolicyCommand.Status, readCancellation.Token));
            Task<GpuPolicyProcessResult>? writing = null;
            var write = new GpuPolicyBridge(false, (command, token) => writing = GpuPolicyBridge.RunHelper(executable, command, token));
            using var writeCancellation = new CancellationTokenSource();
            var waiting = write.Set(true, writeCancellation.Token);
            await Task.Delay(40); writeCancellation.Cancel();
            await Fails<OperationCanceledException>(waiting);
            Require(writing != null && GpuPolicyBridge.Parse((await writing.WaitAsync(Guard)).Output).State == GpuPolicyState.Enabled,
                "Closing Settings aborted an in-flight driver transaction.");
        }
        finally { Environment.SetEnvironmentVariable("HIVEDESK_GPU_FIXTURE_DELAY", null); }
    }
    internal static async Task<int> Main(string[] arguments)
    {
        if (arguments.Length == 1 && arguments[0] is "--status" or "--enable" or "--restore")
        {
            if (int.TryParse(Environment.GetEnvironmentVariable("HIVEDESK_GPU_FIXTURE_DELAY"), out int delay)) await Task.Delay(delay);
            Console.Write(Reply(arguments[0] == "--enable" ? "enabled" : "disabled", arguments[0] != "--status"));
            return 0;
        }
        try
        {
            Protocol(); await Behavior(); await Processes();
            Console.WriteLine("PASS settings GPU policy: strict LF/CRLF protocol, actual-state errors, preview isolation, async completion, deadlines/cancellation, missing helper, subprocess arguments and read cancellation, in-flight write completion; no driver changes.");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine("FAIL settings GPU policy: " + error); return 1; }
    }
}
