using System.Diagnostics;

namespace Nhl3on3Launcher;

public sealed class LauncherForm : Form
{
    const uint TitleId = 0x58410975;  // 3 on 3 NHL Arcade
    const string GameExe = "nhl3on3.exe";

    readonly string _baseDir = AppContext.BaseDirectory;
    string GameDir => Path.Combine(_baseDir, "game");
    bool GameReady => File.Exists(Path.Combine(GameDir, "default.xex"));

    readonly Label _status = new() { AutoSize = true, MaximumSize = new Size(520, 0) };
    readonly TextBox _packagePath = new() { ReadOnly = true, Width = 420 };
    readonly Button _browse = new() { Text = "Browse...", AutoSize = true };
    readonly Button _setup = new() { Text = "Set up game", AutoSize = true };
    readonly Button _play = new() { Text = "Play", AutoSize = true, Font = new Font(SystemFonts.MessageBoxFont!.FontFamily, 12f, FontStyle.Bold) };
    readonly ProgressBar _progress = new() { Width = 520, Visible = false };

    public LauncherForm()
    {
        Text = "3 on 3 NHL Arcade";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        Padding = new Padding(16);

        var layout = new FlowLayoutPanel
        {
            FlowDirection = FlowDirection.TopDown,
            AutoSize = true,
            WrapContents = false,
        };
        layout.Controls.Add(new Label
        {
            Text = "3 on 3 NHL Arcade",
            AutoSize = true,
            Font = new Font(SystemFonts.MessageBoxFont!.FontFamily, 16f, FontStyle.Bold),
        });
        layout.Controls.Add(new Label
        {
            Text = "Your XBLA package (the file inside 58410975\\000D0000):",
            AutoSize = true,
            Margin = new Padding(0, 12, 0, 2),
        });
        var pathRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = Padding.Empty };
        pathRow.Controls.Add(_packagePath);
        pathRow.Controls.Add(_browse);
        layout.Controls.Add(pathRow);
        layout.Controls.Add(_setup);
        layout.Controls.Add(_progress);
        layout.Controls.Add(_status);
        _play.Margin = new Padding(0, 12, 0, 0);
        layout.Controls.Add(_play);
        Controls.Add(layout);

        _browse.Click += (_, _) => Browse();
        _setup.Click += async (_, _) => await SetupAsync();
        _play.Click += (_, _) => Play();

        if (!GameReady)
        {
            string? found = FindPackage(_baseDir);
            if (found != null) _packagePath.Text = found;
        }
        UpdateState();
    }

    void UpdateState(string? message = null)
    {
        bool ready = GameReady;
        bool haveGameExe = File.Exists(Path.Combine(_baseDir, GameExe));
        _play.Enabled = ready && haveGameExe;
        _setup.Enabled = _packagePath.Text.Length > 0;
        _setup.Text = ready ? "Set up again" : "Set up game";
        _status.Text = message
            ?? (!haveGameExe ? $"{GameExe} is missing from this folder."
            : ready ? "Ready to play."
            : _packagePath.Text.Length > 0 ? "Package found. Click \"Set up game\" to unpack it."
            : "Put your XBLA package next to this launcher, or click Browse.");
    }

    /// <summary>Looks for the game's STFS package in the launcher folder and a few levels below it.</summary>
    static string? FindPackage(string root)
    {
        var options = new EnumerationOptions { RecurseSubdirectories = true, MaxRecursionDepth = 4, IgnoreInaccessible = true };
        foreach (string file in Directory.EnumerateFiles(root, "*", options))
        {
            if (Path.GetExtension(file).Length > 0) continue;  // packages have no extension
            if (new FileInfo(file).Length < 1 << 20) continue;
            using var pkg = StfsPackage.TryOpen(file);
            if (pkg?.TitleId == TitleId) return file;
        }
        return null;
    }

    void Browse()
    {
        using var dialog = new OpenFileDialog
        {
            Title = "Select your 3 on 3 NHL Arcade XBLA package",
            Filter = "All files (*.*)|*.*",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        using var pkg = StfsPackage.TryOpen(dialog.FileName);
        if (pkg == null)
        {
            UpdateState("That file is not an Xbox 360 package.");
            return;
        }
        if (pkg.TitleId != TitleId)
        {
            UpdateState($"That package is \"{pkg.DisplayName}\" (title {pkg.TitleId:X8}), not 3 on 3 NHL Arcade.");
            return;
        }
        _packagePath.Text = dialog.FileName;
        UpdateState();
    }

    async Task SetupAsync()
    {
        string packagePath = _packagePath.Text;
        string staging = GameDir + ".partial";
        _setup.Enabled = _browse.Enabled = _play.Enabled = false;
        _progress.Visible = true;
        _progress.Value = 0;
        var progress = new Progress<(long Done, long Total)>(p =>
        {
            _progress.Value = p.Total == 0 ? 0 : (int)(p.Done * 100 / p.Total);
            _status.Text = $"Unpacking... {p.Done / (1 << 20)} / {p.Total / (1 << 20)} MB";
        });
        try
        {
            await Task.Run(() =>
            {
                using var pkg = StfsPackage.TryOpen(packagePath)
                    ?? throw new InvalidOperationException("The package can no longer be read.");
                if (pkg.TitleId != TitleId)
                    throw new InvalidOperationException("That package is not 3 on 3 NHL Arcade.");
                if (Directory.Exists(staging)) Directory.Delete(staging, true);
                pkg.ExtractAll(staging, progress, CancellationToken.None);
                string xex = Path.Combine(staging, "default.xex");
                if (!File.Exists(xex) || !File.ReadAllBytes(xex).AsSpan(0, 4).SequenceEqual("XEX2"u8))
                    throw new InvalidOperationException("The package unpacked, but default.xex is missing or invalid.");
                if (Directory.Exists(GameDir)) Directory.Delete(GameDir, true);
                Directory.Move(staging, GameDir);
            });
            UpdateState("Game set up. Ready to play.");
        }
        catch (Exception ex)
        {
            UpdateState("Setup failed: " + ex.Message);
        }
        finally
        {
            _progress.Visible = false;
            _browse.Enabled = true;
            _setup.Enabled = _packagePath.Text.Length > 0;
            _play.Enabled = GameReady && File.Exists(Path.Combine(_baseDir, GameExe));
        }
    }

    void Play()
    {
        try
        {
            Process.Start(new ProcessStartInfo(Path.Combine(_baseDir, GameExe)) { WorkingDirectory = _baseDir });
            Close();
        }
        catch (Exception ex)
        {
            UpdateState("Could not start the game: " + ex.Message);
        }
    }
}
