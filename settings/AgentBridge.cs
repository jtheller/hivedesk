using System.Globalization;
using System.Runtime.InteropServices;

using System.Text;

namespace HiveDesk.Settings;

internal sealed record Preferences(string Name, bool Startup, bool TrayIcon, uint HotkeyModifiers, uint HotkeyKey, bool DebugOverlay = false, bool RelativeMouse = false);

internal sealed class AgentBridge(bool preview)
{
    private readonly string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "HiveDesk", "settings.ini");
    private Preferences demo = new("Studio Workstation", true, true, 3, 72);
    internal bool Preview => preview;
    internal nint Sender { get; set; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern uint GetPrivateProfileString(string section, string key, string fallback, StringBuilder value, uint size, string file);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern nint FindWindow(string cls, string? title);
    [DllImport("user32.dll")] private static extern nint SendMessageTimeout(nint window, uint message, nuint w, nint l, uint flags, uint timeout, out nuint result);
    [StructLayout(LayoutKind.Sequential)] private struct CopyData { public nuint Tag; public uint Size; public nint Data; }
    private string Read(string key, string fallback = "") { var text = new StringBuilder(16384); GetPrivateProfileString("HiveDesk", key, fallback, text, (uint)text.Capacity, path); return text.ToString(); }
    internal Preferences Load()
    {
        if (preview) return demo;
        return new(Read("Name"), Read("Startup", "1") == "1", Read("TrayIcon", "1") == "1",
            uint.Parse(Read("HotkeyModifiers", "3"), CultureInfo.InvariantCulture), uint.Parse(Read("HotkeyKey", "72"), CultureInfo.InvariantCulture), Read("DebugOverlay", "0") == "1", Read("RelativeMouse", "0") == "1");
    }
    internal bool HotkeyAvailable() {
        if(preview)return true;
        var owner=FindWindow("HiveDesk.Owner",null);
        return owner!=0&&SendMessageTimeout(owner,0x8000+46,0,0,2,2000,out var result)!=0&&result==1;
    }
    internal bool RelativeMouseAvailable() {
        if(preview)return true;
        var owner=FindWindow("HiveDesk.Owner",null);
        return owner!=0&&SendMessageTimeout(owner,0x8000+49,0,0,2,2000,out var result)!=0&&result==1;
    }
    internal void Send(string command, params string[] fields)
    {
        if (preview) return;
        if (fields.Any(x => x.Contains('\0'))) throw new InvalidOperationException("Invalid field value.");
        var owner = FindWindow("HiveDesk.Owner", null);
        if (owner == 0) throw new InvalidOperationException("HiveDesk is not running.");
        string packet = string.Join('\0', new[] { command }.Concat(fields)) + '\0';
        var data = Marshal.StringToHGlobalUni(packet); nint structure = 0;
        try
        {
            var copy = new CopyData { Tag = 0x48445631, Size = checked((uint)packet.Length * 2), Data = data };
            structure = Marshal.AllocHGlobal(Marshal.SizeOf<CopyData>()); Marshal.StructureToPtr(copy, structure, false);
            if (SendMessageTimeout(owner, 0x004A, (nuint)Sender, structure, 2, 3000, out var result) == 0 || result != 1)
                throw new InvalidOperationException("Could not save. The shortcut may already be in use; choose another combination.");
        }
        finally { if (structure != 0) Marshal.FreeHGlobal(structure); Marshal.FreeHGlobal(data); }
    }
    internal async Task Save(Preferences value)
    {
        if (preview) { demo = value; return; }
        await Task.Run(() => Send("preferences", value.Name, value.Startup ? "1" : "0", value.TrayIcon ? "1" : "0", value.HotkeyModifiers.ToString(CultureInfo.InvariantCulture), value.HotkeyKey.ToString(CultureInfo.InvariantCulture), value.DebugOverlay ? "1" : "0", value.RelativeMouse ? "1" : "0"));
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            await Task.Delay(100); var saved = Load();
            if (saved == value) return;
        }
        throw new InvalidOperationException("The change was submitted, but saving has not been confirmed. Check the HiveDesk log.");
    }
}
