using System.Text.Json;
using Trainer.Shared;

namespace Trainer.Probe;

internal static class CommandFiles
{
    public static bool IsReadOnly(string kind) => kind is "hello" or "heartbeat" or "getState" or "getGold" or "getItemCatalog" or "getItemIcon";

    public static List<Request> ReadReplay(string path)
    {
        string[] lines = File.ReadAllText(path).Split('\n', StringSplitOptions.TrimEntries);
        var commands = new List<Request>();
        for (int line = 0; line < lines.Length; line++)
        {
            if (string.IsNullOrWhiteSpace(lines[line])) continue;
            try { commands.Add(Parse(lines[line])); }
            catch (Exception error) when (error is JsonException or InvalidDataException)
            { throw new InvalidDataException($"Invalid commands file at line {line + 1}: {error.Message}", error); }
        }
        return commands;
    }

    internal static Request Parse(string line)
    {
        if (line.Length > PipeProtocol.MaxMessageCharacters) throw new InvalidDataException("Command exceeds protocol limit.");
        var request = JsonSerializer.Deserialize<Request>(line, PipeProtocol.JsonOptions) ?? throw new InvalidDataException("Command cannot be null.");
        if (string.IsNullOrWhiteSpace(request.Kind)) throw new InvalidDataException("Command kind cannot be empty.");
        if (string.IsNullOrWhiteSpace(request.Id)) request.Id = Guid.NewGuid().ToString("N");
        return request;
    }
}

internal sealed class CommandInbox
{
    private string _committedPrefix = "";
    private int _lineNumber;

    public IReadOnlyList<Request> Read(string snapshot, Action<string>? warning = null)
    {
        // A Set-Content reset or atomic replacement starts a new inbox; appends retain exactly-once
        // consumption for every committed line, including whitespace-only lines.
        if (!snapshot.StartsWith(_committedPrefix, StringComparison.Ordinal))
        {
            _committedPrefix = "";
            _lineNumber = 0;
        }
        int end = snapshot.LastIndexOf('\n');
        if (end < _committedPrefix.Length) return Array.Empty<Request>();
        string committed = snapshot[_committedPrefix.Length..(end + 1)];
        string[] lines = committed.Split('\n', StringSplitOptions.TrimEntries);
        var commands = new List<Request>();
        for (int index = 0; index < lines.Length - 1; index++)
        {
            _lineNumber++;
            if (string.IsNullOrWhiteSpace(lines[index])) continue;
            try { commands.Add(CommandFiles.Parse(lines[index])); }
            catch (Exception error) when (error is JsonException or InvalidDataException)
            { warning?.Invoke($"{_lineNumber}: {error.Message}"); }
        }
        _committedPrefix = snapshot[..(end + 1)];
        return commands;
    }
}

internal static class ProbeArguments
{
    public static Dictionary<string, string> Parse(string[] arguments)
    {
        var options = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var valueOptions = new HashSet<string>(new[] { "pid", "duration", "steps", "gameplay-steps", "inbox", "commandsfile", "token", "output" }, StringComparer.OrdinalIgnoreCase);
        for (int index = 0; index < arguments.Length; index++)
        {
            string argument = arguments[index];
            if (!argument.StartsWith("--", StringComparison.Ordinal)) throw new ArgumentException("Expected an option beginning with --: " + argument);
            string[] pair = argument[2..].Split('=', 2);
            string key = pair[0].ToLowerInvariant();
            if (key == "commands-file") key = "commandsfile";
            if (key is "abrupt" or "help") { options[key] = pair.Length == 2 ? pair[1] : "true"; continue; }
            if (!valueOptions.Contains(key)) throw new ArgumentException("Unknown option: --" + key);
            string value = pair.Length == 2 ? pair[1] : index + 1 < arguments.Length && !arguments[index + 1].StartsWith("--", StringComparison.Ordinal)
                ? arguments[++index] : throw new ArgumentException("Missing value for --" + key);
            if (string.IsNullOrWhiteSpace(value)) throw new ArgumentException("Empty value for --" + key);
            options[key] = value;
        }
        if (options.ContainsKey("steps") && options.ContainsKey("gameplay-steps"))
            throw new ArgumentException("Use either --steps (wall time) or --gameplay-steps (unpaused gameplay time).");
        if (options.ContainsKey("gameplay-steps") && options.ContainsKey("abrupt"))
            throw new ArgumentException("--gameplay-steps requires final disableAll cleanup and cannot use --abrupt.");
        return options;
    }
}
