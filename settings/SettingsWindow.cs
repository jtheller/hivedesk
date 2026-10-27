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
    private readonly ToggleSwitch startup = new() { Header = "Start when I sign in", OnContent = "On", OffContent = "Off" };
    private readonly ToggleSwitch tray = new() { Header = "Show tray icon", OnContent = "On", OffContent = "Off" };
    private readonly ToggleSwitch debug = new() { Header = "Debug overlay", OnContent = "On", OffContent = "Off" };
    private readonly Button shortcut = new() { MinWidth = 180, HorizontalAlignment = HorizontalAlignment.Left };
    private readonly Button save = new() { Content = "Save", MinWidth = 104 };
    private readonly Button checkUpdates = new() { Content = "Check for updates" };
    private readonly InfoBar feedback = new() { IsOpen = false, IsClosable = true };
    private static readonly string VersionText = "v" + UpdateBridge.CurrentVersion;
    private readonly TextBlock version = Hint(VersionText);
    private UpdateBridge updates;
    private readonly CancellationTokenSource updateCancellation = new();
    private bool checkingUpdates, closed;
    private uint modifiers = 3, key = 72;
    private bool recording;
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern short GetAsyncKeyState(int key);
    [System.Runtime.InteropServices.DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr window);
    private static TextBlock Label(string text, string style = "BodyTextBlockStyle") => new() { Text = text, Style = (Style)Application.Current.Resources[style], TextWrapping = TextWrapping.Wrap };
    private static StackPanel Stack(double spacing, params UIElement[] children) { var result = new StackPanel { Spacing = spacing }; foreach (var child in children) result.Children.Add(child); return result; }
    private static Border Card(UIElement content) { var result = (Border)XamlReader.Load("<Border xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Padding='20' CornerRadius='8' BorderThickness='1' Background='{ThemeResource CardBackgroundFillColorDefaultBrush}' BorderBrush='{ThemeResource CardStrokeColorDefaultBrush}' />"); result.Child = content; return result; }
    private static TextBlock Hint(string text) { var result = Label(text, "CaptionTextBlockStyle"); result.Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"]; return result; }
    private static void Id(DependencyObject item, string id) => AutomationProperties.SetAutomationId(item, id);
    internal SettingsWindow(bool preview)
    {
        bridge = new AgentBridge(preview); bridge.Sender = WinRT.Interop.WindowNative.GetWindowHandle(this);
        updates = new UpdateBridge(preview);
        Title = preview ? "HiveDesk settings - Preview" : "HiveDesk settings";
        if (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22000)) SystemBackdrop = new MicaBackdrop();
        Id(name, "ComputerName"); Id(startup, "Startup"); Id(tray, "TrayIcon"); Id(shortcut, "SummonHotkey"); Id(debug, "DebugOverlay"); Id(save, "SaveSettings"); Id(feedback, "Feedback"); Id(checkUpdates, "CheckForUpdates");
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
        var content = new StackPanel { Padding = new Thickness(28, 20, 28, 28), Spacing = 16, MaxWidth = 700, HorizontalAlignment = HorizontalAlignment.Stretch };
        var heading = new Grid { ColumnSpacing = 16 };
        heading.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        heading.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        heading.Children.Add(Label("Settings", "TitleTextBlockStyle"));
        version.HorizontalAlignment = HorizontalAlignment.Right; version.VerticalAlignment = VerticalAlignment.Center; Id(version, "AppVersion");
        var release = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 12, VerticalAlignment = VerticalAlignment.Center };
        release.Children.Add(version); release.Children.Add(checkUpdates); Grid.SetColumn(release, 1);
        heading.Children.Add(release); content.Children.Add(heading); content.Children.Add(Card(name));
        content.Children.Add(Card(Stack(18, startup, tray, Hint("With the icon hidden, open HiveDesk again for Settings."))));
        var tools = new Grid { ColumnSpacing = 24 }; tools.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) }); tools.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        tools.Children.Add(Stack(10, Label("Show Hive"), shortcut)); Grid.SetColumn(debug, 1); tools.Children.Add(debug); content.Children.Add(Card(tools));
        var footer = new Grid { ColumnSpacing = 16 }; footer.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) }); footer.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var escape = Hint("Ctrl + Alt + `  returns here"); escape.VerticalAlignment = VerticalAlignment.Center; footer.Children.Add(escape); Grid.SetColumn(save, 1); footer.Children.Add(save);
        content.Children.Add(footer); content.Children.Add(feedback);
        save.Style = (Style)Application.Current.Resources["AccentButtonStyle"]; save.Click += async (_, _) => await Save();
        checkUpdates.Click += async (_, _) => await CheckForUpdates();
        name.KeyDown += async (_, args) => { if (args.Key == Windows.System.VirtualKey.Enter) { args.Handled = true; await Save(); } };
        shortcut.Click += (_, _) => { try { if (!recording) { bridge.Send("record", "1"); recording = true; shortcut.Content = "Press a shortcut..."; } } catch (Exception error) { Show(error.Message, InfoBarSeverity.Error); } };
        shortcut.KeyDown += (_, args) => {
            if (!recording) return; args.Handled = true;
            var code = (uint)args.Key; if (code == 27) { EndRecording(); return; }
            uint mods = (uint)((GetAsyncKeyState(0x11) < 0 ? 2 : 0) | (GetAsyncKeyState(0x12) < 0 ? 1 : 0) | (GetAsyncKeyState(0x10) < 0 ? 4 : 0) | ((GetAsyncKeyState(0x5b) < 0 || GetAsyncKeyState(0x5c) < 0) ? 8 : 0));
            if (ValidHotkey(mods, code)) { modifiers = mods; key = code; EndRecording(); }
        };
        shortcut.LostFocus += (_, _) => EndRecording();
        Closed += (_, _) => { EndRecording(); closed = true; updateCancellation.Cancel(); updateCancellation.Dispose(); };
        var scroll = new ScrollViewer { Content = content, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled }; Grid.SetRow(scroll, 1); root.Children.Add(scroll); Content = root;
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "HiveDesk.ico"));
        if (AppWindow.Presenter is OverlappedPresenter presenter) { presenter.IsResizable = false; presenter.IsMaximizable = false; presenter.IsMinimizable = true; }
        // Fixed logical size, scaled for this display. A smaller work area keeps
        // the window reachable; the existing scroll viewer handles overflow.
        double scale = GetDpiForWindow(bridge.Sender) / 96.0;
        var area = DisplayArea.GetFromWindowId(AppWindow.Id, DisplayAreaFallback.Primary).WorkArea;
        AppWindow.ResizeClient(new SizeInt32(Math.Min((int)Math.Round(580 * scale), area.Width - 32), Math.Min((int)Math.Round(700 * scale), area.Height - 64)));
        try { Load(bridge.Load()); if(!bridge.HotkeyAvailable())Show("Show Hive shortcut is already in use. Choose another shortcut.",InfoBarSeverity.Warning); } catch (Exception error) { Show(error.Message, InfoBarSeverity.Error); save.IsEnabled = false; }
        if (App.Arguments.Contains("--self-test")) root.Loaded += async (_, _) => await SelfTest(root);
    }
    private static bool ValidHotkey(uint mods, uint code) => mods > 0 && mods <= 15 && (mods & 11) != 0 && ((code >= 65 && code <= 90) || (code >= 48 && code <= 57) || (code >= 112 && code <= 122));
    private string ShortcutText() => ((modifiers & 2) != 0 ? "Ctrl + " : "") + ((modifiers & 1) != 0 ? "Alt + " : "") + ((modifiers & 4) != 0 ? "Shift + " : "") + ((modifiers & 8) != 0 ? "Win + " : "") + (key >= 112 && key <= 122 ? "F" + (key - 111) : ((char)key).ToString());
    private void Load(Preferences value) { name.Text = value.Name; startup.IsOn = value.Startup; tray.IsOn = value.TrayIcon; debug.IsOn = value.DebugOverlay; modifiers = value.HotkeyModifiers; key = value.HotkeyKey; shortcut.Content = ShortcutText(); }
    private void EndRecording() { if (!recording) return; recording = false; try { bridge.Send("record", "0"); } catch (Exception error) { Show(error.Message, InfoBarSeverity.Error); } shortcut.Content = ShortcutText(); }
    private Preferences Values() { var text = name.Text.Trim(); if (string.IsNullOrWhiteSpace(text) || System.Text.Encoding.UTF8.GetByteCount(text) > 256) throw new InvalidOperationException("Enter a computer name of up to 256 UTF-8 bytes."); if (!ValidHotkey(modifiers, key)) throw new InvalidOperationException("Use Ctrl, Alt or Win with a letter, number, or F1-F11."); return new(text, startup.IsOn, tray.IsOn, modifiers, key, debug.IsOn); }
    private void Show(string message, InfoBarSeverity severity) { feedback.Message = message; feedback.Severity = severity; feedback.IsOpen = true; }
    private async Task Save() { EndRecording(); save.IsEnabled = false; try { await bridge.Save(Values()); Show(bridge.Preview ? "Preview settings saved." : "Settings saved.", InfoBarSeverity.Success); } catch (Exception error) { Show(error.Message, InfoBarSeverity.Error); } finally { save.IsEnabled = true; } }
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
            int textFields = 0; void Inspect(DependencyObject node) { if (node is TextBox) ++textFields; if (node is NumberBox || node is PasswordBox || node is Expander) throw new Exception("Manual connection/performance control remains"); for (int i = 0; i < VisualTreeHelper.GetChildrenCount(node); ++i) Inspect(VisualTreeHelper.GetChild(node, i)); } Inspect(root);
            if (textFields != 1) throw new Exception("Expected only the computer name text field");
            if (version.Text != VersionText || version.ActualWidth <= 0 || !VersionText.StartsWith("v0.1.0-alpha.")) throw new Exception("Packaged version label missing");
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
            name.Text = "Studio 2"; startup.IsOn = false; tray.IsOn = false; debug.IsOn = true; modifiers = 6; key = 74; shortcut.Content = ShortcutText(); await Save();
            if (feedback.Severity != InfoBarSeverity.Success || bridge.Load() != new Preferences("Studio 2", false, false, 6, 74, true)) throw new Exception("Preference save failed");
            name.Text = " "; await Save(); if (feedback.Severity != InfoBarSeverity.Error) throw new Exception("Blank name accepted"); name.Text = "Studio 2"; modifiers = 0; await Save(); if (feedback.Severity != InfoBarSeverity.Error) throw new Exception("Unmodified shortcut accepted"); modifiers = 3; key = 72; shortcut.Content = ShortcutText();
            if (AppWindow.Presenter is not OverlappedPresenter presenter || presenter.IsResizable || presenter.IsMaximizable || !presenter.IsMinimizable) throw new Exception("Window is not fixed-size with minimize retained");
            if (root.ActualWidth <= 0 || name.ActualWidth <= 0 || name.ActualWidth > root.ActualWidth || !File.Exists(Path.Combine(AppContext.BaseDirectory, "Assets", "HiveDesk.ico"))) throw new Exception("Fixed layout or packaged icon failed");
            report = "PASS real WinUI 3: one name field, startup/tray toggles, shortcut persistence/validation, debug toggle persistence, fixed size, resize/maximize disabled, minimize retained, packaged icon; update action placement, busy/duplicate guard, current/requested/unavailable/failure results; theme=" + root.ActualTheme;
        } catch (Exception error) { report = "FAIL WinUI 3: " + error.Message; Environment.ExitCode = 1; }
        string output = App.Arguments.FirstOrDefault(x => x.StartsWith("--report="))?[9..] ?? Path.Combine(Path.GetTempPath(), "HiveDesk-settings-test.txt"); File.WriteAllText(output, report); Close();
    }
}
