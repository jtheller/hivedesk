using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Windowing;
using Windows.Graphics;
using Microsoft.UI.Xaml.Markup;

namespace HiveDesk.Settings;

public partial class App : Application
{
    private Window? window;
    internal static string[] Arguments => Environment.GetCommandLineArgs();
    public App()
    {
        InitializeComponent();
        if (Arguments.Contains("--dark")) RequestedTheme = ApplicationTheme.Dark;
        if (Arguments.Contains("--light")) RequestedTheme = ApplicationTheme.Light;
        UnhandledException += (_, args) =>
        {
            var directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HiveDesk");
            Directory.CreateDirectory(directory);
            File.WriteAllText(Path.Combine(directory, "settings-error.txt"), args.Exception.GetType().Name + ": " + args.Message);
        };
    }
    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        window = new SettingsWindow(Arguments.Contains("--preview") || Arguments.Contains("--self-test"));
        window.Activate();
    }
}

