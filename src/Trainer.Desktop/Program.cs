using Trainer.Core;

namespace Trainer.Desktop;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        if (args.Contains("--verify-companion-files", StringComparer.OrdinalIgnoreCase))
        {
            // This read-only entry point uses this app folder explicitly. It
            // never searches developer fallback folders, reads user settings,
            // connects to a game, creates a backup, or installs anything.
            try
            {
                var catalog = TrustedPayloadCatalog.LoadEmbedded();
                catalog.VerifyPayload(Path.Combine(AppContext.BaseDirectory, "payload"));
                catalog.ReadSupportedBuild(Path.Combine(AppContext.BaseDirectory, "supported-game.json"));
                Console.WriteLine("Companion file integrity verified.");
            }
            catch (Exception error)
            {
                Console.Error.WriteLine("Companion file integrity check failed: " + error.Message);
                Environment.ExitCode = 2;
            }
            return;
        }
        ApplicationConfiguration.Initialize();
        int previewArg = Array.FindIndex(args, value => value.Equals("--render-preview", StringComparison.OrdinalIgnoreCase)
            || value.Equals("--render-preview-stats", StringComparison.OrdinalIgnoreCase) || value.Equals("--render-preview-about", StringComparison.OrdinalIgnoreCase)
            || value.Equals("--render-preview-found", StringComparison.OrdinalIgnoreCase) || value.Equals("--render-preview-restart", StringComparison.OrdinalIgnoreCase)
            || value.Equals("--render-preview-multiple", StringComparison.OrdinalIgnoreCase) || value.Equals("--render-preview-chest", StringComparison.OrdinalIgnoreCase));
        bool diagnostics = args.Contains("--smoke-test", StringComparer.OrdinalIgnoreCase) || previewArg >= 0;
        if (diagnostics)
        {
            int languageArg = Array.FindIndex(args, value => value.Equals("--language", StringComparison.OrdinalIgnoreCase));
            UiLocalization.SetLanguage(languageArg >= 0 && languageArg + 1 < args.Length ? args[languageArg + 1] : "zh-CN");
            string? path = previewArg >= 0 && previewArg + 1 < args.Length ? args[previewArg + 1] : null;
            if (previewArg >= 0 && args[previewArg].Equals("--render-preview-about", StringComparison.OrdinalIgnoreCase))
            {
                using var about = new ReleaseInfoDialog(UiLocalization.IsEnglish ? "(application folder)\\data" : "（程序目录）\\data");
                about.RenderPreview(path);
                return;
            }
            // Render a hidden form without starting timers, connecting pipes, or reading/writing user settings or saves.
            using var preview = new MainForm(true);
            string? startupState = previewArg < 0 ? null
                : args[previewArg].Equals("--render-preview-chest", StringComparison.OrdinalIgnoreCase) ? "chest"
                : args[previewArg].Equals("--render-preview-restart", StringComparison.OrdinalIgnoreCase) ? "restart"
                : args[previewArg].Equals("--render-preview-multiple", StringComparison.OrdinalIgnoreCase) ? "multiple" : "found";
            preview.RenderPreview(path, previewArg >= 0 && args[previewArg].Equals("--render-preview-stats", StringComparison.OrdinalIgnoreCase), startupState);
            return;
        }
        UiLocalization.SetLanguage(DesktopSettings.Load(RuntimePaths.SettingsPath).Language);
        using var mutex = new Mutex(true, @"Local\VampireSurvivorsTrainer.Desktop.v1", out bool isNew);
        if (!isNew)
        {
            LocalizedMessageBox.Show("修改器已经在运行，请查看任务栏中的窗口。", "吸血鬼幸存者修改器", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        Application.ThreadException += (_, e) => LocalizedMessageBox.Show(e.Exception.Message, "操作未完成", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        using var form = new MainForm();
        Application.Run(form);
    }
}
