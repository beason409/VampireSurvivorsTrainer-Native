using System.Diagnostics;
using System.Reflection;
using System.Text.Json;
using System.Text.RegularExpressions;
using Trainer.Core;
using Trainer.Desktop;

internal static class Program
{
    private static readonly List<string> Checks = new();

    [STAThread]
    private static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.ThrowException);
        string root = Path.GetFullPath(Path.Combine("tests", "Trainer.Desktop.About.Tests", ".artifacts", Guid.NewGuid().ToString("N")));
        string data = Path.Combine(root, "Users", "PrivateOwner_隐私", "中文用户", "data");
        Directory.CreateDirectory(data);
        RuntimePaths.Root = data;
        try
        {
            Directory.CreateDirectory(RuntimePaths.PayloadDirectory);
            string lockedFile = Path.Combine(RuntimePaths.PayloadDirectory, "winhttp.dll");
            File.WriteAllBytes(lockedFile, Array.Empty<byte>());
            using var locked = new FileStream(lockedFile, FileMode.Open, FileAccess.Read, FileShare.None);
            // Prove the ordinary filesystem error contains the private path;
            // the dialog must replace it rather than displaying its Message.
            string rawError = "";
            try { TrustedPayloadCatalog.LoadEmbedded().VerifyPayload(RuntimePaths.PayloadDirectory); }
            catch (Exception error) { rawError = error.Message; }
            if (!rawError.Contains(data, StringComparison.OrdinalIgnoreCase)) throw new Exception("Expected a fixture path in the raw verification error: " + rawError);
            Check(true, "verification fixture produces a native error containing the private path");

            foreach (string initialLanguage in new[] { "zh-CN", "en" })
            {
                UiLocalization.SetLanguage(initialLanguage);
                string? opened = null;
                bool failOpen = false;
                using var dialog = new ReleaseInfoDialog(data, target =>
                {
                    opened = target;
                    if (failOpen) throw new IOException("Private native error: " + data);
                });
                dialog.ShowInTaskbar = false;
                dialog.StartPosition = FormStartPosition.Manual;
                dialog.Location = new Point(-32000, -32000);
                dialog.Show(); Application.DoEvents();
                AssertPrivate(dialog, data, initialLanguage + " initial UI");
                Check(!Descendants(dialog).Any(control => control.Name == "DataDirectory"), initialLanguage + " has no visible data-path control");

                var open = FindButton(dialog, "打开数据目录");
                Check(open.Enabled, initialLanguage + " can open an existing local data folder");
                open.PerformClick();
                Check(opened == data, initialLanguage + " passes the original path only to the internal opener");
                failOpen = true;
                open.PerformClick(); Application.DoEvents();
                string expectedOpenError = UiLocalization.Translate("未能打开。请检查本机访问权限后重试。");
                Check(Verification(dialog).Text == expectedOpenError, initialLanguage + " opening failure uses a fixed localized message");
                AssertPrivate(dialog, data, initialLanguage + " opening error UI");

                var verify = FindButton(dialog, "校验发行文件");
                verify.PerformClick();
                PumpUntil(() => verify.Enabled);
                string expectedVerificationError = UiLocalization.Translate("校验未完成。请确认发行包已完整解压，并从项目发行页重新下载后重试。");
                Check(Verification(dialog).Text == expectedVerificationError, initialLanguage + " verification failure uses a fixed localized message");
                AssertPrivate(dialog, data, initialLanguage + " verification error UI");

                foreach (string language in new[] { "en", "zh-CN", "en" })
                {
                    UiLocalization.SetLanguage(language); Application.DoEvents();
                    AssertPrivate(dialog, data, initialLanguage + " -> " + language);
                    if (language == "en")
                        Check(Descendants(dialog).All(control => !Regex.IsMatch(control.Text, "[\\u3400-\\u9fff]")), initialLanguage + " English UI contains no untranslated Chinese");
                }
                Check(opened == data, initialLanguage + " language switching leaves the internal opening target unchanged");
                if (args.Contains("--screenshots"))
                {
                    string output = Path.GetFullPath("artifacts/about-privacy"); Directory.CreateDirectory(output);
                    foreach (string language in new[] { "zh-CN", "en" })
                    {
                        UiLocalization.SetLanguage(language); Application.DoEvents();
                        dialog.RenderPreview(Path.Combine(output, "about-" + language + ".png"));
                    }
                }
            }

            const string unavailable = @"C:\Users\OtherPrivateOwner_隐私\中文用户\TrainerData";
            using (var dialog = new ReleaseInfoDialog(unavailable, _ => throw new Exception("must not open")))
            {
                AssertPrivate(dialog, unavailable, "unavailable user directory");
                Check(!FindButton(dialog, "打开数据目录").Enabled, "unavailable data folder keeps the open button disabled");
            }
            CheckConfiguredRepository(data, args.Contains("--screenshots"));
            string report = Path.GetFullPath(args.FirstOrDefault(arg => !arg.StartsWith("--", StringComparison.Ordinal)) ?? "artifacts/about-privacy/regression.json");
            Directory.CreateDirectory(Path.GetDirectoryName(report)!);
            File.WriteAllText(report, JsonSerializer.Serialize(new { passed = Checks.Count, failed = 0, gameConnected = false, shellStarted = false, checks = Checks }, new JsonSerializerOptions { WriteIndented = true }));
            Console.WriteLine($"{Checks.Count}/{Checks.Count} About privacy checks passed.");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }

    private static TextBox Verification(ReleaseInfoDialog dialog) => (TextBox)typeof(ReleaseInfoDialog)
        .GetField("_verification", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(dialog)!;

    private static Button FindButton(Control root, string source) => Descendants(root).OfType<Button>()
        .Single(button => UiLocalization.GetSource(button) == source);

    private static void CheckConfiguredRepository(string data, bool screenshots)
    {
        const string repository = "https://github.com/beason409/VampireSurvivorsTrainer-Native";
        // ReleaseInfo reads only this test program's output directory. Preserve
        // any prior fixture and never edit the packaged application's sidecar.
        string sidecar = Path.Combine(AppContext.BaseDirectory, "project-info.json");
        byte[]? original = File.Exists(sidecar) ? File.ReadAllBytes(sidecar) : null;
        try
        {
            File.WriteAllText(sidecar, JsonSerializer.Serialize(new { repositoryUrl = repository }));
            Check(ReleaseInfo.RepositoryUri?.AbsoluteUri == repository, "configured repository URL is loaded exactly");
            foreach (string initialLanguage in new[] { "zh-CN", "en" })
            {
                UiLocalization.SetLanguage(initialLanguage);
                var opened = new List<string>();
                using var dialog = new ReleaseInfoDialog(data, opened.Add)
                {
                    ShowInTaskbar = false,
                    StartPosition = FormStartPosition.Manual,
                    Location = new Point(-32000, -32000)
                };
                dialog.Show(); Application.DoEvents();
                var source = FindButton(dialog, "查看源码");
                Check(source.Enabled, initialLanguage + " configured repository enables the source button");
                AssertRepositoryText(dialog, repository, initialLanguage + " configured initial UI");
                AssertPrivate(dialog, data, initialLanguage + " configured initial UI");
                source.PerformClick();
                Check(opened.SequenceEqual(new[] { repository }), initialLanguage + " source button passes only the configured URL to the injected opener");
                foreach (string language in new[] { initialLanguage == "en" ? "zh-CN" : "en", initialLanguage })
                {
                    UiLocalization.SetLanguage(language); Application.DoEvents();
                    AssertRepositoryText(dialog, repository, initialLanguage + " configured -> " + language);
                    AssertPrivate(dialog, data, initialLanguage + " configured -> " + language);
                }
                Check(opened.Count == 1, initialLanguage + " configured language roundtrip does not open any location");
                if (screenshots)
                {
                    string output = Path.GetFullPath("artifacts/about-privacy"); Directory.CreateDirectory(output);
                    dialog.RenderPreview(Path.Combine(output, "configured-about-" + initialLanguage + ".png"));
                }
            }
        }
        finally
        {
            if (original is null) File.Delete(sidecar);
            else File.WriteAllBytes(sidecar, original);
        }
    }

    private static void AssertRepositoryText(ReleaseInfoDialog dialog, string repository, string label)
    {
        string text = Descendants(dialog).OfType<TextBox>().Single(control => control.Text.Contains(repository, StringComparison.Ordinal)).Text;
        bool localized = UiLocalization.IsEnglish
            ? text.Contains("Source and license", StringComparison.Ordinal)
                && text.Contains("Author credit: Beason9527", StringComparison.Ordinal)
                && text.Contains("Project homepage: " + repository, StringComparison.Ordinal)
                && Descendants(dialog).All(control => !Regex.IsMatch(control.Text, "[\\u3400-\\u9fff]"))
            : text.Contains("来源与许可", StringComparison.Ordinal)
                && text.Contains("作者署名：Beason9527", StringComparison.Ordinal)
                && text.Contains("项目主页：" + repository, StringComparison.Ordinal);
        Check(localized, label + " localizes the source/license section and homepage prefix without changing the URL");
    }

    private static IEnumerable<Control> Descendants(Control root)
    {
        yield return root;
        foreach (Control child in root.Controls)
            foreach (Control descendant in Descendants(child)) yield return descendant;
    }

    private static void AssertPrivate(ReleaseInfoDialog dialog, string privatePath, string label)
    {
        string[] markers = { privatePath, "PrivateOwner_隐私", "OtherPrivateOwner_隐私", "中文用户" };
        var fields = typeof(ReleaseInfoDialog).GetFields(BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public);
        var tips = fields.Select(field => field.GetValue(dialog)).OfType<ToolTip>().ToArray();
        foreach (Control control in Descendants(dialog))
        {
            var values = new[] { control.Text, control.Name, control.AccessibleName, control.AccessibleDescription,
                control.AccessibleDefaultActionDescription, control.AccessibilityObject.Name, control.AccessibilityObject.Description,
                control.AccessibilityObject.Value }.Concat(tips.Select(tip => tip.GetToolTip(control)));
            if (values.Any(value => value is not null && markers.Any(marker => value.Contains(marker, StringComparison.OrdinalIgnoreCase))))
                throw new Exception(label + ": private path found on " + control.GetType().Name);
        }
        Check(true, label + " hides private paths from text, tooltips, and accessibility");
    }

    private static void PumpUntil(Func<bool> complete)
    {
        var watch = Stopwatch.StartNew();
        do
        {
            Application.DoEvents(); Thread.Sleep(1);
            if (watch.Elapsed > TimeSpan.FromSeconds(5)) throw new TimeoutException("Verification did not finish.");
        } while (!complete());
    }

    private static void Check(bool condition, string name)
    {
        if (!condition) throw new Exception(name);
        Checks.Add(name);
    }
}
