using System.Text.RegularExpressions;
using Trainer.Shared;
#if GAME_INTEROP
using VampireSurvivors.Data;
using VampireSurvivors.Data.Weapons;
using VampireSurvivors.Objects;
using VSMenu = VampireSurvivors.UI.MainMenuPage;
#endif

namespace Trainer.Plugin;

internal sealed partial class GameAdapter
{
#if GAME_INTEROP
    private IntPtr _rewardCatalogOwner;
    private int _rewardCatalogSourceCount;
    private string _rewardCatalogLanguage = "";
    private int _rewardCatalogEnglishNames;
    private IReadOnlyList<RewardItemDefinition> _rewardCatalog = Array.Empty<RewardItemDefinition>();
    private readonly Dictionary<string, RewardItemDefinition> _rewardCatalogById = new(StringComparer.Ordinal);
    private static readonly HashSet<string> NonEquipmentRewardIds = new(StringComparer.Ordinal)
    { "VOID", "CANDYBOX", "CANDYBOX2", "ARMADIO" };
#endif

    /// <summary>Called by the main-thread command queue; never reads save files or a bundled item table.</summary>
    internal IReadOnlyList<RewardItemDefinition> GetRewardItemCatalog()
    {
#if GAME_INTEROP
        if (!SafeMode) throw new InvalidOperationException("请等待单人主菜单或关卡就绪后读取物品列表。");
        var dataManager = _manager != null ? _manager.DataManager : UnityEngine.Object.FindObjectOfType<VSMenu>()?._dataManager;
        if (dataManager is null || dataManager.Pointer == IntPtr.Zero)
            throw new InvalidOperationException("游戏物品数据尚未就绪，请进入单人关卡后刷新列表。");
        var source = dataManager.GetConvertedWeapons();
        if (source is null || source.Count == 0)
            throw new InvalidOperationException("游戏物品数据仍在加载，请进入单人关卡后刷新列表。");
        string language = I2.Loc.LocalizationManager.CurrentLanguage ?? "";
        if (_rewardCatalogOwner == dataManager.Pointer && _rewardCatalogSourceCount == source.Count
            && string.Equals(_rewardCatalogLanguage, language, StringComparison.Ordinal) && _rewardCatalog.Count > 0
            && _rewardCatalogEnglishNames > 0)
            return _rewardCatalog;

        var englishLanguages = GetEnglishRewardLanguages();
        var found = new List<RewardItemDefinition>();
        int skipped = 0;
        foreach (var pair in source)
        {
            try
            {
                string id = pair.Key.ToString();
                if (!IsNamedRewardId(id) || NonEquipmentRewardIds.Contains(id)) continue;
                var levels = pair.Value;
                if (levels is null || levels.Count == 0) continue;
                WeaponData? header = null;
                int highestLevel = 0;
                for (int index = 0; index < levels.Count; index++)
                {
                    var level = levels[index];
                    if (level is null) continue;
                    header ??= level;
                    highestLevel = Math.Max(highestLevel, level.level);
                }
                if (header is null || header.alwaysHidden || (header.hidden && !header.isEvolution)
                    || (header.isSpecialOnly && !header.isEvolution) || string.IsNullOrWhiteSpace(header.frameName)) continue;
                // Loaded content is a capability boundary. Unlock/slot/banish state
                // is deliberately not used to hide a valid cheat target.
                // The native DLC helper returns false when a group has no
                // entry in its DLC mapping, including the base/free content.
                if (header.contentGroup is not (ContentGroupType.BASE or ContentGroupType.EXTRA)
                    && !ContentGroupMethods.IsDlcLoadedForContentGroup(header.contentGroup)) continue;
                string name = GetRewardName(header, pair.Key);
                if (string.IsNullOrWhiteSpace(name)) continue;
                found.Add(new RewardItemDefinition
                {
                    Id = id, Name = name, EnglishName = GetRewardEnglishName(header, pair.Key, englishLanguages),
                    Kind = header.isPowerUp ? "passive" : "weapon", IsEvolution = header.isEvolution,
                    MaxLevel = Math.Clamp(Math.Max(highestLevel, levels.Count), 1, 1000), Description = null,
                    // This describes the item's static selection capability;
                    // Current ownership/max level and run eligibility are
                    // validated separately when the target is applied.
                    UpgradeAllowed = !header.isEvolution || !header.isPowerUp,
                    UpgradeUnsupportedReason = header.isEvolution && header.isPowerUp
                        ? "本修改器尚未适配进化被动装备的升级指定，可仅用于箱子奖励。" : null
                });
            }
            catch (Exception)
            {
                // One invalid/inactive content entry cannot prevent the rest of
                // the catalog from loading. No native object is mutated here.
                skipped++;
            }
        }
        var complete = found.OrderBy(item => item.Kind, StringComparer.Ordinal)
            .ThenBy(item => item.Name, StringComparer.CurrentCulture)
            .ThenBy(item => item.Id, StringComparer.Ordinal).ToArray();
        if (complete.Length == 0)
            throw new InvalidOperationException("当前没有可用的武器或被动装备数据，请进入单人关卡后刷新列表。");
        _rewardCatalogById.Clear();
        foreach (var item in complete) _rewardCatalogById[item.Id] = item;
        _rewardCatalog = complete;
        _rewardCatalogOwner = dataManager.Pointer;
        _rewardCatalogSourceCount = source.Count;
        _rewardCatalogLanguage = language;
        // Do not freeze a startup snapshot with no English data: a later
        // Refresh must retry when the game's language source becomes ready.
        _rewardCatalogEnglishNames = complete.Count(item => !string.IsNullOrWhiteSpace(item.EnglishName));
        Diagnostics["chest.catalog.count"] = complete.Length.ToString(System.Globalization.CultureInfo.InvariantCulture);
        Diagnostics["chest.catalog.skipped"] = skipped.ToString(System.Globalization.CultureInfo.InvariantCulture);
        Diagnostics["chest.catalog.englishLanguageCandidates"] = englishLanguages.Count.ToString(System.Globalization.CultureInfo.InvariantCulture);
        Diagnostics["chest.catalog.englishNames"] = _rewardCatalogEnglishNames.ToString(System.Globalization.CultureInfo.InvariantCulture);
        return complete;
#else
        return Array.Empty<RewardItemDefinition>();
#endif
    }

    internal void ValidateChestRewardTarget(string id)
    {
#if GAME_INTEROP
        if (!SafeMode || !PlayerReady || _manager is null || _player is null
            || _manager.IsMultiplayer || _manager.StartedAsOnlineMultiplayerRun
            || _player.IsDead || _manager.InGameOverState)
            throw new InvalidOperationException("请进入单人关卡后开启指定箱子奖励。");
        _ = GetRewardItemCatalog();
        if (!TryGetRewardType(id, out var type))
            throw new InvalidOperationException("所选武器或被动装备当前不可用，请刷新物品列表。");
        var current = GetRewardEquipment(type);
        if (current is not null && current.IsMaxLevel())
            throw new InvalidOperationException("所选装备已达到最高等级，无法继续升级，请更换目标。");
#else
        throw new InvalidOperationException("需要重新编译带游戏接口的插件，当前不能开启指定箱子奖励。");
#endif
    }

#if GAME_INTEROP
    internal Equipment? GetRewardEquipment(WeaponType type)
    {
        if (_player is null) return null;
        // Native searchHidden selects exactly one collection: false means
        // active, true means hidden. Check both explicitly; removed/evolved-
        // away equipment is not a currently held reward.
        var weapon = _player.WeaponsManager?.GetWeaponByType(type, false)
            ?? _player.WeaponsManager?.GetWeaponByType(type, true);
        if (weapon is not null) return weapon;
        return _player.AccessoriesManager?.GetAccessoryByType(type, false)
            ?? _player.AccessoriesManager?.GetAccessoryByType(type, true);
    }

    internal bool TryGetRewardType(string id, out WeaponType type)
    {
        type = WeaponType.VOID;
        if (!IsNamedRewardId(id) || NonEquipmentRewardIds.Contains(id)
            || !_rewardCatalogById.ContainsKey(id) || !Enum.TryParse(id, false, out WeaponType parsed)
            || !string.Equals(Enum.GetName(typeof(WeaponType), parsed), id, StringComparison.Ordinal)) return false;
        type = parsed;
        return true;
    }

    internal string RewardDisplayName(WeaponType type) => _rewardCatalogById.TryGetValue(type.ToString(), out var item)
        ? item.Name : type.ToString();

    private static bool IsNamedRewardId(string? id) => !string.IsNullOrWhiteSpace(id) && id.Length <= 128
        && id.All(character => character is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9' or '_');

    private static string GetRewardName(WeaponData data, WeaponType type)
    {
        string? name = null;
        try
        {
            string term = data.GetLocalizedNameTerm(type);
            // WeaponData.GetTranslation prepends "lang/" itself; the name
            // term already contains its full category. Resolve it directly.
            string translated = I2.Loc.LocalizationManager.GetTranslation(term);
            if (!IsUntranslatedRewardName(translated, term)) name = translated;
        }
        catch (Exception) { }
        if (string.IsNullOrWhiteSpace(name) && !IsUntranslatedRewardName(data.name, "")) name = data.name;
        // Base equipment must remain usable if localization is momentarily
        // unavailable; unknown DLC placeholders are not useful UI entries.
        if (string.IsNullOrWhiteSpace(name)) return data.contentGroup is ContentGroupType.BASE or ContentGroupType.EXTRA
            ? type.ToString() : "";
        return NormalizeRewardName(name);
    }

    private static IReadOnlyList<EnglishRewardLanguage> GetEnglishRewardLanguages()
    {
        var result = new List<EnglishRewardLanguage>();
        var sources = I2.Loc.LocalizationManager.Sources;
        if (sources is null) return result;
        for (int sourceIndex = 0; sourceIndex < sources.Count; sourceIndex++)
        {
            var source = sources[sourceIndex];
            if (source?.mLanguages is not { } languages) continue;
            for (int index = 0; index < languages.Count; index++)
            {
                var language = languages[index];
                if (language is not null && (IsEnglishLanguageKey(language.Code) || IsEnglishLanguageKey(language.Name)))
                    result.Add(new EnglishRewardLanguage(source, index));
            }
        }
        return result;
    }

    private static bool IsEnglishLanguageKey(string? value) => !string.IsNullOrWhiteSpace(value)
        && (value.Equals("en", StringComparison.OrdinalIgnoreCase)
            || value.StartsWith("en-", StringComparison.OrdinalIgnoreCase)
            || value.StartsWith("en_", StringComparison.OrdinalIgnoreCase)
            || value.Equals("English", StringComparison.OrdinalIgnoreCase)
            || value.StartsWith("English (", StringComparison.OrdinalIgnoreCase));

    private sealed class EnglishRewardLanguage
    {
        internal I2.Loc.LanguageSourceData Source { get; }
        internal int Index { get; }
        internal EnglishRewardLanguage(I2.Loc.LanguageSourceData source, int index) { Source = source; Index = index; }
    }

    private static string? GetRewardEnglishName(WeaponData data, WeaponType type, IReadOnlyList<EnglishRewardLanguage> languages)
    {
        string term;
        try { term = data.GetLocalizedNameTerm(type); }
        catch (Exception) { return null; }
        foreach (var language in languages)
        {
            try
            {
                // The game registers culture codes such as "en" as language names;
                // a literal "English" override is not a reliable lookup key. Read
                // the actual source's English slot instead. Unlike the global
                // lookup, this cannot fall back to a different language. It never
                // changes CurrentLanguage, loads/unloads language data, or saves.
                var termData = language.Source.GetTermData(term);
                if (termData?.Languages is not { } values || language.Index < 0 || language.Index >= values.Length) continue;
                string translated = termData.GetTranslation(language.Index);
                if (IsUntranslatedRewardName(translated, term)) continue;
                string name = NormalizeRewardName(translated);
                if (!string.IsNullOrWhiteSpace(name)) return name;
            }
            catch (Exception) { }
        }
        return null;
    }

    private static string NormalizeRewardName(string name)
    {
        if (name.Length > 4096) name = name[..4096];
        name = Regex.Replace(name, "<[^>]*>", "", RegexOptions.CultureInvariant, TimeSpan.FromMilliseconds(100));
        name = Regex.Replace(name, @"\s+", " ", RegexOptions.CultureInvariant, TimeSpan.FromMilliseconds(100)).Trim();
        name = new string(name.Where(character => !char.IsControl(character)).ToArray());
        if (name.Length > 96) name = name[..96];
        if (name.Length > 0 && char.IsHighSurrogate(name[^1])) name = name[..^1];
        return name;
    }

    private static bool IsUntranslatedRewardName(string? value, string term) => string.IsNullOrWhiteSpace(value)
        || string.Equals(value, term, StringComparison.Ordinal)
        || value.StartsWith("lang/", StringComparison.OrdinalIgnoreCase)
        || value.StartsWith("weaponLang/", StringComparison.OrdinalIgnoreCase);
#endif
}
