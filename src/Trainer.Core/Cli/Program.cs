using System.Text.Encodings.Web;
using System.Text.Json;
using Trainer.Core;

Console.OutputEncoding = System.Text.Encoding.UTF8;
try
{
    if (args.Length == 0 || args[0] is "help" or "--help")
    {
        Console.WriteLine("verify <game> <supported.json>\nbackup <storage> <game>\nproof <storage> <game>\nlist <storage>\ninstallation-status <storage> <game>\ninstall <storage> <game> <payload> <proof.json> <supported.json>\nuninstall <storage> <game>\nrestore <storage> <game> <manifest.json>");
        return 0;
    }
    var paths = new GamePaths();
    object? result = args[0] switch
    {
        "verify" when args.Length == 3 => FingerprintService.Verify(args[1], args[2]),
        "backup" when args.Length == 3 => new BackupService(paths, args[1]).CreateBackup(args[2]),
        "proof" when args.Length == 3 => new { ProofPath = new BackupService(paths, args[1]).LatestSessionProofPath(args[2]) },
        "list" when args.Length == 2 => new BackupService(paths, args[1]).ListBackups(),
        "installation-status" when args.Length == 3 => new { Installed = new InstallationService(paths, args[1]).IsInstalled(args[2]), ManifestPath = new InstallationService(paths, args[1]).GetManifestPath(args[2]) },
        "install" when args.Length == 6 => new InstallationService(paths, args[1]).Install(args[2], args[3], args[4], args[5]),
        "uninstall" when args.Length == 3 => new InstallationService(paths, args[1]).Uninstall(args[2]),
        "restore" when args.Length == 4 => new BackupService(paths, args[1]).Restore(args[2], args[3]),
        _ => throw new ArgumentException("命令或参数数量无效，请使用 --help 查看。")
    };
    Console.WriteLine(JsonSerializer.Serialize(result, new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase, WriteIndented = true, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));
    return result is VersionCheck { IsSupported: false } ? 2 : 0;
}
catch (Exception exception)
{
    Console.Error.WriteLine(exception.Message);
    return 1;
}
