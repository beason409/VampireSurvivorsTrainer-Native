using System.Diagnostics;
using System.Reflection;
using System.Text.Json;

internal static class Program
{
    private const BindingFlags Instance = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    [STAThread]
    private static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.ThrowException);
        string root = Path.GetFullPath(Path.Combine("tests", "Trainer.Desktop.Maintenance.Tests", ".artifacts", Guid.NewGuid().ToString("N")));
        Directory.CreateDirectory(root);
        string file = Path.Combine(root, "fake-installed.dll"); File.WriteAllText(file, "original");
        var checks = new List<string>();
        void Check(bool value, string name) { if (!value) throw new Exception(name); checks.Add(name); }
        try
        {
            var assembly = Assembly.Load("VampireSurvivorsTrainer");
            var type = assembly.GetType("Trainer.Desktop.MainForm", true)!;
            using var form = (Form)Activator.CreateInstance(type, new object[] { true })!;
            form.ShowInTaskbar = false; form.StartPosition = FormStartPosition.Manual; form.Location = new Point(-32000, -32000);
            form.Show(); Application.DoEvents();
            var pending = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            type.GetField("_pollIdle", Instance)!.SetValue(form, pending.Task);
            int generation = (int)type.GetField("_stateGeneration", Instance)!.GetValue(form)!;
            bool actionRan = false, duplicateRan = false;
            using (var read = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                var maintenance = type.GetMethod("MaintenanceAsync", Instance)!;
                Func<Task> action = () => { File.WriteAllText(file, "updated"); actionRan = true; return Task.CompletedTask; };
                var task = (Task)maintenance.Invoke(form, new object[] { "isolated maintenance test", action })!;
                Application.DoEvents();
                Check(!task.IsCompleted && !actionRan, "maintenance waits for the in-flight poll before file writes");
                Check((bool)type.GetField("_busy", Instance)!.GetValue(form)!, "maintenance marks the UI busy while the poll finishes");
                Check((int)type.GetField("_stateGeneration", Instance)!.GetValue(form)! > generation, "maintenance invalidates old poll results immediately");
                Func<Task> duplicate = () => { duplicateRan = true; return Task.CompletedTask; };
                var other = (Task)maintenance.Invoke(form, new object[] { "duplicate", duplicate })!;
                Check(other.IsCompleted && !duplicateRan, "a second maintenance action cannot overlap the first");
                Check(File.ReadAllText(file) == "original", "pending maintenance preserves the original bytes");
                read.Dispose();
                pending.SetResult();
                Pump(task);
                task.GetAwaiter().GetResult();
                Check(actionRan && File.ReadAllText(file) == "updated", "maintenance writes only after poll read handles are released");
                Check(!(bool)type.GetField("_busy", Instance)!.GetValue(form)!, "maintenance restores the idle UI after completion");
            }
            var client = type.GetField("_client", Instance)!.GetValue(form)!;
            Check(!(bool)client.GetType().GetProperty("IsConnected")!.GetValue(client)!, "diagnostic fixture never connects to a game");
            var settings = type.GetField("_diagnosticsMode", Instance)!.GetValue(form);
            Check(settings is true, "fixture uses diagnostic settings and no real installation action");
            string output = Path.GetFullPath(args.FirstOrDefault() ?? "artifacts/security-hardening/maintenance-regression.json");
            Directory.CreateDirectory(Path.GetDirectoryName(output)!);
            File.WriteAllText(output, JsonSerializer.Serialize(new { passed = checks.Count, failed = 0, gameConnected = false, checks }, new JsonSerializerOptions { WriteIndented = true }));
            Console.WriteLine($"{checks.Count}/{checks.Count} maintenance concurrency checks passed.");
            return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }

    private static void Pump(Task task)
    {
        var time = Stopwatch.StartNew();
        while (!task.IsCompleted)
        {
            if (time.Elapsed > TimeSpan.FromSeconds(5)) throw new TimeoutException("Maintenance did not resume.");
            Application.DoEvents(); Thread.Sleep(1);
        }
    }
}
