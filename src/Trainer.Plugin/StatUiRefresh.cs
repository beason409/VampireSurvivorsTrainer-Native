using Trainer.Shared;

namespace Trainer.Plugin;

internal sealed partial class GameAdapter
{
    private string? _lastStatButtonSettings;

    // Native upgrade pages cache their consumable buttons. Revalidate after an
    // override changes, including a watchdog disconnect, so a restored zero
    // budget cannot leave a previously granted button available on screen.
    private void RefreshStatButtons(bool force = false)
    {
#if GAME_INTEROP
        if (_manager == null || !_manager.IsLevelUpPageOpen || !SafeMode)
        {
            _lastStatButtonSettings = null;
            return;
        }
        try
        {
            var page = UnityEngine.Object.FindObjectOfType<VampireSurvivors.UI.LevelUpPage>();
            var stats = _player?.PlayerStats;
            if (page == null || stats == null) { _lastStatButtonSettings = null; return; }
            // Heartbeat loss can disable settings on the pipe thread between TickStats
            // and this call. Include the applied native budgets so the following tick's
            // restoration still refreshes the page, even if settings already read OFF.
            string remaining = F((float)stats._ReRolls_k__BackingField) + "|"
                + F((float)stats._Skips_k__BackingField) + "|" + F((float)stats._Banish_k__BackingField);
            string settings = page.Pointer.ToString("X") + "|" + stats.Pointer.ToString("X") + "|" + remaining + "|"
                + string.Join("|", new[] { StatIds.Rerolls, StatIds.Skips, StatIds.Banishes }
                    .Select(id => id + ":" + _runtime.StatEnabled(id) + ":" + _runtime.StatValue(id)));
            if (!force && settings == _lastStatButtonSettings) return;
            // Native OnShowStart computes cached Has* flags first (RVA 064CFE44),
            // then UpdateButtonsUI applies those flags via GameObject.SetActive
            // (RVA 064D0243). Reversing this order leaves the old visibility cached.
            page.ValidateButtonStates();
            page.UpdateButtonsUI();
            _lastStatButtonSettings = settings;
            Diagnostics["stats.buttonsRefreshUtc"] = DateTime.UtcNow.ToString("O");
            Diagnostics["stats.buttonsRemaining"] = remaining;
            Diagnostics.Remove("stats.buttonsRefreshError");
        }
        catch (Exception e) { Diagnostics["stats.buttonsRefreshError"] = e.Message; }
#endif
    }
}
