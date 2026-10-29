using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Windowing;
using Windows.Graphics;
using Microsoft.UI.Xaml.Markup;

namespace HiveDesk.Settings;

internal sealed class SettingsWindow : Window
{
    private readonly AgentBridge bridge;
    private readonly TextBox name = new() { Header = "Computer name", MaxLength = 128, IsSpellCheckEnabled = false };
    private readonly ToggleSwitch startup = new() { Header = "Start with Windows", OnContent = "On", OffContent = "Off" };
    private readonly ToggleSwitch tray = new() { Header = "Show tray icon", OnContent = "On", OffContent = "Off" };
    private readonly ToggleSwitch debug = new() { Header = "Debug overlay", OnContent = "On", OffContent = "Off" };
    private readonly Button shortcut = new() { MinWidth = 180, HorizontalAlignment = HorizontalAlignment.Left };
    private readonly TextBlock saveStatus = Hint("Saved automatically");
    private readonly Button retrySave = new() { Content = "Retry" };
    private readonly Button discardChanges = new() { Content = "Close without saving" };
    private readonly DispatcherTimer saveTimer = new() { Interval = TimeSpan.FromMilliseconds(350) };
    private readonly SemaphoreSlim saveGate = new(1, 1);
    private Func<Preferences, Task> persistSettings;
    private bool loadingPreferences, preferencesReady, closingSettings, closeAllowed;
    private readonly Button checkUpdates = new() { Content = "Check for updates" };
    private readonly CheckBox gpuPerformance = new() { Content = "Prefer maximum GPU performance", IsEnabled = false };
    private readonly CheckBox relativeMouse = new() { Content = "Relative mouse" };
    private readonly TextBlock gpuPolicyHint = Hint("Checking GPU policy...");
    private readonly Button retryGpuRestart = new() { Content = "Retry restart" };
    private readonly InfoBar feedback = new() { IsOpen = false, IsClosable = true };
    private static readonly string VersionText = "v" + UpdateBridge.CurrentVersion;
    private readonly TextBlock version = Hint(VersionText);
    private UpdateBridge updates;
    private GpuPolicyBridge gpuPolicy;
    private GpuPolicyResult? gpuPolicyState;
    private Action requestVideoRestart, closeAfterRestart;
    private Preferences? savedPreferences;
    private bool changingGpuPolicy;
    private readonly CancellationTokenSource updateCancellation = new();
    private bool checkingUpdates, closed;
    private uint modifiers = 3, key = 72;
    private bool recording;
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr window);
    private static TextBlock Label(string text, string style = "BodyTextBlockStyle") => new() { Text = text, Style = (Style)Application.Current.Resources[style], TextWrapping = TextWrapping.Wrap };
    private static StackPanel Stack(double spacing, params UIElement[] children) { var result = new StackPanel { Spacing = spacing }; foreach (var child in children) result.Children.Add(child); return result; }
    private static Border Card(UIElement content) { var result = (Border)XamlReader.Load("<Border xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Padding='16' CornerRadius='8' BorderThickness='1' Background='{ThemeResource CardBackgroundFillColorDefaultBrush}' BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}' />"); result.Child = content; return result; }
    private static TextBlock Hint(string text) { var result = Label(text, "CaptionTextBlockStyle"); result.Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"]; return result; }
    private static void Id(DependencyObject item, string id) => AutomationProperties.SetAutomationId(item, id);
    internal SettingsWindow(bool preview)
    {
        bridge = new AgentBridge(preview); bridge.Sender = WinRT.Interop.WindowNative.GetWindowHandle(this);
        persistSettings = bridge.Save;
        updates = new UpdateBridge(preview);
        gpuPolicy = new GpuPolicyBridge(preview);
        requestVideoRestart = () => bridge.Send("restart-video");
        closeAfterRestart = () => { if (!preview) Close(); };
        Title = preview ? "HiveDesk settings - Preview" : "HiveDesk settings";
        if (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22000)) SystemBackdrop = new MicaBackdrop();
        Id(name, "ComputerName"); Id(startup, "Startup"); Id(tray, "TrayIcon"); Id(shortcut, "SummonHotkey"); Id(debug, "DebugOverlay"); Id(saveStatus, "AutoSaveStatus"); Id(feedback, "Feedback"); Id(checkUpdates, "CheckForUpdates");
        Id(gpuPerformance, "GpuMaximumPerformance"); Id(gpuPolicyHint, "GpuPolicyHint"); Id(retryGpuRestart, "RetryGpuRestart");
        var root = (Grid)XamlReader.Load("<Grid xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Background='{ThemeResource ApplicationPageBackgroundThemeBrush}' />");
        if (SystemBackdrop != null) root.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(40) }); root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        var titlebar = new Grid { Padding = new Thickness(16, 0, 140, 0), Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        var brand = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, VerticalAlignment = VerticalAlignment.Center };
        brand.Children.Add(new Image { Source = new BitmapImage(new Uri(Path.Combine(AppContext.BaseDirectory, "Assets", "HiveDesk.png"))), Width = 22, Height = 22 });
        brand.Children.Add(new TextBlock { Text = "HiveDesk", FontSize = 12, VerticalAlignment = VerticalAlignment.Center });
        titlebar.Children.Add(brand);
        root.Children.Add(titlebar); ExtendsContentIntoTitleBar = true; SetTitleBar(titlebar);
        void CaptionTheme() { AppWindow.TitleBar.ButtonBackgroundColor = Microsoft.UI.Colors.Transparent; AppWindow.TitleBar.ButtonInactiveBackgroundColor = Microsoft.UI.Colors.Transparent; AppWindow.TitleBar.ButtonForegroundColor = root.ActualTheme == ElementTheme.Dark ? Microsoft.UI.Colors.White : Microsoft.UI.Colors.Black; }
        root.ActualThemeChanged += (_, _) => CaptionTheme(); CaptionTheme();
        var content = new StackPanel { Padding = new Thickness(28, 16, 28, 20), Spacing = 12, MaxWidth = 700, HorizontalAlignment = HorizontalAlignment.Stretch };
        var heading = new Grid { ColumnSpacing = 16 };
        heading.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        heading.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        heading.Children.Add(Label("Settings", "TitleTextBlockStyle"));
        version.HorizontalAlignment = HorizontalAlignment.Right; version.VerticalAlignment = VerticalAlignment.Center; Id(version, "AppVersion");
        var release = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12, VerticalAlignment = VerticalAlignment.Center };
        release.Children.Add(version); release.Children.Add(checkUpdates); Grid.SetColumn(release, 1);
        heading.Children.Add(release); content.Children.Add(heading); content.Children.Add(Card(name));
        content.Children.Add(Card(Stack(12, startup, tray, Hint("With the icon hidden, open HiveDesk again for Settings."))));
        var tools = new Grid { ColumnSpacing = 24 }; tools.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) }); tools.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        tools.Children.Add(Stack(10, Label("Show Hive"), shortcut));
        var mouseAndDebug = Stack(0, debug, relativeMouse); Grid.SetColumn(mouseAndDebug, 1); tools.Children.Add(mouseAndDebug);
        Id(relativeMouse, "RelativeMouse");
        relativeMouse.IsEnabled = bridge.RelativeMouseAvailable();
        content.Children.Add(Card(Stack(8, tools, Stack(0, gpuPerformance, gpuPolicyHint))));
        var footer = new Grid { ColumnSpacing = 16 }; footer.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) }); footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var escape = Hint("Ctrl + Alt + `  back one level"); ToolTipService.SetToolTip(escape, "Ctrl + Alt + Backspace returns directly to this computer.\nCtrl + Shift + Alt + ` disconnects its controller."); escape.VerticalAlignment = VerticalAlignment.Center; footer.Children.Add(escape); Grid.SetColumn(saveStatus, 1); footer.Children.Add(saveStatus);
        content.Children.Add(footer); content.Children.Add(feedback);
        saveTimer.Tick += async (_, _) => { saveTimer.Stop(); await Save(); };
        retrySave.Click += async (_, _) => await Save();
        discardChanges.Click += (_, _) => { closeAllowed = true; Close(); };
        name.TextChanged += (_, _) => QueueSave(true);
        name.LostFocus += async (_, _) => { if (preferencesReady && !loadingPreferences) await Save(); };
        startup.Toggled += (_, _) => QueueSave(); tray.Toggled += (_, _) => QueueSave(); debug.Toggled += (_, _) => QueueSave();
        relativeMouse.Checked += (_, _) => QueueSave(); relativeMouse.Unchecked += (_, _) => QueueSave();
        checkUpdates.Click += async (_, _) => await CheckForUpdates();
        gpuPerformance.Click += async (_, _) => await ChangeGpuPolicy(gpuPolicyState?.State == GpuPolicyState.Conflict ? false : gpuPerformance.IsChecked == true);
        retryGpuRestart.Click += async (_, _) => await RetryGpuRestart();
        name.KeyDown += async (_, args) => { if (args.Key == Windows.System.VirtualKey.Enter) { args.Handled = true; await Save(); } };
        shortcut.Click += (_, _) => { try { if (!recording) { bridge.Send("record", "1"); recording = true; shortcut.Content = "Press a shortcut..."; } } catch (Exception error) { Show(error.Message, InfoBarSeverity.Error); } };
        shortcut.KeyDown += (_, args) => {
            if (!recording) return; args.Handled = true;
            var code = (uint)args.Key; if (code == 27) { EndRecording(); return; }
            uint mods = (uint)((GetAsyncKeyState(0x11) < 0 ? 2 : 0) | (GetAsyncKeyState(0x12) < 0 ? 1 : 0) | (GetAsyncKeyState(0x10) < 0 ? 4 : 0) | ((GetAsyncKeyState(0x5b) < 0 || GetAsyncKeyState(0x5c) < 0) ? 8 : 0));
            if (ValidHotkey(mods, code)) { modifiers = mods; key = code; EndRecording(); QueueSave(); }
        };
        shortcut.LostFocus += (_, _) => EndRecording();
        AppWindow.Closing += async (_, args) => {
            if (closeAllowed || closed || !preferencesReady) return;
            args.Cancel = true;
            if (closingSettings) return;
            closingSettings = true;
            await Task.Yield(); // Finish the native Closing notification first.
            if (await Save()) { closeAllowed = true; Close(); }
            else feedback.ActionButton = discardChanges;
            closingSettings = false;
        };
        Closed += (_, _) => { saveTimer.Stop(); EndRecording(); closed = true; updateCancellation.Cancel(); updateCancellation.Dispose(); };
        var scroll = new ScrollViewer { Content = content, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled }; Grid.SetRow(scroll, 1); root.Children.Add(scroll); Content = root;
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "HiveDesk.ico"));
        if (AppWindow.Presenter is OverlappedPresenter presenter) { presenter.IsResizable = false; presenter.IsMaximizable = false; presenter.IsMinimizable = true; }
        // Fixed logical size, scaled for this display. A smaller work area keeps
        // the window reachable; the existing scroll viewer handles overflow.
        double scale = GetDpiForWindow(bridge.Sender) / 96.0;
        var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        AppWindow.ResizeClient(new SizeInt32(Math.Min((int)Math.Round(580 * scale), area.Width - 32), Math.Min((int)Math.Round(700 * scale), area.Height - 64)));
        try { Load(bridge.Load()); preferencesReady = true; if(!bridge.HotkeyAvailable())Show("Show Hive shortcut is already in use. Choose another shortcut.",InfoBarSeverity.Warning); } catch (Exception error) { Show(error.Message, InfoBarSeverity.Error); saveStatus.Text = "Unavailable"; }
        root.Loaded += async (_, _) => await ReadGpuPolicy();
        if (App.Arguments.Contains("--self-test")) root.Loaded += async (_, _) => await SelfTest(root);
    }
    private static bool ValidHotkey(uint mods, uint code) => mods > 0 && mods <= 15 && (mods & 11) != 0 && ((code >= 65 && code <= 90) || (code >= 48 && code <= 57) || (code >= 112 && code <= 122));
    private string ShortcutText() => ((modifiers & 2) != 0 ? "Ctrl + " : "") + ((modifiers & 1) != 0 ? "Alt + " : "") + ((modifiers & 4) != 0 ? "Shift + " : "") + ((modifiers & 8) != 0 ? "Win + " : "") + (key >= 112 && key <= 122 ? "F" + (key - 111) : ((char)key).ToString());
    private void Load(Preferences value) { loadingPreferences = true; try { name.Text = value.Name; startup.IsOn = value.Startup; tray.IsOn = value.TrayIcon; debug.IsOn = value.DebugOverlay; relativeMouse.IsChecked = value.RelativeMouse; modifiers = value.HotkeyModifiers; key = value.HotkeyKey; shortcut.Content = ShortcutText(); savedPreferences = value; } finally { loadingPreferences = false; } }
    private void EndRecording() { if (!recording) return; recording = false; try { bridge.Send("record", "0"); } catch (Exception error) { Show(error.Message, InfoBarSeverity.Error); } shortcut.Content = ShortcutText(); }
    private Preferences Values() { var text = name.Text.Trim(); if (string.IsNullOrWhiteSpace(text) || System.Text.Encoding.UTF8.GetByteCount(text) > 256) throw new InvalidOperationException("Enter a computer name of up to 256 UTF-8 bytes."); if (!ValidHotkey(modifiers, key)) throw new InvalidOperationException("Use Ctrl, Alt or Win with a letter, number, or F1-F11."); return new(text, startup.IsOn, tray.IsOn, modifiers, key, debug.IsOn, relativeMouse.IsChecked == true); }
    private void Show(string message, InfoBarSeverity severity) { feedback.ActionButton = null; feedback.Message = message; feedback.Severity = severity; feedback.IsOpen = true; }
    private void QueueSave(bool debounce = false) {
        if (!preferencesReady || loadingPreferences || closed || changingGpuPolicy) return;
        saveTimer.Stop(); saveStatus.Text = "Saving...";
        if (debounce) saveTimer.Start(); else _ = Save();
    }
    private async Task<bool> Save() {
        saveTimer.Stop(); EndRecording();
        await saveGate.WaitAsync();
        try {
            if (closed || !preferencesReady) return false;
            // One writer, latest values: changes made during an asynchronous
            // acknowledgement are saved next, never overwritten by an old save.
            for (;;) {
                var value = Values();
                if (value == savedPreferences) break;
                saveStatus.Text = "Saving...";
                await persistSettings(value);
                savedPreferences = value;
                if (closed) return false;
            }
            saveStatus.Text = "Saved automatically";
            if (feedback.ActionButton == retrySave || feedback.ActionButton == discardChanges) feedback.IsOpen = false;
            return true;
        } catch (Exception error) {
            if (!closed) { saveStatus.Text = "Not saved"; Show(error.Message, InfoBarSeverity.Error); feedback.ActionButton = retrySave; }
            return false;
        } finally { saveGate.Release(); }
    }
    private void DisplayGpuPolicy(GpuPolicyResult? result)
    {
        gpuPolicyState = result;
        gpuPerformance.IsChecked = result?.State switch { GpuPolicyState.Enabled => true, GpuPolicyState.Disabled => false, _ => null };
        bool recoverable = result?.State == GpuPolicyState.Conflict && result.Code == "interrupted";
        gpuPerformance.IsEnabled = !changingGpuPolicy && (result?.State is GpuPolicyState.Enabled or GpuPolicyState.Disabled || recoverable);
        gpuPolicyHint.Text = result?.State switch
        {
            GpuPolicyState.Enabled or GpuPolicyState.Disabled => "This PC only. Settings reopens after HiveDesk restarts when idle.",
            GpuPolicyState.Conflict when recoverable => "Previous change was interrupted. Click to restore the original policy.",
            _ => result?.Message ?? "GPU policy is unavailable. Reopen Settings to check again."
        };
    }
    private async Task ReadGpuPolicy()
    {
        try
        {
            var result = await gpuPolicy.Read(updateCancellation.Token);
            if (!closed)
            {
                DisplayGpuPolicy(result);
                if (result.RestartRequired)
                {
                    Show("GPU policy is saved. Restart HiveDesk to apply it when remote connections are idle.", InfoBarSeverity.Informational);
                    feedback.ActionButton = retryGpuRestart;
                }
            }
        }
        catch (OperationCanceledException) when (closed) { }
        catch (Exception error) { if (!closed) { DisplayGpuPolicy(null); gpuPolicyHint.Text = error.Message; } }
    }
    private async Task ChangeGpuPolicy(bool enabled, bool forceRestart = false)
    {
        if (closed || changingGpuPolicy) return;
        DisplayGpuPolicy(gpuPolicyState);
        EndRecording(); changingGpuPolicy = true; gpuPerformance.IsEnabled = false;
        name.IsEnabled = startup.IsEnabled = tray.IsEnabled = debug.IsEnabled = shortcut.IsEnabled = false;
        relativeMouse.IsEnabled = false;
        var previousGpuPolicy = gpuPolicyState;
        try
        {
            if (!await Save()) return;
            Show("Changing GPU policy. Windows may request administrator permission...", InfoBarSeverity.Informational);
            var result = await gpuPolicy.Set(enabled, updateCancellation.Token);
            if (closed) return;
            DisplayGpuPolicy(result);
            if (!result.Succeeded)
            {
                // Driver save can succeed before journal/readback fails. Even a
                // structured rejection therefore needs independent reconciliation.
                await ReadGpuPolicy();
                if (!closed) ShowGpuPolicyFailure(result.Message, previousGpuPolicy);
                return;
            }
            if (result.State != (enabled ? GpuPolicyState.Enabled : GpuPolicyState.Disabled))
            { Show(result.Message, InfoBarSeverity.Warning); return; }
            if (result.RestartRequired || forceRestart) RequestGpuRestart();
            else Show(enabled ? "Maximum GPU performance is selected." : "The original GPU policy is selected.", InfoBarSeverity.Success);
        }
        catch (OperationCanceledException) when (closed) { }
        catch (Exception error)
        {
            if (!closed)
            {
                // A timeout may happen after a committed write. Re-read actual
                // driver state rather than claim that a failed request rolled back.
                await ReadGpuPolicy();
                if (!closed) ShowGpuPolicyFailure(error.Message, previousGpuPolicy);
            }
        }
        finally
        {
            changingGpuPolicy = false;
            if (!closed)
            {
                name.IsEnabled = startup.IsEnabled = tray.IsEnabled = debug.IsEnabled = shortcut.IsEnabled = true;
                relativeMouse.IsEnabled = bridge.RelativeMouseAvailable();
                DisplayGpuPolicy(gpuPolicyState);
            }
        }
    }
    private void ShowGpuPolicyFailure(string message, GpuPolicyResult? previous)
    {
        bool readable = gpuPolicyState?.State is GpuPolicyState.Enabled or GpuPolicyState.Disabled;
        bool savedChange = readable && (gpuPolicyState!.RestartRequired || previous?.State != gpuPolicyState.State);
        Show(message + (savedChange ? " Restart HiveDesk to apply the saved change." : ""), InfoBarSeverity.Error);
        if (savedChange) feedback.ActionButton = retryGpuRestart;
    }
    private async Task RetryGpuRestart()
    {
        if (gpuPolicyState?.State is not (GpuPolicyState.Enabled or GpuPolicyState.Disabled)) return;
        // Finish an interrupted undo journal before restarting. A read-only
        // status followed only by IPC would show the same recovery notice after
        // every restart. Idempotent writes may return restart=0, so this action
        // explicitly retains the user's restart request.
        await ChangeGpuPolicy(gpuPolicyState.State == GpuPolicyState.Enabled, forceRestart: true);
    }
    private void RequestGpuRestart()
    {
        try
        {
            if (savedPreferences == null || Values() != savedPreferences)
                throw new InvalidOperationException("Unsaved settings changed.");
        }
        catch (Exception)
        {
            Show("Settings have not finished saving. Retry the restart after they are saved.", InfoBarSeverity.Warning);
            feedback.ActionButton = retryGpuRestart;
            return;
        }
        try
        {
            requestVideoRestart();
            Show(bridge.Preview ? "Preview GPU policy saved." : "Saved. HiveDesk will restart when remote connections are idle.", InfoBarSeverity.Success);
            closeAfterRestart();
        }
        catch (Exception)
        {
            Show("Policy saved; could not request restart. Try again when HiveDesk is running.", InfoBarSeverity.Warning);
            feedback.ActionButton = retryGpuRestart;
        }
    }
    private async Task CheckForUpdates()
    {
        if (closed || checkingUpdates) return;
        EndRecording(); checkingUpdates = true; checkUpdates.IsEnabled = false; checkUpdates.Content = "Checking...";
        Show("Checking for updates...", InfoBarSeverity.Informational);
        try
        {
            var result = await updates.Check(updateCancellation.Token);
            if (closed) return;
            switch (result.Status)
            {
                case UpdateCheckStatus.Current: Show("You're up to date.", InfoBarSeverity.Success); break;
                case UpdateCheckStatus.Requested:
                    Show($"Update {result.Version} requested. It will install when remote connections are idle.", InfoBarSeverity.Informational); break;
                case UpdateCheckStatus.Unavailable:
                    Show($"Update {result.Version} is available, but the automatic updater is unavailable. Try again shortly.", InfoBarSeverity.Warning); break;
            }
        }
        catch (OperationCanceledException) when (closed) { }
        catch (Exception error) { if (!closed) Show(error.Message, InfoBarSeverity.Error); }
        finally
        {
            checkingUpdates = false;
            if (!closed) { checkUpdates.IsEnabled = true; checkUpdates.Content = "Check for updates"; }
        }
    }
    private async Task SelfTest(Grid root)
    {
        string report;
        try {
            await Task.Delay(300);
            int textFields = 0; void Inspect(DependencyObject node) { if (node is TextBox) ++textFields; if (node is Button && AutomationProperties.GetAutomationId(node) == "SaveSettings") throw new Exception("Manual Save button remains"); if (AutomationProperties.GetAutomationId(node) == "DisplayCadence" || node is CheckBox box && box.Content is string label && label.Contains("cadence", StringComparison.OrdinalIgnoreCase)) throw new Exception("Retired cadence control remains"); if (node is NumberBox || node is PasswordBox || node is Expander) throw new Exception("Manual connection/performance control remains"); for (int i = 0; i < VisualTreeHelper.GetChildrenCount(node); ++i) Inspect(VisualTreeHelper.GetChild(node, i)); } Inspect(root);
            if (textFields != 1) throw new Exception("Expected only the computer name text field");
            if (!relativeMouse.IsEnabled || relativeMouse.ActualWidth <= 0 || AutomationProperties.GetAutomationId(relativeMouse) != "RelativeMouse") throw new Exception("Relative mouse checkbox missing");
            var mouseBounds = relativeMouse.TransformToVisual(root).TransformBounds(new Windows.Foundation.Rect(0, 0, relativeMouse.ActualWidth, relativeMouse.ActualHeight));
            if (mouseBounds.Right > root.ActualWidth - 12 || mouseBounds.Bottom > root.ActualHeight - 12) throw new Exception("Relative mouse checkbox clips");
            var saveBounds = saveStatus.TransformToVisual(root).TransformBounds(new Windows.Foundation.Rect(0, 0, saveStatus.ActualWidth, saveStatus.ActualHeight));
            if (saveStatus.ActualWidth <= 0 || saveBounds.Bottom > root.ActualHeight - 12 || saveBounds.Right > root.ActualWidth - 12) throw new Exception("Automatic-save status clips");
            if (version.Text != VersionText || version.ActualWidth <= 0 || !Version.TryParse(VersionText[1..], out _)) throw new Exception("Packaged version label missing");
            if (checkUpdates.ActualWidth <= 0 || AutomationProperties.GetAutomationId(checkUpdates) != "CheckForUpdates") throw new Exception("Update action missing");
            var versionBounds = version.TransformToVisual(root).TransformBounds(new Windows.Foundation.Rect(0, 0, version.ActualWidth, version.ActualHeight));
            var updateBounds = checkUpdates.TransformToVisual(root).TransformBounds(new Windows.Foundation.Rect(0, 0, checkUpdates.ActualWidth, checkUpdates.ActualHeight));
            if (versionBounds.Right > updateBounds.Left || updateBounds.Right > root.ActualWidth - 12) throw new Exception("Version/update action overlaps or clips");
            string Reply(string status) => $"HiveDesk-Update-Check-1\nstatus={status}\nversion=0.1.0-alpha.99\n";
            var pending = new TaskCompletionSource<UpdateProcessResult>(TaskCreationOptions.RunContinuationsAsynchronously);
            int checks = 0;
            updates = new UpdateBridge(false, _ => { ++checks; return pending.Task; });
            var checking = CheckForUpdates();
            if (checkUpdates.IsEnabled || !checkingUpdates || feedback.Message != "Checking for updates...") throw new Exception("Update busy state missing");
            await CheckForUpdates(); if (checks != 1) throw new Exception("Duplicate update request started");
            pending.SetResult(new(0, Reply("current"), "")); await checking;
            if (!checkUpdates.IsEnabled || checkingUpdates || feedback.Severity != InfoBarSeverity.Success) throw new Exception("Current-version result failed");
            updates = new UpdateBridge(false, _ => Task.FromResult(new UpdateProcessResult(0, Reply("requested"), ""))); await CheckForUpdates();
            if (feedback.Severity != InfoBarSeverity.Informational || !feedback.Message.Contains("remote connections are idle")) throw new Exception("Idle update result missing");
            updates = new UpdateBridge(false, _ => Task.FromResult(new UpdateProcessResult(0, Reply("unavailable"), ""))); await CheckForUpdates();
            if (feedback.Severity != InfoBarSeverity.Warning || !checkUpdates.IsEnabled) throw new Exception("Unavailable updater result failed");
            updates = new UpdateBridge(false, _ => Task.FromResult(new UpdateProcessResult(1, Reply("current"), "network failure"))); await CheckForUpdates();
            if (feedback.Severity != InfoBarSeverity.Error || !checkUpdates.IsEnabled || checkingUpdates) throw new Exception("Update failure was hidden or left action disabled");
            if (gpuPerformance.ActualWidth <= 0 || AutomationProperties.GetAutomationId(gpuPerformance) != "GpuMaximumPerformance") throw new Exception("GPU policy checkbox missing");
            var gpuBounds = gpuPerformance.TransformToVisual(root).TransformBounds(new Windows.Foundation.Rect(0, 0, gpuPerformance.ActualWidth, gpuPerformance.ActualHeight));
            if (gpuBounds.Left < 12 || gpuBounds.Right > root.ActualWidth - 12 || gpuBounds.Bottom > root.ActualHeight - 12) throw new Exception("GPU policy checkbox clips or requires scrolling");
            string GpuReply(string state, bool restart = false, string code = "ok") => $"HiveDesk-Gpu-Policy-1\nstatus={state}\nrestart={(restart ? 1 : 0)}\ncode={code}\nmessage=GPU policy test result.\n";
            int gpuWrites = 0, restarts = 0, closes = 0;
            requestVideoRestart = () => ++restarts; closeAfterRestart = () => ++closes;
            var gpuPending = new TaskCompletionSource<GpuPolicyProcessResult>(TaskCreationOptions.RunContinuationsAsynchronously);
            gpuPolicy = new GpuPolicyBridge(false, (_, _) => { ++gpuWrites; return gpuPending.Task; });
            var changing = ChangeGpuPolicy(true);
            if (!changingGpuPolicy || gpuPerformance.IsEnabled || name.IsEnabled || gpuPerformance.IsChecked != false) throw new Exception("GPU policy pending state was optimistic or editable");
            await ChangeGpuPolicy(false); if (gpuWrites != 1) throw new Exception("Duplicate GPU policy change started");
            gpuPending.SetResult(new(1, GpuReply("disabled"), "denied")); await changing;
            if (changingGpuPolicy || !gpuPerformance.IsEnabled || gpuPerformance.IsChecked != false || feedback.Severity != InfoBarSeverity.Error || restarts != 0) throw new Exception("Rejected GPU change was not rolled back to actual state");
            if (feedback.ActionButton != null || feedback.Message.Contains("Restart HiveDesk")) throw new Exception("Rejected unchanged GPU save incorrectly offered restart");
            gpuPolicy = new GpuPolicyBridge(false, (command, _) => Task.FromResult(command == GpuPolicyCommand.Status
                ? new GpuPolicyProcessResult(0, GpuReply("disabled"), "")
                : new GpuPolicyProcessResult(2, GpuReply("unavailable", code: "elevation_cancelled"), "")));
            await ChangeGpuPolicy(true);
            if (gpuPerformance.IsChecked != false || feedback.ActionButton != null || restarts != 0 || feedback.Message.Contains("Restart HiveDesk")) throw new Exception("Cancelled permission request changed state or offered restart");
            gpuPolicy = new GpuPolicyBridge(false, (_, _) => Task.FromResult(new GpuPolicyProcessResult(0, GpuReply("enabled", true), "")));
            name.Text = " "; await ChangeGpuPolicy(true);
            if (gpuPerformance.IsChecked != false || feedback.Severity != InfoBarSeverity.Error || restarts != 0) throw new Exception("GPU change bypassed invalid preferences");
            name.Text = "Pending GPU name"; await ChangeGpuPolicy(true);
            if (bridge.Load().Name != "Pending GPU name") throw new Exception("GPU restart lost the pending automatic save");
            if (gpuPerformance.IsChecked != true || feedback.Severity != InfoBarSeverity.Success || restarts != 1 || closes != 1) throw new Exception("Verified GPU enable did not queue idle restart and close Settings");
            requestVideoRestart = () => throw new InvalidOperationException("Owner unavailable");
            gpuPolicy = new GpuPolicyBridge(false, (_, _) => Task.FromResult(new GpuPolicyProcessResult(0, GpuReply("disabled", true), ""))); await ChangeGpuPolicy(false);
            if (gpuPerformance.IsChecked != false || feedback.Severity != InfoBarSeverity.Warning || feedback.ActionButton != retryGpuRestart || closes != 1) throw new Exception("Saved policy was reverted or restart failure hidden");
            GpuPolicyCommand? retryCommand = null;
            gpuPolicy = new GpuPolicyBridge(false, (command, _) => { retryCommand = command; return Task.FromResult(new GpuPolicyProcessResult(0, GpuReply("disabled"), "")); });
            requestVideoRestart = () => ++restarts; await RetryGpuRestart();
            if (restarts != 2 || closes != 2 || feedback.ActionButton != null || retryCommand != GpuPolicyCommand.Restore) throw new Exception("GPU restart retry did not finalize journal or restart an idempotent restore");
            gpuPolicy = new GpuPolicyBridge(false, (command, _) => command == GpuPolicyCommand.Status
                ? Task.FromResult(new GpuPolicyProcessResult(0, GpuReply("enabled"), ""))
                : Task.FromException<GpuPolicyProcessResult>(new IOException("Helper output interrupted.")));
            await ChangeGpuPolicy(true);
            if (gpuPerformance.IsChecked != true || feedback.Severity != InfoBarSeverity.Error || feedback.ActionButton != retryGpuRestart) throw new Exception("Uncertain GPU write did not re-read actual policy and offer restart");
            gpuPolicy = new GpuPolicyBridge(false, (command, _) => Task.FromResult(command == GpuPolicyCommand.Status
                ? new GpuPolicyProcessResult(0, GpuReply("enabled", true), "")
                : new GpuPolicyProcessResult(1, GpuReply("unavailable", code: "write_failed"), "")));
            await ChangeGpuPolicy(true);
            if (gpuPerformance.IsChecked != true || feedback.Severity != InfoBarSeverity.Error || feedback.ActionButton != retryGpuRestart) throw new Exception("Post-commit structured error lost verified state or restart recovery");
            gpuPolicy = new GpuPolicyBridge(false, (_, _) => Task.FromResult(new GpuPolicyProcessResult(0, GpuReply("conflict", code: "interrupted"), ""))); await ReadGpuPolicy();
            if (gpuPerformance.IsChecked != null || !gpuPerformance.IsEnabled) throw new Exception("Recoverable GPU transaction state missing");
            gpuPolicy = new GpuPolicyBridge(false, (_, _) => Task.FromResult(new GpuPolicyProcessResult(1, GpuReply("unavailable", code: "no_driver"), ""))); await ReadGpuPolicy();
            if (gpuPerformance.IsChecked != null || gpuPerformance.IsEnabled) throw new Exception("Unsupported GPU was shown as a working toggle");
            name.Text = "Studio 2"; startup.IsOn = false; tray.IsOn = false; debug.IsOn = true; relativeMouse.IsChecked = true; modifiers = 6; key = 74; shortcut.Content = ShortcutText(); await Save();
            if (saveStatus.Text != "Saved automatically" || bridge.Load() != new Preferences("Studio 2", false, false, 6, 74, true, true)) throw new Exception("Preference save failed");
            Load(bridge.Load()); if (relativeMouse.IsChecked != true) throw new Exception("Relative mouse preference did not reload");
            relativeMouse.IsChecked = false; await Save(); if (bridge.Load().RelativeMouse) throw new Exception("Relative mouse preference did not disable");
            name.Text = " "; await Save(); if (feedback.Severity != InfoBarSeverity.Error) throw new Exception("Blank name accepted"); name.Text = "Studio 2"; modifiers = 0; await Save(); if (feedback.Severity != InfoBarSeverity.Error) throw new Exception("Unmodified shortcut accepted"); modifiers = 3; key = 72; shortcut.Content = ShortcutText();
            await Task.Delay(100); await Save(); // Drain queued XAML toggle notifications from the setup above.
            async Task WaitFor(Func<bool> condition) { for (int i = 0; i < 150 && !condition(); ++i) await Task.Delay(20); if (!condition()) throw new Exception("Automatic save did not settle"); }
            int writes = 0;
            persistSettings = async value => { ++writes; await bridge.Save(value); };
            name.Text = "Typing 1"; await Task.Delay(40); name.Text = "Typing 2"; await Task.Delay(40); name.Text = "Typing done";
            if (writes != 0) throw new Exception("Name editing was not debounced");
            await WaitFor(() => bridge.Load().Name == "Typing done");
            if (writes != 1) throw new Exception("Name editing wrote intermediate text");
            startup.IsOn = !startup.IsOn; tray.IsOn = !tray.IsOn; debug.IsOn = !debug.IsOn;
            relativeMouse.IsChecked = true;
            await WaitFor(() => bridge.Load() == Values());
            var firstWrite = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            writes = 0; int activeWrites = 0, maxWrites = 0;
            persistSettings = async value => {
                ++writes; ++activeWrites; maxWrites = Math.Max(maxWrites, activeWrites);
                if (writes == 1) await firstWrite.Task;
                await bridge.Save(value); --activeWrites;
            };
            name.Text = "First pending"; await WaitFor(() => writes == 1);
            name.Text = "Latest pending"; startup.IsOn = !startup.IsOn; tray.IsOn = !tray.IsOn;
            firstWrite.SetResult(); await WaitFor(() => bridge.Load() == Values() && saveStatus.Text == "Saved automatically");
            if (maxWrites != 1 || writes != 2) throw new Exception("Concurrent automatic saves reordered or queued intermediate values");
            var lastGood = bridge.Load(); persistSettings = _ => Task.FromException(new IOException("Generated save failure"));
            name.Text = "Retry name"; await WaitFor(() => saveStatus.Text == "Not saved");
            if (bridge.Load() != lastGood || feedback.Severity != InfoBarSeverity.Error || feedback.ActionButton != retrySave) throw new Exception("Failed automatic save lost persisted data or retry feedback");
            persistSettings = bridge.Save; await Save();
            if (bridge.Load().Name != "Retry name" || feedback.IsOpen) throw new Exception("Automatic-save retry failed");
            name.Text = "Close flush"; await Save(); // Same immediate flush used by AppWindow.Closing.
            if (bridge.Load().Name != "Close flush") throw new Exception("Close flush lost debounced text");
            if (AppWindow.Presenter is not OverlappedPresenter presenter || presenter.IsResizable || presenter.IsMaximizable || !presenter.IsMinimizable) throw new Exception("Window is not fixed-size with minimize retained");
            if (root.ActualWidth <= 0 || name.ActualWidth <= 0 || name.ActualWidth > root.ActualWidth || !File.Exists(Path.Combine(AppContext.BaseDirectory, "Assets", "HiveDesk.ico"))) throw new Exception("Fixed layout or packaged icon failed");
            report = "PASS real WinUI 3: no Save button, automatic toggles, debounced name, serialized latest-value saves, failure/retry, close flush, pending-save protection before GPU restart; one name field, shortcut validation, fixed size, minimize retained, packaged icon; update action states and guards; GPU policy actual-state rollback/readback, idle restart/close and retry, conflict recovery; theme=" + root.ActualTheme;
        } catch (Exception error) { report = "FAIL WinUI 3: " + error.Message; Environment.ExitCode = 1; }
        string output = App.Arguments.FirstOrDefault(x => x.StartsWith("--report="))?[9..] ?? Path.Combine(Path.GetTempPath(), "HiveDesk-settings-test.txt"); File.WriteAllText(output, report); closeAllowed = true; Close();
    }
}
