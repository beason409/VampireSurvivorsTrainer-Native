#include "../common/win_util.hpp"
#include "../common/stat_catalog.hpp"
#include "client_settings.hpp"
#include "game_discovery.hpp"
#include "maintenance.hpp"
#include "ui_theme.hpp"
#include "../vendor/yyjson/yyjson.h"
#include "embedded_backend.hpp"
#include "embedded_licenses.hpp"
#include "native_version.hpp"
#include <sddl.h>
#include <aclapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <commctrl.h>
#include <compressapi.h>
#include <tlhelp32.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <condition_variable>
#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <memory>
#include <thread>

namespace {
using vst::Handle;
namespace ui = vst::client::ui;
constexpr wchar_t kBackendName[] = L"VampireSurvivorsTrainer.NativeBackend.dll";
constexpr char kGameStarting[] = "The game is still starting. Wait for the game to load.";
constexpr char kPipeUnavailable[] = "The native pipe is unavailable or another client is connected.";
constexpr UINT kStatusMessage = WM_APP + 1, kStoppedMessage = WM_APP + 2, kSettingsMessage = WM_APP + 3, kMaintenanceMessage = WM_APP + 4;
constexpr UINT kRewardIconMessage = WM_APP + 5;
constexpr UINT_PTR kLiveValueTimer = 1, kHotkeyTimer = 2, kSettingsTimer = 3, kRewardSearchTimer = 4, kRewardCatalogTimer = 5;
constexpr UINT_PTR kRewardIconTimer = 6;
constexpr uint64_t kRewardScrollQuietMs = 200, kStatusIntervalMs = 450, kGameScanIntervalMs = 1800;
constexpr int kConnect = 101, kManualConnect = 102, kToggle = 103, kDisable = 104, kLanguageChinese = 105, kAbout = 106, kLanguageEnglish = 107;
constexpr int kFeatureToggle = 120, kFeatureValue = 130;
constexpr int kCommonPage = 201, kStatsPage = 202, kHotkeysPage = 203, kMaintenancePage = 204, kRewardsPage = 205, kStatToggle = 300, kStatValue = 400, kHotkeyCapture = 600;
constexpr int kGoldPage = 206;
constexpr int kGameDirectory = 800, kBrowseGame = 801, kCreateBackup = 802, kListBackups = 803, kRestoreBackup = 804, kBackupList = 805;
constexpr int kGoldValue = 806, kSetGold = 807, kGoldBackup = 808;
constexpr int kRewardSearch = 900, kRewardFilter = 901, kRewardRefresh = 902, kRewardList = 903;
constexpr int kRewardApply = 904, kRewardChest = 905, kRewardUpgrade = 906, kRewardBoth = 907, kRewardStop = 908;
constexpr int kPageTop = 276, kCardHeight = 62, kCardPitch = 69;
constexpr std::array<const char*, 9> kFeatures{"health", "experience", "damage", "movement", "cooldown", "instantKill", "noGoldSpend", vst::kGameSpeedId, vst::kSkipChestAnimationId};
constexpr auto kHotkeyFeatures = [] {
    std::array<size_t, vst::client::kFeatureHotkeyCount> features{};
    for (size_t action = 0; action < features.size(); ++action) {
        size_t feature = 0;
        while (feature < kFeatures.size() && std::string_view(kFeatures[feature]) != vst::client::kFeatureHotkeyIds[action]) ++feature;
        features[action] = feature;
    }
    return features;
}();
static_assert(std::ranges::all_of(kHotkeyFeatures, [](size_t feature) { return feature < kFeatures.size(); }));
constexpr size_t kGameSpeedFeature = 7, kChestAnimationFeature = 8;
constexpr size_t kArmorIndex = static_cast<size_t>(vst::NativeStat::Armor);
constexpr size_t kStatTargetOffset = vst::client::kStatPreferenceOffset, kLiveTargetCount = vst::client::kPreferenceValueCount;
constexpr size_t kNoLiveTarget = static_cast<size_t>(-1);
bool english = false, diagnostics_mode = false, closing = false, stats_page = false, hotkeys_page = false, maintenance_page = false, rewards_page = false, gold_page = false, syncing_edits = false;
HWND window = nullptr, heading, description, status_label, values_label, count_label, number_edit, connect_button, manual_connect_button, toggle_button, disable_button, about_button, guide_label;
HWND chinese_button, english_button, armor_label;
HWND common_page_button, stats_page_button, hotkeys_page_button, maintenance_page_button, rewards_page_button, gold_page_button, stat_pane, hotkeys_pane;
HWND game_directory_label, game_directory_edit, browse_game_button, create_backup_button, list_backups_button, restore_backup_button, backup_list, maintenance_label;
HWND gold_label, gold_balance_label, gold_readiness_label, gold_target_label, gold_value_edit, set_gold_button, gold_backup_button, gold_feedback_label;
HWND reward_search, reward_filter, reward_refresh, reward_list, reward_summary, reward_selection, reward_active, reward_operation, reward_chest_record, reward_upgrade_record;
HWND reward_apply, reward_chest, reward_upgrade, reward_both, reward_stop;
std::vector<vst::client::RewardItem> reward_catalog;
std::vector<size_t> reward_filtered;
std::string reward_selected_id, reward_catalog_error, reward_operation_error, reward_error_scope;
uint64_t reward_catalog_generation = 0, reward_pending_sequence = 0;
size_t reward_catalog_total = 0, reward_catalog_next = 0;
bool reward_catalog_loading = false, reward_catalog_requested = false, reward_filtering = false, reward_language_initialized = false, reward_language_english = false;
bool reward_catalog_request_pending = false;
bool reward_icons_supported = false;
HIMAGELIST reward_images = nullptr;
std::unordered_map<std::string, int> reward_image_indices;
std::string reward_icon_pending_id;
vst::client::RewardIcon reward_icon_waiting;
uint64_t reward_last_scroll = 0;
std::array<HWND, kFeatures.size()> feature_labels{}, feature_edits{}, feature_buttons{}, feature_counts{};
std::array<HWND, vst::kNativeStatCount> stat_labels{}, stat_edits{}, stat_buttons{}, stat_values{}, stat_counts{};
std::array<HWND, vst::client::kHotkeyCount> hotkey_labels{}, hotkey_buttons{}, hotkey_notes{};
int stat_scroll = 0, hotkey_scroll = 0;
vst::client::ClientSettings client_settings;
vst::client::HotkeyManager hotkey_manager;
std::wstring settings_path;
std::string settings_notice;
std::wstring capture_notice;
bool settings_writable = true, settings_dirty = false;
size_t capturing_hotkey = vst::client::kHotkeyCount;
uint64_t hotkey_pending_sequence = 0;
bool hotkey_timer_armed = false, status_refresh_deferred = false;
uint64_t connection_sequence = 0;
bool maintenance_busy = false;
std::vector<vst::maintenance::BackupInfo> backup_entries;
std::string maintenance_notice;
HFONT font = nullptr, title_font = nullptr, caption_font = nullptr, detail_font = nullptr;
bool high_contrast = false;
COLORREF connection_indicator = ui::muted;
UINT dpi = 96;

struct Status {
    struct Feature {
        bool supported = false, enabled = false;
        double value = 1;
        uint64_t calls = 0, actual_calls = 0, overrides = 0;
    };
    struct Stat {
        bool supported = false, enabled = false, original_known = false, current_known = false, bound_known = false;
        double value = 0, original = 0, current = 0, bound = 0;
        uint64_t reads = 0, overrides = 0;
        std::string reason, source;
    };
    bool connected = false, player_ready = false, supported = false, enabled = false, command_ok = true;
    bool backup_authorized = false, wallet_ready = false, gold_known = false, gold_write_supported = false, gold_write_ready = false, paused_known = false, paused = true;
    bool original_known = false, current_known = false;
    DWORD pid = 0;
    double original = 0, current = 0, target = 20;
    uint64_t reads = 0;
    uint64_t received_at = 0, client_sequence = 0;
    double gold = 0;
    std::array<Feature, kFeatures.size()> features{};
    std::array<Stat, vst::kNativeStatCount> stats{};
    vst::client::RewardState reward;
    std::shared_ptr<vst::client::RewardCatalogPage> catalog_page;
    uint64_t catalog_generation = 0;
    size_t catalog_offset = 0;
    bool reward_result = false;
    uint64_t gold_reply_sequence = 0;
    bool gold_cancelled = false;
    bool connecting_automatically = false;
    vst::client::RewardIcon reward_icon;
    std::string error_scope;
    std::string scene, error, note, armor_reason;
    std::wstring game_directory;
    vst::client::GameScan game_scan;
};
Status displayed;
struct GoldSubmission {
    enum class Result { None, Saved, Failed, Cancelled, Unconfirmed };
    uint64_t pending = 0;
    double value = 0;
    Result result = Result::None;
    std::string error;

    void Accept(const Status& reply) {
        if (!pending || reply.gold_reply_sequence != pending) return;
        pending = 0;
        if (reply.gold_cancelled) result = Result::Cancelled;
        else if (!reply.connected) result = Result::Unconfirmed;
        else if (!reply.command_ok) { result = Result::Failed; error = reply.error; }
        else result = reply.gold_known && reply.gold == value ? Result::Saved : Result::Unconfirmed;
    }
};
GoldSubmission gold_submission;
bool CommonPage() { return !stats_page && !hotkeys_page && !maintenance_page && !rewards_page && !gold_page; }
void Refresh();
void QueueInitialRewardCatalog();
void FilterRewards();
void ResetRewardCatalog();
void ApplyRewardCatalogResult(const Status& status);

std::wstring Text(const wchar_t* chinese, const wchar_t* en) { return english ? en : chinese; }
size_t FeatureLiveTarget(size_t feature) {
    if (feature == kGameSpeedFeature) return vst::client::kGameSpeedPreference;
    return feature >= 1 && feature <= 3 ? feature - 1 : kNoLiveTarget;
}
size_t LiveFeatureIndex(size_t target) {
    return target == vst::client::kGameSpeedPreference ? kGameSpeedFeature : target + 1;
}
size_t LiveTargetForId(int id) {
    if (id == 110) return kStatTargetOffset + kArmorIndex;
    if (id >= kFeatureValue && id < kFeatureValue + static_cast<int>(kFeatures.size())) return FeatureLiveTarget(static_cast<size_t>(id - kFeatureValue));
    if (id >= kStatValue && id < kStatValue + static_cast<int>(vst::kNativeStatCount)) return kStatTargetOffset + static_cast<size_t>(id - kStatValue);
    return kNoLiveTarget;
}
bool ParseLiveValue(size_t target, std::wstring_view input, double& value) {
    while (!input.empty() && std::iswspace(input.front())) input.remove_prefix(1);
    while (!input.empty() && std::iswspace(input.back())) input.remove_suffix(1);
    // A trailing decimal/exponent is still being typed. Do not send it.
    if (input.empty() || input.size() >= 128 || input.back() < L'0' || input.back() > L'9') return false;
    std::wstring text(input); wchar_t* end = nullptr;
    value = std::wcstod(text.c_str(), &end);
    if (!end || end == text.c_str() || *end || !std::isfinite(value)) return false;
    return vst::client::ValidPreferenceValue(target, value);
}
class LiveValueSchedule {
    struct Edit { bool dirty = false; uint64_t due = 0; };
    std::array<Edit, kLiveTargetCount> edits_{};
public:
    void Changed(size_t target, uint64_t now) { if (target < edits_.size()) edits_[target] = {true, now + 300}; }
    void Cancel(size_t target) { if (target < edits_.size()) edits_[target] = {}; }
    void CancelAll() { edits_ = {}; }
    bool Dirty(size_t target) const { return target < edits_.size() && edits_[target].dirty; }
    bool Due(size_t target, uint64_t now, bool force = false) const { return Dirty(target) && (force || now >= edits_[target].due); }
} live_values;
bool LiveTargetEnabled(const Status& status, size_t target) {
    if (!status.connected || !status.player_ready) return false;
    if (target >= kStatTargetOffset && target < kLiveTargetCount) {
        const auto& stat = status.stats[target - kStatTargetOffset];
        return stat.supported && stat.enabled;
    }
    if (target >= kStatTargetOffset) return false;
    const auto& feature = status.features[LiveFeatureIndex(target)];
    return feature.supported && feature.enabled;
}
std::wstring Message(std::string_view value) {
    if (english) return vst::Wide(std::string(value));
    if (value == "Game speed must be a finite number from 1 to 10.") return L"游戏加速倍率应在 1 到 10 之间。";
    if (value == "Game speed could not be verified or restored safely.") return L"未能安全核对或恢复游戏速度。";
    if (value == "The old game loader is active. Exit the game and uninstall the old companion plugin before this native test."
        || value == "The managed backend is active. Exit the game and uninstall the old companion plugin before this native test.")
        return L"检测到旧版加载器。请退出游戏，在正式版“维护与备份 → 卸载插件”完成卸载，再从 Steam 启动游戏。";
    if (value == "A different native build is already loaded. Exit the game before updating the pilot.")
        return L"游戏仍加载着上一版原生后端。请正常退出游戏、关闭旧修改器，再重启游戏并连接新版。";
    if (value == vst::client::kNoRunningGame) return L"暂未检测到游戏。请从 Steam 启动游戏，或手动选择已运行游戏的目录。";
    if (value == vst::client::kNoGameInFolder) return L"所选目录中的游戏尚未运行。请从 Steam 启动对应的游戏后再试。";
    if (value == vst::client::kMultipleGames) return L"检测到多个游戏进程。请手动选择游戏目录。";
    if (value == vst::client::kMultipleGamesInFolder) return L"所选目录有多个游戏实例。请只保留一个再试。";
    if (value == vst::client::kMissingGameExecutable) return L"请选择包含 VampireSurvivors.exe 的文件夹。";
    if (value == "The selected game process changed.") return L"游戏进程已更换，请重新连接。";
    if (value == kGameStarting) return L"游戏仍在启动，请稍等。";
    if (value == kPipeUnavailable) return L"原生连接尚未就绪，或被其他修改器窗口占用。请确认只开一个修改器窗口。";
    if (value.starts_with("Unsupported game fingerprint:") || value == "The Steam installation/build does not match the supported game.")
        return L"当前 Steam 游戏版本与修改器支持的版本不匹配。";
    if (value == "The game connection is closed.") return L"与游戏的连接已断开，请重新连接。";
    if (value == "Connection is no longer active.") return L"控制连接已失效，功能已关闭，请重新连接。";
    if (value == "Enter a supported single-player run before enabling this effect.") return L"请先进入普通单人关卡，再开启该功能。";
    if (value == "The native game response timed out.") return L"等待游戏响应超时，请检查游戏是否仍在运行后重新连接。";
    if (value == "The current character uses a different native armor method.") return L"当前角色的护甲入口尚未适配。";
    if (value == "Settings could not be read; the original file was preserved.") return L"设置无法读取，原文件已保留。";
    if (value == "Settings were not saved; changes apply only in this window.") return L"设置未保存，本次窗口中的更改仍有效。";
    if (value == "Previous language, values and hotkeys were imported. Existing files were kept.") return L"已沿用旧版的语言、数值和快捷键，原文件保持不变。";
    if (value == "Choose the installed game folder first.") return L"请先选择已安装的游戏目录。";
    if (value == "Exit the game and Steam before backing up or restoring saves.") return L"备份或恢复前，请先退出游戏和 Steam。";
    if (value == "No nonempty SaveData for the current Steam account was found.") return L"未找到当前 Steam 账户的有效存档。";
    if (value == "A save changed during capture; wait for Steam sync and retry.") return L"存档仍在变化，请等待 Steam 同步结束后重试。";
    if (value == "Backup created. Exit the game before restoring it.") return L"备份已创建。恢复前请先退出游戏。";
    if (value == "Backup restored. The previous progress has a safety snapshot.") return L"备份已恢复，恢复前的进度已保留为保护快照。";
    if (value == "Backup list refreshed.") return L"备份列表已刷新。";
    if (value == "A verified pre-start backup and the normal single-player main menu are required."
        || value == "The normal wallet or startup backup is no longer ready.") return L"请确认已完成启动前备份，并回到普通单人主菜单。";
    if (value == "Permanent gold requires the ordinary single-player main menu.") return L"请在普通单人主菜单修改局外金币。";
    if (value == "The native gold update failed; the previous balance was restored and saved.") return L"保存失败，原余额已恢复并保存。";
    if (value == "The native gold update failed and its previous balance could not be confirmed restored.") return L"保存失败，未能确认原余额已恢复。请在游戏中核对余额。";
    if (value == "The native wallet balance is unavailable.") return L"暂时无法读取普通模式的余额，请稍后再试。";
    if (value == "The native client is busy. Try again shortly.") return L"修改器正在处理其他操作，请稍后再试。";
    if (value == "The item request queue is busy. Try refreshing.") return L"物品请求队列繁忙，请稍后刷新。";
    if (value == "The game did not return an item catalog page.") return L"游戏尚未返回物品列表，请稍后刷新。";
    if (value == "The game returned an invalid item catalog page." || value == "The game returned an invalid reward item.") return L"物品列表格式无效，请刷新列表。";
    if (value == "The item catalog changed while loading. Refresh the list.") return L"物品列表在读取时发生变化，请重新刷新。";
    if (value == "The item limit was reached; only the first 4096 entries are loaded.") return L"物品列表已到上限，仅加载前4096项。";
    if (value == "The reward request queue is busy. Try again.") return L"指定奖励请求繁忙，请稍后重试。";
    if (value == "The game did not confirm the reward request.") return L"游戏尚未确认指定奖励请求。";
    if (value == "Offline layout preview. No game connection.") return L"离线界面预览，未连接游戏。";
    if (value == "The selected item does not support upgrade targeting.") return L"所选物品尚不支持升级指定。";
    if (value == "The selected equipment is already at its maximum level.") return L"所选装备已满级，请更换目标。";
    if (value == "Select a named, loaded equipment item." || value == "Select a valid weapon or passive item first.") return L"请从当前列表选择武器或被动装备。";
    if (value == "The selected equipment is unavailable; refresh the catalog.") return L"所选物品当前不可用，请刷新列表。";
    if (value == "The equipment catalog is loading. Try again shortly.") return L"物品列表仍在读取，请稍后重试。";
    if (value == "The current run's reward data is not ready." || value == "The menu's reward data is still loading."
        || value == "The game's equipment catalog is still loading." || value == "No loaded weapons or passive items are available yet.") return L"游戏物品数据尚未就绪，请稍后刷新。";
    if (value == "Wait for the single-player menu or run before reading equipment.") return L"请先进入普通单人主菜单或关卡，再刷新物品列表。";
    if (value == "Enter a ready single-player run before enabling specified rewards."
        || value == "Enter a supported single-player run before enabling specified rewards.") return L"请先进入普通单人关卡，再开启指定奖励。";
    if (value == "Wait for fresh single-player game data before reading or changing rewards."
        || value == "The single-player reward context is no longer ready.") return L"正在等待关卡状态更新，请稍后重试。";
    if (value == "Evolved passive items cannot be forced into an upgrade choice." || value == "Evolved passive upgrades are not supported."
        || value == "Evolved passive upgrades are not supported; chest rewards remain available.") return L"进化被动暂不支持升级指定，可使用“仅宝箱”。";
    if (value == "The requested native reward entry is unavailable." || value == "The native reward adapter is unavailable."
        || value == "Native reward data is unavailable.") return L"指定奖励接口暂不可用，请等待游戏就绪。";
    if (value == "The native chest reward route is unavailable.") return L"宝箱指定接口暂不可用。";
    if (value == "The native upgrade choice route is unavailable.") return L"升级指定接口暂不可用。";
    if (value == "The native equipment catalog is unavailable.") return L"物品列表暂不可用，请稍后刷新。";
    if (value == "The current run's upgrade data is unavailable." || value == "The selected item has no native upgrade levels."
        || value == "The selected item has no valid native upgrade header.") return L"所选装备的升级数据尚未就绪，请刷新列表。";
    if (value == "The reward context or target changed while validating the request." || value == "The local reward context changed during validation."
        || value == "The single-player menu changed while reading equipment." || value == "The equipment data owner changed while loading."
        || value == "The equipment source changed while loading." || value == "The game's language changed while loading equipment.") return L"关卡、目标或物品数据已变化，请刷新后重新确认。";
    if (value == "A reward disable cancelled older target changes.") return L"指定已关闭，先前的目标请求已取消。";
    if (value == "The native reward adapter failed." || value == "Native reward validation failed." || value == "The reward request failed.") return L"游戏未能确认指定奖励，请稍后重试。";
    if (value.starts_with("Armor: ")) return L"护甲：" + Message(value.substr(7));
    return vst::Wide(std::string(value));
}
int Scale(int value) { return MulDiv(value, static_cast<int>(dpi), 96); }
HFONT ControlFont(HWND control) {
    if (control == heading || control == gold_balance_label) return title_font;
    if (control == armor_label || control == gold_label || control == gold_target_label || std::find(feature_labels.begin(), feature_labels.end(), control) != feature_labels.end()) return caption_font;
    if (control == description || control == guide_label || control == reward_summary || control == reward_operation || control == count_label
        || std::find(feature_counts.begin(), feature_counts.end(), control) != feature_counts.end()
        || std::find(stat_counts.begin(), stat_counts.end(), control) != stat_counts.end()
        || std::find(hotkey_notes.begin(), hotkey_notes.end(), control) != hotkey_notes.end()) return detail_font;
    return font;
}
void ApplyFonts() {
    EnumChildWindows(window, [](HWND control, LPARAM) -> BOOL {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ControlFont(control)), FALSE);
        return TRUE;
    }, 0);
    for (HWND edit : {number_edit, game_directory_edit, gold_value_edit, reward_search})
        if (edit) SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Scale(7), Scale(7)));
    for (HWND edit : feature_edits) if (edit) SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN, Scale(7));
    for (HWND edit : stat_edits) if (edit) SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN, Scale(7));
}
void DeleteFonts() {
    for (HFONT face : {font, title_font, caption_font, detail_font})
        if (face) DeleteObject(face);
}
void UpdateFonts() {
    const std::array old{font, title_font, caption_font, detail_font};
    auto make = [](int size, int weight) {
        return CreateFontW(-Scale(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    font = make(16, FW_NORMAL);
    title_font = make(30, FW_SEMIBOLD);
    caption_font = make(16, FW_SEMIBOLD);
    detail_font = make(14, FW_NORMAL);
    ApplyFonts();
    for (HFONT face : old) if (face) DeleteObject(face);
}
void SetText(HWND control, const std::wstring& value) {
    const int length = GetWindowTextLengthW(control);
    if (static_cast<size_t>(length) == value.size()) {
        wchar_t buffer[128];
        if (length < static_cast<int>(std::size(buffer))) {
            const int read = GetWindowTextW(control, buffer, length + 1);
            if (std::wstring_view(buffer, read) == value) return;
        } else {
            std::wstring previous(static_cast<size_t>(length) + 1, L'\0');
            previous.resize(GetWindowTextW(control, previous.data(), length + 1));
            if (previous == value) return;
        }
    }
    SetWindowTextW(control, value.c_str());
}
void SetEnabled(HWND control, bool enabled) {
    if (control && (IsWindowEnabled(control) != FALSE) != enabled) EnableWindow(control, enabled);
}
struct InterfaceBounds {
    RECT client{};
    int left = Scale(24), content = 0, column = 0, right_column = 0;
    explicit InterfaceBounds(HWND hwnd) {
        GetClientRect(hwnd, &client);
        content = std::max(Scale(200), static_cast<int>(client.right) - 2 * left);
        column = (content - Scale(16)) / 2;
        right_column = left + column + Scale(16);
    }
    RECT StatusCard() const { return {left, Scale(106), left + content, Scale(174)}; }
    RECT FeatureCard(int row, bool right) const {
        int x = right ? right_column : left;
        int y = kPageTop + row * kCardPitch;
        return {x, Scale(y), x + column, Scale(y + kCardHeight)};
    }
    RECT ArmorReadings() const { return {left, Scale(622), left + content, Scale(668)}; }
    RECT RewardDetails() const { return {left, client.bottom - Scale(212), left + content, client.bottom - Scale(42)}; }
    RECT GoldBalance() const { return {left, Scale(kPageTop), left + content, Scale(402)}; }
    RECT GoldEditor() const { return {left, Scale(416), left + content, Scale(574)}; }
};

void PaintInterface(HWND hwnd, HDC dc) {
    const InterfaceBounds bounds(hwnd);
    if (high_contrast) { FillRect(dc, &bounds.client, GetSysColorBrush(COLOR_WINDOW)); return; }
    ui::Fill(dc, bounds.client, ui::canvas);
    ui::Fill(dc, {0, 0, bounds.client.right, Scale(94)}, ui::banner);
    ui::Fill(dc, {0, 0, bounds.client.right, Scale(3)}, ui::gold);
    ui::Panel(dc, bounds.StatusCard(), Scale(14));
    ui::Panel(dc, {bounds.left + Scale(12), Scale(122), bounds.left + Scale(21), Scale(131)}, Scale(9),
        connection_indicator, connection_indicator);
    if (rewards_page) ui::Panel(dc, bounds.RewardDetails(), Scale(14));
    else if (gold_page) {
        ui::Panel(dc, bounds.GoldBalance(), Scale(14));
        ui::Panel(dc, bounds.GoldEditor(), Scale(14));
    } else if (CommonPage()) {
        for (int row = 0; row < 5; ++row) {
            ui::Panel(dc, bounds.FeatureCard(row, false), Scale(14));
            ui::Panel(dc, bounds.FeatureCard(row, true), Scale(14));
        }
        ui::Panel(dc, bounds.ArmorReadings(), Scale(14));
    }
}
void PaintScrollablePane(HWND hwnd, HDC dc) {
    RECT rect{}; GetClientRect(hwnd, &rect);
    if (high_contrast) { FillRect(dc, &rect, GetSysColorBrush(COLOR_WINDOW)); return; }
    ui::Fill(dc, rect, ui::surface);
    const bool keys = hwnd == hotkeys_pane;
    const int pitch = Scale(keys ? 60 : 78), offset = keys ? hotkey_scroll : stat_scroll;
    for (int y = pitch - offset % pitch; y < rect.bottom; y += pitch)
        ui::Fill(dc, {Scale(12), y - 1, rect.right - Scale(12), y}, ui::edge);
}
LRESULT ThemedControlColor(UINT message, WPARAM wparam, LPARAM lparam, bool pane = false) {
    if (high_contrast || (message != WM_CTLCOLORSTATIC && message != WM_CTLCOLOREDIT
        && message != WM_CTLCOLORBTN && message != WM_CTLCOLORLISTBOX)) return 0;
    const auto dc = reinterpret_cast<HDC>(wparam);
    const auto control = reinterpret_cast<HWND>(lparam);
    COLORREF background = pane ? ui::surface : ui::canvas;
    COLORREF foreground = ui::ink;
    const int id = GetDlgCtrlID(control);
    if (control == heading || control == description || control == chinese_button || control == english_button) {
        background = ui::banner;
        foreground = control == heading ? ui::surface : ui::banner_muted;
    } else if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        background = ui::input;
    } else if (control == status_label || control == reward_selection || control == reward_active || control == reward_operation
        || control == reward_chest_record || control == reward_upgrade_record || (id >= kRewardApply && id <= kRewardStop)
        || control == gold_label || control == gold_balance_label || control == gold_readiness_label || control == gold_target_label || control == gold_feedback_label
        || (!pane && CommonPage() && (message == WM_CTLCOLORSTATIC || id == kToggle
            || (id >= kFeatureToggle && id < kFeatureToggle + static_cast<int>(kFeatures.size()))))) {
        background = ui::surface;
    }
    if (control == guide_label || control == reward_summary) { background = ui::canvas; foreground = ui::muted; }
    SetTextColor(dc, foreground);
    SetBkColor(dc, background);
    SetBkMode(dc, message == WM_CTLCOLOREDIT ? OPAQUE : TRANSPARENT);
    return reinterpret_cast<LRESULT>(ui::Brush(dc, background));
}
LRESULT DrawThemedControl(NMHDR* header) {
    if (high_contrast || !header || header->code != NM_CUSTOMDRAW) return CDRF_DODEFAULT;
    const auto& draw = *reinterpret_cast<NMCUSTOMDRAW*>(header);
    if (header->hwndFrom == reward_list) {
        if (draw.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (draw.dwDrawStage == CDDS_ITEMPREPAINT && !(draw.uItemState & CDIS_SELECTED)) {
            auto& list = *reinterpret_cast<NMLVCUSTOMDRAW*>(header);
            list.clrText = ui::ink;
            list.clrTextBk = draw.dwItemSpec % 2 ? ui::row_alt : ui::surface;
        }
        return CDRF_DODEFAULT;
    }
    if (draw.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    wchar_t type[16]{};
    HWND control = header->hwndFrom;
    if (!GetClassNameW(control, type, static_cast<int>(std::size(type))) || _wcsicmp(type, L"Button") != 0) return CDRF_DODEFAULT;
    const int id = GetDlgCtrlID(control);
    auto role = ui::ButtonRole::Normal;
    if (id == kLanguageChinese || id == kLanguageEnglish) {
        role = SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED ? ui::ButtonRole::LanguageSelected : ui::ButtonRole::Language;
    } else if (id >= kCommonPage && id <= kGoldPage && !IsWindowEnabled(control)) {
        role = ui::ButtonRole::Tab;
    } else if (id == kConnect || id == kRewardApply || id == kSetGold) {
        role = ui::ButtonRole::Primary;
    } else if (id == kDisable || id == kRewardStop || id == kRestoreBackup) {
        role = ui::ButtonRole::Danger;
    } else if ((id == kToggle && displayed.enabled)
        || (id >= kFeatureToggle && id < kFeatureToggle + static_cast<int>(kFeatures.size()) && displayed.features[id - kFeatureToggle].enabled)
        || (id >= kStatToggle && id < kStatToggle + static_cast<int>(vst::kNativeStatCount) && displayed.stats[id - kStatToggle].enabled)
        || (id == kRewardChest && displayed.reward.enabled && !displayed.reward.upgrade_enabled)
        || (id == kRewardUpgrade && displayed.reward.upgrade_enabled && !displayed.reward.enabled)
        || (id == kRewardBoth && displayed.reward.enabled && displayed.reward.upgrade_enabled)) {
        role = ui::ButtonRole::Active;
    }
    ui::DrawButton(draw, role, Scale(9), Scale(8), font);
    return CDRF_SKIPDEFAULT;
}
void UpdateTheme() {
    high_contrast = ui::HighContrast();
    if (reward_list) {
        ListView_SetBkColor(reward_list, high_contrast ? GetSysColor(COLOR_WINDOW) : ui::surface);
        ListView_SetTextBkColor(reward_list, high_contrast ? GetSysColor(COLOR_WINDOW) : ui::surface);
        ListView_SetTextColor(reward_list, high_contrast ? GetSysColor(COLOR_WINDOWTEXT) : ui::ink);
    }
}
std::wstring Number(double value) { wchar_t text[64]{}; std::swprintf(text, std::size(text), L"%.3f", value); return text; }
void WriteOutput(const std::string& value) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) output = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (output && output != INVALID_HANDLE_VALUE) { DWORD written = 0; WriteFile(output, value.data(), static_cast<DWORD>(value.size()), &written, nullptr); }
}
bool VerifyBackendPayload(std::string& error) {
    std::string digest;
    if (!vst::Sha256(vst::embedded::backend_payload, sizeof(vst::embedded::backend_payload), digest, error)) return false;
    if (digest != vst::embedded::payload_sha256) { error = "The embedded native backend is damaged."; return false; }
    return true;
}
bool DecodeBackend(std::vector<unsigned char>& bytes, std::string& error) {
    static_assert(vst::embedded::backend_size <= 16 * 1024 * 1024);
    wchar_t system[MAX_PATH];
    const UINT count = GetSystemDirectoryW(system, std::size(system));
    if (!count || count >= std::size(system)) { error = vst::WinError("Locate Windows decompression"); return false; }
    using Module = std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)>;
    Module cabinet(LoadLibraryExW(vst::JoinPath(system, L"cabinet.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32), FreeLibrary);
    if (!cabinet) { error = vst::WinError("Open Windows decompression"); return false; }
    auto create = reinterpret_cast<decltype(&CreateDecompressor)>(GetProcAddress(cabinet.get(), "CreateDecompressor"));
    auto decompress = reinterpret_cast<decltype(&Decompress)>(GetProcAddress(cabinet.get(), "Decompress"));
    auto close = reinterpret_cast<decltype(&CloseDecompressor)>(GetProcAddress(cabinet.get(), "CloseDecompressor"));
    DECOMPRESSOR_HANDLE handle = nullptr;
    if (!create || !decompress || !close || !create(COMPRESS_ALGORITHM_LZMS, nullptr, &handle)) {
        error = vst::WinError("Start Windows decompression"); return false;
    }
    std::unique_ptr<std::remove_pointer_t<DECOMPRESSOR_HANDLE>, decltype(close)> decoder(handle, close);
    bytes.resize(vst::embedded::backend_size);
    SIZE_T decoded = 0;
    if (!decompress(decoder.get(), const_cast<unsigned char*>(vst::embedded::backend_payload), sizeof(vst::embedded::backend_payload), bytes.data(), bytes.size(), &decoded)
        || decoded != bytes.size()) { error = "The embedded native backend could not be decompressed."; return false; }
    std::string digest;
    if (!vst::Sha256(bytes.data(), bytes.size(), digest, error)) return false;
    if (digest != vst::embedded::backend_sha256) { error = "The embedded native backend is damaged."; return false; }
    return true;
}
bool ValidBundle(std::string& error) {
    std::vector<unsigned char> bytes;
    return VerifyBackendPayload(error) && DecodeBackend(bytes, error);
}
bool BackendPath(std::wstring& path, std::string& error) {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local))) { error = "Local application data is unavailable."; return false; }
    std::wstring base = vst::JoinPath(local, L"Beason9527.VSTrainer.NativePilot"); CoTaskMemFree(local);
    path = vst::JoinPath(vst::JoinPath(base, vst::Wide(vst::embedded::backend_sha256)), kBackendName); return true;
}
bool LockVerifiedBackend(const std::wstring& path, Handle& lock, std::string& error) {
    if (!vst::RejectReparsePath(path, false, error)) return false;
    Handle candidate(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!candidate) { error = vst::WinError("Lock verified backend cache"); return false; }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(candidate.get(), &info)) { error = vst::WinError("Inspect locked backend cache"); return false; }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        || info.nFileSizeHigh || info.nFileSizeLow != vst::embedded::backend_size || info.nNumberOfLinks != 1) {
        error = "The cached native backend was changed; it was not loaded."; return false;
    }
    std::vector<unsigned char> bytes(vst::embedded::backend_size);
    DWORD read = 0;
    if (!ReadFile(candidate.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) {
        error = vst::WinError("Read locked backend cache"); return false;
    }
    std::string digest;
    if (!vst::Sha256(bytes.data(), bytes.size(), digest, error)) return false;
    if (digest != vst::embedded::backend_sha256) { error = "The cached native backend was changed; it was not loaded."; return false; }
    lock = std::move(candidate);
    return true;
}
// An old pilot stays resident until game exit. Never load or silently use a
// different build beside it, even when both builds use the same wire version.
bool CheckResidentBackend(DWORD pid, const std::wstring& expected, bool require, std::string& error) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    MODULEENTRY32W module{sizeof(module)}; bool found = false;
    if (!snapshot || !Module32FirstW(snapshot.get(), &module)) { error = vst::WinError("Verify resident backend"); return false; }
    do {
        if (!vst::EqualPath(module.szModule, kBackendName)) continue;
        if (found || !vst::EqualPath(module.szExePath, expected)) { error = "A different native build is already loaded. Exit the game before updating the pilot."; return false; }
        found = true;
    } while (Module32NextW(snapshot.get(), &module));
    if (require && !found) { error = "The native pipe does not belong to the embedded backend build."; return false; }
    if (found) {
        Handle lock;
        if (!LockVerifiedBackend(expected, lock, error)) return false;
    }
    return true;
}
struct Security {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID user = nullptr;
    std::vector<unsigned char> token_user;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    ~Security() { if (descriptor) LocalFree(descriptor); }
    bool Open(std::string& error) {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) { error = vst::WinError("Read cache owner"); return false; }
        Handle token(raw); DWORD length = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
        if (!length || length > 65536) { error = "Invalid cache owner."; return false; }
        token_user.resize(length);
        if (!GetTokenInformation(token.get(), TokenUser, token_user.data(), length, &length)) { error = vst::WinError("Read cache owner"); return false; }
        user = reinterpret_cast<TOKEN_USER*>(token_user.data())->User.Sid;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(user, &sid)) { error = vst::WinError("Format cache owner"); return false; }
        std::wstring sddl = L"O:" + std::wstring(sid) + L"D:P(A;OICI;FA;;;" + sid + L")";
        LocalFree(sid);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) { error = vst::WinError("Create private cache descriptor"); return false; }
        attributes.lpSecurityDescriptor = descriptor; return true;
    }
    bool EnsureDirectory(const std::wstring& path, std::string& error) {
        if (!vst::RejectReparsePath(path, true, error)) return false;
        if (!CreateDirectoryW(path.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS) { error = vst::WinError("Create private backend cache"); return false; }
        if (!vst::RejectReparsePath(path, false, error)) return false;
        DWORD flags = GetFileAttributesW(path.c_str());
        if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY)) { error = "Backend cache path is not a directory."; return false; }
        PSID owner = nullptr; PACL dacl = nullptr; PSECURITY_DESCRIPTOR actual = nullptr;
        DWORD result = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &actual);
        bool valid = result == ERROR_SUCCESS && owner && EqualSid(owner, user) && dacl;
        if (valid) {
            SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
            valid = GetSecurityDescriptorControl(actual, &control, &revision) && (control & SE_DACL_PROTECTED);
            for (DWORD i = 0; valid && i < dacl->AceCount; ++i) {
                void* raw_ace = nullptr;
                if (!GetAce(dacl, i, &raw_ace)) { valid = false; break; }
                auto ace = static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
                if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || !EqualSid(&ace->SidStart, user)) valid = false;
            }
        }
        if (actual) LocalFree(actual);
        if (!valid) { error = "The existing backend cache does not have the expected private owner/access rules."; return false; }
        return true;
    }
};
bool PrivateSettingsFile(const std::wstring& path, const Security& security, std::string& error) {
    if (!vst::RejectReparsePath(path, false, error)) return false;
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) { error = "Settings path is not a regular file."; return false; }
    PSID owner = nullptr; PACL dacl = nullptr; PSECURITY_DESCRIPTOR actual = nullptr;
    DWORD code = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &actual);
    bool valid = code == ERROR_SUCCESS && owner && EqualSid(owner, security.user) && dacl;
    if (valid) {
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
        valid = GetSecurityDescriptorControl(actual, &control, &revision) && (control & SE_DACL_PROTECTED);
        for (DWORD index = 0; valid && index < dacl->AceCount; ++index) {
            void* raw = nullptr;
            if (!GetAce(dacl, index, &raw)) { valid = false; break; }
            auto ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || !EqualSid(&ace->SidStart, security.user)) valid = false;
        }
    }
    if (actual) LocalFree(actual);
    if (!valid) error = "Settings file does not have private owner/access rules.";
    return valid;
}
bool SettingsLocation(std::wstring& path, Security& security, std::string& error) {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local))) { error = "Local application data is unavailable."; return false; }
    std::wstring base = vst::JoinPath(local, L"Beason9527.VSTrainer.NativePilot"); CoTaskMemFree(local);
    std::wstring directory = vst::JoinPath(base, L"settings");
    if (!security.Open(error) || !security.EnsureDirectory(base, error) || !security.EnsureDirectory(directory, error)) return false;
    path = vst::JoinPath(directory, L"settings.json");
    return vst::RejectReparsePath(path, true, error);
}
void LoadClientSettings() {
    Security security; std::string error;
    if (!SettingsLocation(settings_path, security, error)) { settings_writable = false; settings_notice = "Settings could not be read; the original file was preserved."; return; }
    DWORD attributes = GetFileAttributesW(settings_path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        std::string json;
        if (!PrivateSettingsFile(settings_path, security, error)
            || !vst::ReadSmallFile(settings_path, vst::client::kMaximumSettingsBytes, json, error)
            || !vst::client::ParseClientSettings(json, false, client_settings, error)) {
            settings_writable = false; settings_notice = "Settings could not be read; the original file was preserved.";
        }
        english = client_settings.english; return;
    }
    DWORD missing = GetLastError();
    if (missing != ERROR_FILE_NOT_FOUND && missing != ERROR_PATH_NOT_FOUND) { settings_writable = false; settings_notice = "Settings could not be read; the original file was preserved."; return; }
    // Import only the known legacy settings at an explicitly configured data
    // location. No directory scan and no modification of the old document.
    std::array<wchar_t, 32768> configured{};
    DWORD length = GetEnvironmentVariableW(L"VS_TRAINER_DATA", configured.data(), static_cast<DWORD>(configured.size()));
    std::wstring root;
    if (length && length < configured.size() && vst::LocalPath(std::wstring(configured.data(), length), root, error)) {
        auto legacy = vst::JoinPath(root, L"desktop-settings.json");
        std::string json;
        if (GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES
            && vst::ReadSmallFile(legacy, vst::client::kMaximumSettingsBytes, json, error)
            && vst::client::ParseClientSettings(json, true, client_settings, error)) {
            settings_notice = "Previous language, values and hotkeys were imported. Existing files were kept.";
            settings_dirty = true;
        }
    }
    english = client_settings.english;
}
bool WriteClientSettings(const vst::client::ClientSettings& settings) {
    if (!settings_writable || settings_path.empty()) return false;
    std::string error, json = vst::client::EncodeClientSettings(settings);
    Security security; std::wstring expected;
    if (json.empty() || !SettingsLocation(expected, security, error) || !vst::EqualPath(expected, settings_path)) return false;
    DWORD attributes = GetFileAttributesW(expected.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && !PrivateSettingsFile(expected, security, error)) return false;
    if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND) return false;
    std::wstring temporary = expected + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    Handle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
    if (!output) return false;
    DWORD written = 0;
    bool valid = WriteFile(output.get(), json.data(), static_cast<DWORD>(json.size()), &written, nullptr)
        && written == json.size() && FlushFileBuffers(output.get());
    output.reset();
    if (valid) valid = vst::RejectReparsePath(expected, true, error) && MoveFileExW(temporary.c_str(), expected.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!valid) DeleteFileW(temporary.c_str());
    return valid;
}
bool WriteBackendCache(const std::wstring& path, Security& security, std::string& error) {
    std::vector<unsigned char> bytes;
    if (!DecodeBackend(bytes, error)) return false;
    const auto temporary = path + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    Handle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!output) { error = vst::WinError("Create verified backend cache"); return false; }
    DWORD written = 0;
    bool complete = WriteFile(output.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        && written == bytes.size() && FlushFileBuffers(output.get());
    if (!complete) error = vst::WinError("Write verified backend cache");
    output.reset();
    if (complete && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
    const DWORD move_error = GetLastError();
    DeleteFileW(temporary.c_str());
    if (!complete) return false;
    // A concurrent client may have published its own complete cache first.
    if (move_error == ERROR_ALREADY_EXISTS || move_error == ERROR_FILE_EXISTS) return true;
    error = vst::WinError("Publish verified backend cache", move_error);
    return false;
}
bool ExtractBackend(std::wstring& path, Handle& lock, std::string& error) {
    if (!VerifyBackendPayload(error)) return false;
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local))) { error = "Local application data is unavailable."; return false; }
    std::wstring base = vst::JoinPath(local, L"Beason9527.VSTrainer.NativePilot"); CoTaskMemFree(local);
    Security security; if (!security.Open(error) || !security.EnsureDirectory(base, error)) return false;
    std::wstring directory = vst::JoinPath(base, vst::Wide(vst::embedded::backend_sha256));
    if (!security.EnsureDirectory(directory, error)) return false;
    path = vst::JoinPath(directory, kBackendName);
    if (!vst::RejectReparsePath(path, true, error)) return false;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND) { error = vst::WinError("Inspect verified backend cache", code); return false; }
        if (!WriteBackendCache(path, security, error)) return false;
    }
    return LockVerifiedBackend(path, lock, error);
}
bool LoadEmbeddedBackend(const vst::GameIdentity& game, std::string& error) {
    Handle process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_CREATE_THREAD | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, game.pid));
    if (!process || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) { error = vst::WinError("Open supported game"); return false; }
    if (!vst::SameUserAndSession(process.get(), game.pid, error) || !vst::IsNativeX64(process.get(), error) || vst::HasManagedBackend(game.pid, error)) return false;
    // Recheck this exact held process before requesting any code load. There is
    // no arbitrary PID/DLL/entry option: only this embedded, hashed backend.
    std::wstring executable(32768, L'\0'); DWORD length = static_cast<DWORD>(executable.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, executable.data(), &length)) { error = vst::WinError("Recheck game identity"); return false; }
    executable.resize(length);
    if (!vst::EqualPath(executable, game.executable)) { error = "The selected game process changed."; return false; }
    if (!vst::VerifyGameDirectory(game.directory, error)) return false;
    std::wstring backend; Handle cache_lock;
    if (!ExtractBackend(backend, cache_lock, error)) return false;
    if (!CheckResidentBackend(game.pid, backend, false, error)) return false;
    struct TargetGuard {
        HANDLE game, remote = nullptr;
        bool pending = false;
        ~TargetGuard() {
            if (!remote || pending) return;
            HANDLE local = nullptr;
            if (DuplicateHandle(game, remote, GetCurrentProcess(), &local, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE) && local) CloseHandle(local);
        }
    } guard{process.get()};
    if (!DuplicateHandle(GetCurrentProcess(), cache_lock.get(), process.get(), &guard.remote, GENERIC_READ, FALSE, 0)) { error = vst::WinError("Hold immutable backend during game loading"); return false; }
    DWORD entry_rva = 0;
    if (!vst::ExportRva(backend, "StartNativeTrainer", entry_rva, error)) return false;
    uintptr_t backend_base = 0; DWORD backend_size = 0;
    if (!vst::FindRemoteModule(game.pid, backend, backend_base, backend_size, error)) {
        auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"); HMODULE owner = nullptr;
        if (!load || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(load), &owner)
            || !vst::IsExecutableAddressInModule(reinterpret_cast<void*>(load), owner)) { error = "Cannot resolve the system library loader."; return false; }
        std::wstring owner_path = vst::ModulePath(owner);
        std::array<wchar_t, 32768> system{}; UINT n = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        if (!n || !vst::EqualPath(vst::ParentPath(owner_path), std::wstring(system.data(), n))) { error = "The local library loader is not in the Windows system directory."; return false; }
        uintptr_t remote_owner = 0; DWORD remote_size = 0;
        if (!vst::FindRemoteModule(game.pid, owner_path, remote_owner, remote_size, error)) return false;
        uintptr_t offset = reinterpret_cast<uintptr_t>(load) - reinterpret_cast<uintptr_t>(owner);
        if (offset >= remote_size) { error = "Invalid system loader offset."; return false; }
        size_t bytes = (backend.size() + 1) * sizeof(wchar_t);
        void* remote_path = VirtualAllocEx(process.get(), nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote_path) { error = vst::WinError("Prepare fixed backend path"); return false; }
        SIZE_T written = 0;
        if (!WriteProcessMemory(process.get(), remote_path, backend.c_str(), bytes, &written) || written != bytes) {
            VirtualFreeEx(process.get(), remote_path, 0, MEM_RELEASE); error = vst::WinError("Transfer fixed backend path"); return false;
        }
        Handle thread(CreateRemoteThread(process.get(), nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_owner + offset), remote_path, 0, nullptr));
        if (!thread) { VirtualFreeEx(process.get(), remote_path, 0, MEM_RELEASE); error = vst::WinError("Load fixed native backend"); return false; }
        if (WaitForSingleObject(thread.get(), 10000) != WAIT_OBJECT_0) {
            // Do not terminate a game thread or free memory it may still read.
            // Its duplicated deny-write/delete file guard survives this client.
            guard.pending = true;
            error = "The game has not completed loading the backend. No effect was enabled."; return false;
        }
        VirtualFreeEx(process.get(), remote_path, 0, MEM_RELEASE);
        if (!vst::FindRemoteModule(game.pid, backend, backend_base, backend_size, error)) return false;
    }
    if (!entry_rva || entry_rva >= backend_size) { error = "Invalid fixed backend entry."; return false; }
    Handle start(CreateRemoteThread(process.get(), nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(backend_base + entry_rva), nullptr, 0, nullptr));
    if (!start) { error = vst::WinError("Start native backend"); return false; }
    if (WaitForSingleObject(start.get(), 10000) != WAIT_OBJECT_0) { guard.pending = true; error = "The native backend is still initializing; no effect was enabled."; return false; }
    DWORD code = 0;
    if (!GetExitCodeThread(start.get(), &code) || (code != ERROR_SUCCESS && code != ERROR_ALREADY_INITIALIZED)) { error = "The native backend refused this game/loader environment."; return false; }
    return true;
}

class Channel {
    Handle pipe_, process_;
    DWORD pid_ = 0;
    unsigned counter_ = 0;
    std::string buffered_;
    std::string backup_proof_;
    bool Io(bool write, void* data, DWORD length, DWORD& transferred, uint64_t deadline, std::string& error) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) { error = vst::WinError("Create pipe event"); return false; }
        OVERLAPPED operation{}; operation.hEvent = event.get();
        BOOL completed = write ? WriteFile(pipe_.get(), data, length, &transferred, &operation) : ReadFile(pipe_.get(), data, length, &transferred, &operation);
        if (!completed) {
            if (GetLastError() != ERROR_IO_PENDING) { error = vst::WinError("Exchange native state"); return false; }
            uint64_t now = GetTickCount64();
            DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
            if (WaitForSingleObject(event.get(), remaining) != WAIT_OBJECT_0) {
                CancelIoEx(pipe_.get(), &operation);
                WaitForSingleObject(event.get(), INFINITE);
                error = "The native game response timed out."; return false;
            }
            if (!GetOverlappedResult(pipe_.get(), &operation, &transferred, FALSE)) { error = vst::WinError("Complete native exchange"); return false; }
        }
        return transferred != 0;
    }
    bool AcceptResponse(std::string& line, const std::string& id, Status& status) {
        int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), nullptr, 0);
        if (!characters || characters > 262144) { status.error = "Native response is invalid UTF-8 or exceeds the character limit."; Close(); return false; }
        yyjson_doc* doc = yyjson_read(line.data(), line.size(), 0);
        if (!doc) { status.error = "Invalid native response."; Close(); return false; }
        auto response = yyjson_doc_get_root(doc), state = yyjson_obj_get(response, "state");
        auto get_string = [](yyjson_val* object, const char* key) -> std::string { const char* s = yyjson_get_str(yyjson_obj_get(object, key)); return s ? s : ""; };
        bool valid = yyjson_is_obj(state) && get_string(response, "id") == id && get_string(yyjson_obj_get(state, "diagnostics"), "backend.kind") == "native-pilot";
        if (!valid) { yyjson_doc_free(doc); status.error = "The response is not from this native backend."; Close(); return false; }
        Status next;
        next.connected = true; next.pid = pid_; next.command_ok = yyjson_get_bool(yyjson_obj_get(response, "ok"));
        next.received_at = GetTickCount64(); next.game_directory = status.game_directory;
        next.player_ready = yyjson_get_bool(yyjson_obj_get(state, "playerReady")); next.scene = get_string(state, "scene");
        next.backup_authorized = yyjson_get_bool(yyjson_obj_get(state, "backupAuthorized"));
        next.wallet_ready = yyjson_get_bool(yyjson_obj_get(state, "walletReady"));
        next.gold_write_supported = yyjson_get_bool(yyjson_obj_get(state, "goldWriteSupported"));
        next.gold_write_ready = yyjson_get_bool(yyjson_obj_get(state, "goldWriteReady"));
        auto gold = yyjson_obj_get(state, "gold"); next.gold_known = yyjson_is_num(gold); next.gold = yyjson_get_num(gold);
        next.error = get_string(response, "error");
        next.error_scope = get_string(response, "errorScope");
        next.reward = vst::client::ParseRewardState(yyjson_obj_get(state, "chestReward"));
        auto catalog_page = vst::client::ParseRewardCatalogPage(response);
        if (catalog_page.present) next.catalog_page = std::make_shared<vst::client::RewardCatalogPage>(std::move(catalog_page));
        next.reward_icon = vst::client::ParseRewardIcon(response);
        auto features = yyjson_obj_get(state, "features"); size_t fi, fm; yyjson_val* feature_value;
        yyjson_arr_foreach(features, fi, fm, feature_value) {
            auto name = get_string(feature_value, "id");
            for (size_t f = 0; f < kFeatures.size(); ++f) if (name == kFeatures[f]) {
                next.features[f].supported = yyjson_get_bool(yyjson_obj_get(feature_value, "supported"));
                next.features[f].enabled = yyjson_get_bool(yyjson_obj_get(feature_value, "enabled"));
                next.features[f].value = yyjson_get_num(yyjson_obj_get(feature_value, "value"));
            }
        }
        auto stats = yyjson_obj_get(state, "stats"); size_t i, max; yyjson_val* stat;
        yyjson_arr_foreach(stats, i, max, stat) {
            const auto* definition = vst::FindNativeStat(get_string(stat, "id"));
            if (!definition) continue;
            auto& numeric = next.stats[static_cast<size_t>(definition->stat)];
            numeric.supported = yyjson_get_bool(yyjson_obj_get(stat, "supported")); numeric.enabled = yyjson_get_bool(yyjson_obj_get(stat, "enabled"));
            numeric.value = yyjson_get_num(yyjson_obj_get(stat, "value")); numeric.reason = get_string(stat, "reason");
            auto original = yyjson_obj_get(stat, "originalValue"), current = yyjson_obj_get(stat, "currentValue");
            numeric.original_known = yyjson_is_num(original); numeric.current_known = yyjson_is_num(current);
            numeric.original = yyjson_get_num(original); numeric.current = yyjson_get_num(current);
            auto bound = yyjson_obj_get(stat, "boundValue"); numeric.bound_known = yyjson_is_num(bound); numeric.bound = yyjson_get_num(bound);
        }
        const auto& armor = next.stats[kArmorIndex];
        next.supported = armor.supported; next.enabled = armor.enabled; next.target = armor.value;
        next.original_known = armor.original_known; next.current_known = armor.current_known;
        next.original = armor.original; next.current = armor.current;
        if (!next.supported) next.armor_reason = armor.reason;
        auto diagnostics = yyjson_obj_get(state, "diagnostics");
        auto paused = get_string(diagnostics, "game.paused");
        next.paused_known = paused == "true" || paused == "false" || paused == "True" || paused == "False";
        next.paused = paused == "true" || paused == "True";
        auto read_text = get_string(diagnostics, "stat.armor.gameReads");
        if (!read_text.empty()) next.reads = std::strtoull(read_text.c_str(), nullptr, 10);
        for (size_t f = 0; f < kFeatures.size(); ++f) {
            auto prefix = std::string("native.") + kFeatures[f];
            next.features[f].calls = std::strtoull(get_string(diagnostics, (prefix + ".calls").c_str()).c_str(), nullptr, 10);
            next.features[f].actual_calls = std::strtoull(get_string(diagnostics, (prefix + ".actualCalls").c_str()).c_str(), nullptr, 10);
            next.features[f].overrides = std::strtoull(get_string(diagnostics, (prefix + ".overrideCalls").c_str()).c_str(), nullptr, 10);
        }
        for (size_t index = 0; index < vst::kNativeStatCount; ++index) {
            auto prefix = std::string("stat.") + vst::kNativeStats[index].id;
            next.stats[index].reads = std::strtoull(get_string(diagnostics, (prefix + ".gameReads").c_str()).c_str(), nullptr, 10);
            auto overrides = get_string(diagnostics, (prefix + ".overrideReads").c_str());
            if (index == kArmorIndex && overrides.empty()) overrides = get_string(diagnostics, "native.armor.overrideReads");
            next.stats[index].overrides = std::strtoull(overrides.c_str(), nullptr, 10);
            next.stats[index].source = get_string(diagnostics, (prefix + ".source").c_str());
        }
        if (next.error.empty()) next.error = get_string(diagnostics, "backend.error");
        if (next.error.empty() && !next.armor_reason.empty()) next.note = "Armor: " + next.armor_reason;
        yyjson_doc_free(doc);
        if (!next.command_ok && next.error == "Connection is no longer active.") {
            // A live pipe can outlast its control session. Release it so a
            // fresh handshake is possible; never retain or reopen effects.
            Close();
            next.connected = false; next.pid = 0; next.enabled = false;
            next.current_known = false;
            for (auto& feature : next.features) feature.enabled = false;
            for (auto& numeric : next.stats) { numeric.enabled = false; numeric.current_known = numeric.bound_known = false; }
            next.reward.enabled = next.reward.upgrade_enabled = false;
            status = std::move(next); return false;
        }
        status = std::move(next); return true;
    }
public:
    void Close() { pipe_.reset(); process_.reset(); pid_ = 0; buffered_.clear(); backup_proof_.clear(); }
    bool Connected() const { return static_cast<bool>(pipe_); }
    bool CheckResponsePolicy() {
        // Exercise the wire parser and owned-handle cleanup without opening
        // a game pipe. These events stand in only for transport ownership.
        auto transport = [&] {
            Close();
            pipe_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            process_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            pid_ = GetCurrentProcessId(); buffered_ = "old-session";
            return pipe_ && process_;
        };
        Status status;
        if (!transport()) return false;
        std::string expired = R"({"id":"session-test","ok":false,"error":"Connection is no longer active.","state":{"playerReady":true,"features":[{"id":"health","supported":true,"enabled":true}],"stats":[{"id":"armor","supported":true,"enabled":true,"currentValue":20},{"id":"maxHealth","supported":true,"enabled":true,"currentValue":500}],"diagnostics":{"backend.kind":"native-pilot"}}})";
        bool valid = !AcceptResponse(expired, "session-test", status)
            && !Connected() && !process_ && pid_ == 0 && buffered_.empty()
            && !status.connected && !status.enabled && !status.current_known
            && !status.features[0].enabled && !status.command_ok
            && status.error == "Connection is no longer active.";
        for (const auto& numeric : status.stats) valid = valid && !numeric.enabled && !numeric.current_known;
        if (!transport()) return false;
        std::string hello = R"({"id":"new-session","ok":true,"error":null,"state":{"playerReady":true,"features":[{"id":"health","supported":true,"enabled":false}],"stats":[{"id":"armor","supported":true,"enabled":false}],"diagnostics":{"backend.kind":"native-pilot"}}})";
        valid = valid && AcceptResponse(hello, "new-session", status)
            && Connected() && status.connected && status.command_ok
            && status.error.empty() && !status.enabled && !status.features[0].enabled;
        std::string rejected = R"({"id":"bad-value","ok":false,"error":"Armor must be a finite number from 0 to 10000.","state":{"playerReady":true,"diagnostics":{"backend.kind":"native-pilot"}}})";
        valid = valid && AcceptResponse(rejected, "bad-value", status)
            && Connected() && status.connected && !status.command_ok;
        std::string numeric = R"({"id":"stats-test","ok":true,"state":{"playerReady":true,"stats":[{"id":"might","supported":true,"enabled":true,"value":100,"originalValue":45,"currentValue":100},{"id":"omni","supported":true,"enabled":false,"value":100,"originalValue":null,"currentValue":null}],"diagnostics":{"backend.kind":"native-pilot","stat.might.gameReads":"7","stat.might.overrideReads":"5","stat.might.source":"omni","stat.omni.gameReads":"12","stat.omni.overrideReads":"9"}}})";
        valid = valid && AcceptResponse(numeric, "stats-test", status);
        const auto& might = status.stats[static_cast<size_t>(vst::NativeStat::Might)];
        const auto& unified = status.stats[static_cast<size_t>(vst::NativeStat::Omni)];
        valid = valid && might.supported && might.enabled && might.value == 100
            && might.original_known && might.original == 45 && might.current_known && might.current == 100
            && might.reads == 7 && might.overrides == 5 && might.source == "omni"
            && unified.supported && !unified.original_known && !unified.current_known && unified.reads == 12 && unified.overrides == 9;
        Close();
        return valid;
    }
    bool Connect(bool allow_load, Status& status, const std::wstring& selected_folder = {}, DWORD detected_pid = 0) {
        Close(); std::string error; vst::GameIdentity game;
        std::wstring expected_executable;
        if (!selected_folder.empty() && !vst::client::ResolveGameFolder(selected_folder, expected_executable, error)) { status.error = error; return false; }
        auto scan = vst::client::ScanRunningGames(expected_executable);
        if (expected_executable.empty()) status.game_scan = scan;
        if (!scan.pid) { status.error = scan.error; return false; }
        if (detected_pid && scan.pid != detected_pid) { status.error = "The selected game process changed."; return false; }
        if (!vst::VerifyGameProcess(scan.pid, game, error)) { status.error = error; return false; }
        if (!expected_executable.empty() && !vst::EqualPath(game.executable, expected_executable)) { status.error = "The selected game process changed."; return false; }
        if (vst::HasManagedBackend(game.pid, error)) { status.error = error; return false; }
        process_.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, game.pid));
        if (!process_) { status.error = vst::WinError("Hold game process"); return false; }
        std::wstring actual(32768, L'\0'); DWORD actual_length = static_cast<DWORD>(actual.size());
        if (!vst::SameUserAndSession(process_.get(), game.pid, error) || !vst::IsNativeX64(process_.get(), error)
            || !QueryFullProcessImageNameW(process_.get(), 0, actual.data(), &actual_length)) { status.error = error.empty() ? "Cannot recheck the game process." : error; Close(); return false; }
        actual.resize(actual_length);
        if (!vst::EqualPath(actual, game.executable)) { status.error = "The game process identity changed."; Close(); return false; }
        // File verification may take longer than the server's first-hello
        // deadline. Finish it before opening that timed pipe session.
        vst::maintenance::Options options; std::wstring proof; std::string proof_error;
        if (vst::maintenance::MakeDefaultOptions(game.directory, L"", options, proof_error)
            && vst::maintenance::LatestSessionProof(options, proof, proof_error)) backup_proof_ = vst::Utf8(proof);
        std::wstring expected_backend;
        if (!BackendPath(expected_backend, error) || !CheckResidentBackend(game.pid, expected_backend, false, error)) { status.error = error; Close(); return false; }
        std::wstring name = L"\\\\.\\pipe\\VampireSurvivors.Trainer.v1." + std::to_wstring(game.pid);
        auto open = [&]() { return CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr); };
        pipe_.reset(open());
        if (!pipe_ && GetLastError() == ERROR_FILE_NOT_FOUND && allow_load) {
            uintptr_t game_assembly = 0; DWORD assembly_size = 0;
            if (!vst::FindRemoteModule(game.pid, vst::JoinPath(game.directory, L"GameAssembly.dll"), game_assembly, assembly_size, error)) {
                status.error = kGameStarting; Close(); return false;
            }
            if (!LoadEmbeddedBackend(game, error)) { status.error = error; Close(); return false; }
            uint64_t deadline = GetTickCount64() + 5000;
            do { pipe_.reset(open()); if (pipe_) break; Sleep(50); } while (GetTickCount64() < deadline && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT);
        }
        if (!pipe_) { status.error = kPipeUnavailable; Close(); return false; }
        if (!CheckResidentBackend(game.pid, expected_backend, true, error)) { status.error = error; Close(); return false; }
        ULONG server_pid = 0, server_session = 0; DWORD game_session = 0;
        if (!GetNamedPipeServerProcessId(pipe_.get(), &server_pid) || server_pid != game.pid || !GetNamedPipeServerSessionId(pipe_.get(), &server_session)
            || !ProcessIdToSessionId(game.pid, &game_session) || game_session != server_session
            || WaitForSingleObject(process_.get(), 0) != WAIT_TIMEOUT) { status.error = "Native pipe identity mismatch."; Close(); return false; }
        pid_ = game.pid;
        status.game_directory = game.directory;
        return Exchange("hello", false, false, 0, status);
    }
    bool Exchange(const char* kind, bool has_enabled, bool enabled, double value, Status& status, const char* feature = nullptr, const char* stat_id = "armor", const vst::client::RewardRequest* reward = nullptr) {
        if (!pipe_ || !process_ || WaitForSingleObject(process_.get(), 0) != WAIT_TIMEOUT) { status.connected = false; status.error = "The game connection is closed."; Close(); return false; }
        std::string id = "native-" + std::to_string(++counter_) + "-" + std::to_string(GetTickCount64());
        yyjson_mut_doc* request = yyjson_mut_doc_new(nullptr); if (!request) { status.error = "Out of memory."; return false; }
        auto root = yyjson_mut_obj(request); yyjson_mut_doc_set_root(request, root);
        yyjson_mut_obj_add_strcpy(request, root, "id", id.c_str()); yyjson_mut_obj_add_strcpy(request, root, "kind", kind);
        if (!backup_proof_.empty() && (std::strcmp(kind, "hello") == 0
            || (std::strcmp(kind, "setFeature") == 0 && feature && std::strcmp(feature, "noGoldSpend") == 0)
            || std::strcmp(kind, "setGold") == 0)) yyjson_mut_obj_add_strcpy(request, root, "backupToken", backup_proof_.c_str());
        if (std::strcmp(kind, "setStat") == 0 || std::strcmp(kind, "setFeature") == 0) {
            if (std::strcmp(kind, "setStat") == 0) yyjson_mut_obj_add_strcpy(request, root, "stat", stat_id);
            else yyjson_mut_obj_add_strcpy(request, root, "feature", feature ? feature : "");
            if (has_enabled) yyjson_mut_obj_add_bool(request, root, "enabled", enabled);
            if (enabled || !has_enabled) yyjson_mut_obj_add_real(request, root, "value", value);
        }
        if (std::strcmp(kind, "setGold") == 0) yyjson_mut_obj_add_real(request, root, "value", value);
        if (reward && std::strcmp(kind, "getItemCatalog") == 0) {
            yyjson_mut_obj_add_uint(request, root, "catalogOffset", reward->catalog_offset);
            yyjson_mut_obj_add_uint(request, root, "catalogLimit", vst::client::kRewardPageSize);
        }
        if (reward && std::strcmp(kind, "setChestReward") == 0) {
            if (!reward->item_id.empty()) yyjson_mut_obj_add_strcpy(request, root, "itemId", reward->item_id.c_str());
            if (reward->enabled >= 0) yyjson_mut_obj_add_bool(request, root, "enabled", reward->enabled != 0);
            if (reward->upgrade_enabled >= 0) yyjson_mut_obj_add_bool(request, root, "upgradeEnabled", reward->upgrade_enabled != 0);
        }
        if (reward && std::strcmp(kind, "getItemIcon") == 0)
            yyjson_mut_obj_add_strcpy(request, root, "itemId", reward->item_id.c_str());
        size_t size = 0; char* raw = yyjson_mut_write(request, 0, &size);
        std::string bytes = raw ? std::string(raw, size) + "\n" : ""; free(raw); yyjson_mut_doc_free(request);
        if (bytes.empty()) { status.error = "Cannot encode the native request."; return false; }
        uint64_t deadline = GetTickCount64() + 2000; DWORD count = 0;
        std::string error;
        size_t sent = 0;
        while (sent < bytes.size()) {
            if (!Io(true, bytes.data() + sent, static_cast<DWORD>(bytes.size() - sent), count, deadline, error)) { status.error = error; status.connected = false; Close(); return false; }
            sent += count;
        }
        while (buffered_.find('\n') == std::string::npos) {
            std::array<char, 4096> chunk{};
            if (!Io(false, chunk.data(), static_cast<DWORD>(chunk.size()), count, deadline, error)) { status.error = error; status.connected = false; Close(); return false; }
            buffered_.append(chunk.data(), count);
            if (buffered_.size() > 1048576) { status.error = "Native response is too large."; Close(); return false; }
        }
        size_t newline = buffered_.find('\n'); std::string line = buffered_.substr(0, newline); buffered_.erase(0, newline + 1);
        return AcceptResponse(line, id, status);
    }
};

enum class Action { Connect, Toggle, Feature, Stat, Disable, Stop, SaveSettings, CreateBackup, ListBackups, RestoreBackup, SetGold, RewardCatalog, SetReward, RewardIcon };
struct Command {
    Action action; double value = 20; size_t feature = 0; bool enabled = true, live = false; size_t stat = kArmorIndex;
    uint64_t sequence = 0;
    uint64_t cancelled_gold_sequence = 0;
    std::shared_ptr<vst::client::ClientSettings> settings;
    std::wstring game_directory, manifest;
    vst::client::RewardRequest reward_request;
    uint64_t catalog_generation = 0;
};
bool ControlAction(Action action) { return action == Action::Connect || action == Action::Toggle || action == Action::Feature || action == Action::Stat || action == Action::Disable || action == Action::Stop || action == Action::SetGold || action == Action::SetReward; }
bool RetryGameConnection(std::string_view error) {
    return error == kGameStarting || error == kPipeUnavailable || error == "The native game response timed out."
        || error == "The game has not completed loading the backend. No effect was enabled."
        || error == "The native backend is still initializing; no effect was enabled."
        || error.starts_with("Inspect game modules (Windows ") || error.starts_with("Verify resident backend (Windows ");
}
bool PostStatus(const Status& status) {
    auto result = new (std::nothrow) Status(status);
    if (!result) return false;
    if (PostMessageW(window, kStatusMessage, 0, reinterpret_cast<LPARAM>(result))) return true;
    delete result;
    return false;
}
struct RewardIconResult {
    uint64_t generation;
    std::string requested_id;
    bool ok;
    vst::client::RewardIcon icon;
};
struct MaintenanceResult {
    Action action = Action::ListBackups;
    bool ok = false;
    std::string error;
    std::wstring game_directory;
    std::vector<vst::maintenance::BackupInfo> entries;
    vst::maintenance::BackupInfo backup;
    vst::maintenance::RestoreResult restored;
};
bool SameEffect(const Command& first, const Command& second) {
    if (first.action == Action::Feature || second.action == Action::Feature)
        return first.action == second.action && first.feature == second.feature;
    auto stat_command = [](Action action) { return action == Action::Toggle || action == Action::Stat; };
    return stat_command(first.action) && stat_command(second.action)
        && (first.action == Action::Toggle ? kArmorIndex : first.stat) == (second.action == Action::Toggle ? kArmorIndex : second.stat);
}
Command LiveCommand(size_t target, double value) {
    Command command{target < kStatTargetOffset ? Action::Feature : Action::Stat, value};
    command.live = true;
    if (target < kStatTargetOffset) command.feature = LiveFeatureIndex(target);
    else command.stat = target - kStatTargetOffset;
    return command;
}
class Worker {
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable signal_;
    std::deque<Command> commands_;
    bool working_control_ = false;
    bool automatic_enabled_ = true, choosing_directory_ = false, connecting_automatically_ = false;
    uint64_t next_sequence_ = 0;
    uint64_t last_control_sequence_ = 0, ui_acked_sequence_ = 0;
    void Loop() try {
        Channel channel; Status status; uint64_t completed_sequence = 0, last_status_post = 0;
        uint64_t next_game_scan = 0;
        vst::client::AutomaticConnectionSchedule automatic;
        for (;;) {
            Command command{}; bool have = false;
            {
                std::unique_lock lock(mutex_);
                const uint64_t now = GetTickCount64();
                const uint64_t wait_ms = channel.Connected() ? kStatusIntervalMs : next_game_scan > now ? next_game_scan - now : 0;
                signal_.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] { return !commands_.empty(); });
                if (!commands_.empty()) { command = std::move(commands_.front()); commands_.pop_front(); have = true; }
                working_control_ = have && ControlAction(command.action);
            }
            if (have && command.action == Action::Stop) {
                if (channel.Connected()) channel.Exchange("disableAll", false, false, 0, status);
                if (command.settings) WriteClientSettings(*command.settings);
                channel.Close(); PostMessageW(window, kStoppedMessage, 0, 0); return;
            }
            if (have && command.action == Action::SaveSettings) {
                bool saved = command.settings && WriteClientSettings(*command.settings);
                PostMessageW(window, kSettingsMessage, saved ? 1 : 0, 0);
                if (channel.Connected()) channel.Exchange("heartbeat", false, false, 0, status);
            } else if (have && (command.action == Action::CreateBackup || command.action == Action::ListBackups || command.action == Action::RestoreBackup)) {
                auto result = new (std::nothrow) MaintenanceResult();
                if (result) {
                    result->action = command.action;
                    vst::maintenance::Options options;
                    if (vst::maintenance::MakeDefaultOptions(command.game_directory, L"", options, result->error)) {
                        result->game_directory = options.game_directory;
                        if (command.action == Action::CreateBackup) result->ok = vst::maintenance::CreateBackup(options, result->backup, result->error);
                        else if (command.action == Action::RestoreBackup) result->ok = vst::maintenance::RestoreBackup(options, command.manifest, result->restored, result->error);
                        else result->ok = true;
                        if (result->ok) result->ok = vst::maintenance::ListBackups(options, result->entries, result->error);
                    }
                    if (!PostMessageW(window, kMaintenanceMessage, 0, reinterpret_cast<LPARAM>(result))) delete result;
                } else PostMessageW(window, kMaintenanceMessage, 0, 0);
            }
            else if (have && command.action == Action::Connect) {
                if (!channel.Connected()) {
                    automatic.Observe(0, GetTickCount64());
                    auto scan = std::move(status.game_scan);
                    status = {};
                    status.game_scan = std::move(scan);
                    channel.Connect(true, status, command.game_directory);
                    if (command.game_directory.empty() && !channel.Connected()) {
                        automatic.Observe(status.game_scan.pid, GetTickCount64());
                        automatic.Complete(GetTickCount64(), RetryGameConnection(status.error));
                    }
                }
                else channel.Exchange("getState", false, false, 0, status);
                next_game_scan = GetTickCount64() + kGameScanIntervalMs;
            } else if (have && command.live && !LiveTargetEnabled(status, command.action == Action::Feature ? FeatureLiveTarget(command.feature) : kStatTargetOffset + command.stat)) {
                if (channel.Connected()) channel.Exchange("heartbeat", false, false, 0, status);
            } else if (have && command.action == Action::Toggle) channel.Exchange("setStat", !command.live, command.enabled, command.value, status);
            else if (have && command.action == Action::Feature && command.feature < kFeatures.size())
                channel.Exchange("setFeature", !command.live, command.enabled, command.value, status, kFeatures[command.feature]);
            else if (have && command.action == Action::Stat && command.stat < vst::kNativeStatCount)
                channel.Exchange("setStat", !command.live, command.enabled, command.value, status, nullptr, vst::kNativeStats[command.stat].id);
            else if (have && command.action == Action::Disable) channel.Exchange("disableAll", false, false, 0, status);
            else if (have && command.action == Action::SetGold) channel.Exchange("setGold", false, false, command.value, status);
            else if (have && command.action == Action::RewardCatalog) channel.Exchange("getItemCatalog", false, false, 0, status, nullptr, "armor", &command.reward_request);
            else if (have && command.action == Action::RewardIcon) channel.Exchange("getItemIcon", false, false, 0, status, nullptr, "armor", &command.reward_request);
            else if (have && command.action == Action::SetReward) channel.Exchange("setChestReward", false, false, 0, status, nullptr, "armor", &command.reward_request);
            else if (channel.Connected()) channel.Exchange("heartbeat", false, false, 0, status);
            else if (GetTickCount64() >= next_game_scan) {
                auto scan = vst::client::ScanRunningGames();
                const uint64_t now = GetTickCount64();
                next_game_scan = now + kGameScanIntervalMs;
                automatic.Observe(scan.pid, now);
                bool attempt = false;
                if (automatic.Due(now)) {
                    std::lock_guard lock(mutex_);
                    attempt = automatic_enabled_ && !choosing_directory_ && commands_.empty();
                    connecting_automatically_ = working_control_ = attempt;
                }
                if (attempt) {
                    status = {};
                    status.game_scan = scan;
                    status.client_sequence = completed_sequence;
                    status.connecting_automatically = true;
                    PostStatus(status);
                    channel.Connect(true, status, {}, scan.pid);
                    status.connecting_automatically = false;
                    if (channel.Connected()) automatic.Observe(0, GetTickCount64());
                    else automatic.Complete(GetTickCount64(), RetryGameConnection(status.error));
                } else {
                    if (scan == status.game_scan) continue;
                    if (status.error == vst::client::kNoRunningGame || status.error == vst::client::kMultipleGames) status.error.clear();
                    status.game_scan = std::move(scan);
                }
            }
            else continue;
            if (!channel.Connected()) status.connected = false;
            status.reward_result = have && command.action == Action::SetReward;
            status.gold_cancelled = have && command.cancelled_gold_sequence != 0;
            status.gold_reply_sequence = !have ? 0 : command.action == Action::SetGold ? command.sequence : command.cancelled_gold_sequence;
            if (have && command.action == Action::RewardCatalog) { status.catalog_generation = command.catalog_generation; status.catalog_offset = command.reward_request.catalog_offset; }
            if (have) completed_sequence = std::max(completed_sequence, command.sequence);
            status.client_sequence = completed_sequence;
            { std::lock_guard lock(mutex_); if (have && ControlAction(command.action)) last_control_sequence_ = std::max(last_control_sequence_, command.sequence); connecting_automatically_ = working_control_ = false; }
            const uint64_t now = GetTickCount64();
            if (have && command.action == Action::RewardIcon) {
                auto icon = new (std::nothrow) RewardIconResult{command.catalog_generation, std::move(command.reward_request.item_id),
                    status.connected && status.command_ok, std::move(status.reward_icon)};
                status.reward_icon = {};
                if (icon && !PostMessageW(window, kRewardIconMessage, 0, reinterpret_cast<LPARAM>(icon))) delete icon;
                if (status.connected) {
                    status.error.clear();
                    // Keep status fresh during a burst of icons without
                    // repainting the application for every individual image.
                    if (now - last_status_post < kStatusIntervalMs) continue;
                }
            }
            if (PostStatus(status)) last_status_post = now;
        }
    } catch (...) {
        auto result = new (std::nothrow) Status();
        if (result) { result->error = "The native client operation failed."; if (!PostMessageW(window, kStatusMessage, 0, reinterpret_cast<LPARAM>(result))) delete result; }
        PostMessageW(window, kStoppedMessage, 0, 0);
    }
public:
    void Start() { thread_ = std::thread([this] { Loop(); }); }
    bool CheckLatestDraftPolicy();
    bool AutomaticEnabled() { std::lock_guard lock(mutex_); return automatic_enabled_; }
    bool BeginGameSelection() {
        std::lock_guard lock(mutex_);
        if (connecting_automatically_ || choosing_directory_) return false;
        choosing_directory_ = true;
        return true;
    }
    void EndGameSelection() {
        std::lock_guard lock(mutex_);
        choosing_directory_ = false;
    }
    void Acknowledge(uint64_t sequence) { std::lock_guard lock(mutex_); ui_acked_sequence_ = std::max(ui_acked_sequence_, sequence); }
    bool ControlPending() { std::lock_guard lock(mutex_); return working_control_ || ui_acked_sequence_ < last_control_sequence_ || std::any_of(commands_.begin(), commands_.end(), [](const Command& command) { return ControlAction(command.action); }); }
    bool Push(Command command, uint64_t* assigned_sequence = nullptr) {
        std::lock_guard lock(mutex_);
        if (command.action == Action::Stop) commands_.clear();
        if (command.action == Action::Disable) std::erase_if(commands_, [&](const Command& previous) {
            if (previous.action == Action::SetGold) command.cancelled_gold_sequence = previous.sequence;
            else if (previous.cancelled_gold_sequence) command.cancelled_gold_sequence = previous.cancelled_gold_sequence;
            return ControlAction(previous.action);
        });
        if (command.action == Action::SaveSettings) std::erase_if(commands_, [](const Command& previous) { return previous.action == Action::SaveSettings; });
        auto stamp = [&] { if (++next_sequence_ == 0) ++next_sequence_; command.sequence = next_sequence_; if (assigned_sequence) *assigned_sequence = command.sequence; };
        if (command.action == Action::SetReward && command.reward_request.enabled == 0 && command.reward_request.upgrade_enabled == 0) {
            std::erase_if(commands_, [](const Command& prior) { return prior.action == Action::SetReward; });
            stamp(); commands_.push_front(std::move(command)); signal_.notify_one(); return true;
        }
        if ((command.action == Action::Toggle || command.action == Action::Feature || command.action == Action::Stat) && !command.live && !command.enabled) {
            std::erase_if(commands_, [&](const Command& prior) { return SameEffect(prior, command); });
            stamp(); commands_.push_front(std::move(command)); signal_.notify_one(); return true;
        }
        if (command.live) std::erase_if(commands_, [&](const Command& prior) { return prior.live && SameEffect(prior, command); });
        if (commands_.size() < 8) {
            if (command.action == Action::Connect) automatic_enabled_ = command.game_directory.empty();
            stamp();
            if (command.action == Action::Disable) commands_.push_front(std::move(command));
            else commands_.push_back(std::move(command));
            signal_.notify_one();
            return true;
        }
        return false;
    }
    void DiscardLive(size_t target) {
        std::lock_guard lock(mutex_);
        Command command = LiveCommand(target, 0);
        std::erase_if(commands_, [&](const Command& prior) { return prior.live && SameEffect(prior, command); });
    }
    void DiscardAllLive() {
        std::lock_guard lock(mutex_);
        std::erase_if(commands_, [](const Command& command) { return command.live; });
    }
    bool CheckQueuePolicy() {
        for (int i = 0; i < 8; ++i) Push({Action::Toggle});
        Push({Action::Disable});
        if (commands_.size() != 1 || commands_.front().action != Action::Disable) return false;
        for (int i = 0; i < 8; ++i) Push({Action::RewardIcon});
        Push({Action::Stop});
        bool valid = commands_.size() == 1 && commands_.front().action == Action::Stop;
        commands_.clear();
        Push({Action::Feature, 2, 1, true}); Push({Action::Feature, 3, 2, true}); Push({Action::Feature, 2, 1, false});
        valid = valid && commands_.size() == 2 && commands_.front().feature == 1 && !commands_.front().enabled
            && commands_.back().feature == 2 && commands_.back().enabled;
        commands_.clear();
        Push(LiveCommand(2, 2)); Push(LiveCommand(2, 3)); Push(LiveCommand(2, 0.5));
        valid = valid && commands_.size() == 1 && commands_.front().value == 0.5 && commands_.front().live;
        Push({Action::Feature, 1, 3, false});
        valid = valid && commands_.size() == 1 && !commands_.front().enabled && !commands_.front().live;
        commands_.clear();
        Push(LiveCommand(kStatTargetOffset + kArmorIndex, 50));
        Push({Action::Toggle, 20, 0, false});
        valid = valid && commands_.size() == 1 && commands_.front().action == Action::Toggle && !commands_.front().enabled;
        commands_.clear();
        Push(LiveCommand(0, 5)); Push(LiveCommand(kStatTargetOffset, 500)); DiscardLive(0);
        valid = valid && commands_.size() == 1 && commands_.front().stat == 0;
        Push({Action::Disable});
        valid = valid && commands_.size() == 1 && commands_.front().action == Action::Disable;
        commands_.clear();
        Push(LiveCommand(2, 5));
        // The worker has taken 5 but its reply has not reached the UI, which
        // still reports the previous 2. Returning to 2 must enqueue that edit.
        commands_.pop_front();
        Push(LiveCommand(2, 2));
        valid = valid && commands_.size() == 1 && commands_.front().live && commands_.front().value == 2;
        commands_.clear();
        Command reward_enable{Action::SetReward}; reward_enable.reward_request = {"MAGIC_MISSILE", 1, 1};
        Command reward_target{Action::SetReward}; reward_target.reward_request.item_id = "SPINACH";
        Push(reward_enable); Push(reward_target);
        Command reward_stop_command{Action::SetReward}; reward_stop_command.reward_request.enabled = reward_stop_command.reward_request.upgrade_enabled = 0;
        Push(reward_stop_command);
        valid = valid && commands_.size() == 1 && commands_.front().action == Action::SetReward && commands_.front().reward_request.item_id.empty()
            && commands_.front().reward_request.enabled == 0 && commands_.front().reward_request.upgrade_enabled == 0;
        Push(reward_enable); Push({Action::Disable});
        valid = valid && commands_.size() == 1 && commands_.front().action == Action::Disable;
        commands_.clear();
        uint64_t gold_request = 0;
        Push({Action::SetGold, 1000}, &gold_request);
        Push({Action::Disable}); Push({Action::Disable});
        valid = valid && gold_request && commands_.size() == 1 && commands_.front().action == Action::Disable
            && commands_.front().cancelled_gold_sequence == gold_request;
        commands_.clear(); return valid;
    }
    void Join() { if (thread_.joinable()) thread_.join(); }
} worker;

void QueueSettingsSave() {
    if (diagnostics_mode || !settings_dirty || closing) return;
    if (!settings_writable) { settings_notice = "Settings were not saved; changes apply only in this window."; return; }
    Command command{Action::SaveSettings}; command.settings = std::make_shared<vst::client::ClientSettings>(client_settings);
    if (worker.Push(std::move(command))) { settings_dirty = false; KillTimer(window, kSettingsTimer); }
}
void MarkSettingsDirty() {
    if (diagnostics_mode || syncing_edits || closing) return;
    settings_dirty = true; SetTimer(window, kSettingsTimer, 750, nullptr);
}

HWND LiveControl(size_t target) {
    if (target < kStatTargetOffset) return feature_edits[LiveFeatureIndex(target)];
    if (target >= kLiveTargetCount) return nullptr;
    size_t index = target - kStatTargetOffset;
    return index == kArmorIndex && !stats_page ? number_edit : stat_edits[index];
}
bool ReadLiveValue(size_t target, double& value) {
    HWND edit = LiveControl(target);
    if (!edit || GetWindowTextLengthW(edit) >= 128) return false;
    wchar_t text[128]{}; GetWindowTextW(edit, text, 128);
    return ParseLiveValue(target, text, value);
}
void FlushLiveValue(size_t target, bool force) {
    if (!live_values.Due(target, GetTickCount64(), force) || !LiveTargetEnabled(displayed, target)) return;
    double value = 0;
    if (!ReadLiveValue(target, value)) { live_values.Cancel(target); worker.DiscardLive(target); return; }
    // The displayed response can predate an in-flight older edit. Always
    // queue the latest valid draft, even when it equals the displayed value.
    if (worker.Push(LiveCommand(target, value))) live_values.Cancel(target);
}
void WakeLiveValues() {
    bool pending = false;
    for (size_t target = 0; target < kLiveTargetCount; ++target) pending = pending || (live_values.Dirty(target) && LiveTargetEnabled(displayed, target));
    if (pending) SetTimer(window, kLiveValueTimer, 50, nullptr);
    else KillTimer(window, kLiveValueTimer);
}
void ChangedLiveValue(size_t target) {
    if (syncing_edits || target == kNoLiveTarget) return;
    worker.DiscardLive(target);
    double value = 0;
    if (ReadLiveValue(target, value)) {
        live_values.Changed(target, GetTickCount64());
        if (client_settings.values[target] != value) { client_settings.values[target] = value; MarkSettingsDirty(); }
    }
    else live_values.Cancel(target);
    WakeLiveValues();
}
void CancelLiveValue(size_t target) { live_values.Cancel(target); worker.DiscardLive(target); }
void CancelAllLiveValues() { live_values.CancelAll(); worker.DiscardAllLive(); KillTimer(window, kLiveValueTimer); }
bool Worker::CheckLatestDraftPolicy() {
    if (thread_.joinable() || !commands_.empty()) return false;
    // Exercise the same edit-reading and flush path as WM_TIMER/Enter using
    // an owned invisible edit. No controller thread or game pipe is started.
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND edit = parent ? CreateWindowExW(0, L"EDIT", L"2", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 1, 1, parent, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    if (!edit) { if (parent) DestroyWindow(parent); return false; }
    Status previous = displayed;
    HWND previous_edit = feature_edits[3];
    auto previous_schedule = live_values;
    displayed = {}; displayed.connected = displayed.player_ready = true;
    displayed.features[3].supported = displayed.features[3].enabled = true; displayed.features[3].value = 2;
    feature_edits[3] = edit;
    Push(LiveCommand(2, 5)); commands_.pop_front();
    live_values.Changed(2, GetTickCount64()); FlushLiveValue(2, true);
    bool valid = commands_.size() == 1 && commands_.front().live && commands_.front().value == 2 && !live_values.Dirty(2);
    SetWindowTextW(edit, L"0."); live_values.Changed(2, GetTickCount64()); FlushLiveValue(2, true);
    valid = valid && commands_.empty() && !live_values.Dirty(2);
    // A valid draft entered before the enable response is retained, and only
    // becomes sendable once that enabled state is confirmed.
    displayed.features[3].enabled = false;
    SetWindowTextW(edit, L"0.5"); live_values.Changed(2, GetTickCount64()); FlushLiveValue(2, true);
    valid = valid && commands_.empty() && live_values.Dirty(2);
    displayed.features[3].enabled = true; FlushLiveValue(2, true);
    valid = valid && commands_.size() == 1 && commands_.front().value == 0.5;
    Push({Action::Feature, 1, 3, false});
    valid = valid && commands_.size() == 1 && !commands_.front().live && !commands_.front().enabled;
    commands_.clear(); displayed = std::move(previous); feature_edits[3] = previous_edit; live_values = previous_schedule;
    DestroyWindow(parent);
    return valid;
}
bool CheckLiveValuePolicy() {
    double value = 0;
    if (!ParseLiveValue(2, L"0.5", value) || value != 0.5 || !ParseLiveValue(2, L" 10 ", value)
        || ParseLiveValue(2, L"", value) || ParseLiveValue(2, L"0.", value) || ParseLiveValue(2, L"2e", value)
        || ParseLiveValue(2, L"nan", value) || ParseLiveValue(2, L"0.2", value) || ParseLiveValue(2, L"10.1", value)
        || ParseLiveValue(0, L"0.5", value) || ParseLiveValue(1, L"1001", value)
        || !ParseLiveValue(kStatTargetOffset + kArmorIndex, L"0", value)
        || !ParseLiveValue(kStatTargetOffset + kArmorIndex, L"10000", value)
        || ParseLiveValue(kStatTargetOffset + static_cast<size_t>(vst::NativeStat::Amount), L"2.5", value)) return false;
    LiveValueSchedule edits;
    edits.Changed(2, 1000);
    if (edits.Due(2, 1299) || !edits.Due(2, 1300)) return false;
    edits.Changed(2, 1250);
    if (edits.Due(2, 1300) || !edits.Due(2, 1550) || !edits.Due(2, 1251, true)) return false;
    edits.Cancel(2);
    if (edits.Due(2, 2000, true)) return false;
    edits.Changed(0, 2000); edits.Changed(kStatTargetOffset, 2000); edits.CancelAll();
    if (edits.Dirty(0) || edits.Dirty(kStatTargetOffset)) return false;
    Status status;
    status.connected = status.player_ready = true;
    status.features[3].supported = status.features[3].enabled = true;
    if (!LiveTargetEnabled(status, 2)) return false;
    status.features[3].enabled = false;
    return !LiveTargetEnabled(status, 2) && !LiveTargetEnabled(status, kStatTargetOffset) && worker.CheckLatestDraftPolicy();
}

DWORD ForegroundProcess() { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid; }
bool KeyDown(UINT key) { return (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0; }
void SuspendHotkeys() {
    hotkey_manager.Suspend([](size_t index) { if (window) UnregisterHotKey(window, vst::client::kHotkeyMessageBase + static_cast<int>(index)); });
}
bool FreshStatus(const Status& status) {
    uint64_t now = GetTickCount64();
    return status.connected && status.received_at && now >= status.received_at && now - status.received_at <= 1000;
}
bool FeatureCanToggle(const Status& status, size_t feature) {
    if (!status.connected || feature >= kFeatures.size() || !status.features[feature].supported) return false;
    return status.features[feature].enabled || (feature == 6 ? status.backup_authorized && status.wallet_ready : status.player_ready);
}
std::array<bool, vst::client::kHotkeyCount> EligibleHotkeys(const Status& status) {
    std::array<bool, vst::client::kHotkeyCount> result{};
    for (size_t action = 0; action < kHotkeyFeatures.size(); ++action) result[action] = FeatureCanToggle(status, kHotkeyFeatures[action]);
    for (size_t index = 0; index < vst::kNativeStatCount; ++index) result[vst::client::kFeatureHotkeyCount + index] = status.connected && status.stats[index].supported
        && (status.stats[index].enabled || status.player_ready);
    return result;
}
bool HotkeysAllowed() {
    return !diagnostics_mode && !closing && !maintenance_busy && capturing_hotkey == vst::client::kHotkeyCount
        && !hotkey_pending_sequence && !worker.ControlPending() && FreshStatus(displayed) && displayed.error.empty()
        && displayed.pid && ForegroundProcess() == displayed.pid;
}
void UpdateHotkeys() {
    const auto eligible = EligibleHotkeys(displayed);
    const bool track = !diagnostics_mode && !closing && displayed.connected
        && vst::client::HasUsableHotkey(client_settings.hotkeys, eligible);
    if (track && !hotkey_timer_armed) hotkey_timer_armed = SetTimer(window, kHotkeyTimer, 50, nullptr) != 0;
    if (!track && hotkey_timer_armed) { KillTimer(window, kHotkeyTimer); hotkey_timer_armed = false; }
    bool changed = hotkey_manager.Update(displayed.pid, track && HotkeysAllowed(), client_settings.hotkeys, eligible, KeyDown,
        [](size_t index, vst::client::HotkeyChord chord, DWORD& error) {
            bool valid = RegisterHotKey(window, vst::client::kHotkeyMessageBase + static_cast<int>(index), chord.modifiers | MOD_NOREPEAT, chord.key) != FALSE;
            if (!valid) error = GetLastError();
            return valid;
        }, [](size_t index) { UnregisterHotKey(window, vst::client::kHotkeyMessageBase + static_cast<int>(index)); });
    if (changed && hotkeys_page && !IsIconic(window)) Refresh();
}
std::wstring HotkeyActionName(size_t action) {
    constexpr const wchar_t* chinese[]{L"生命值不减", L"一击必杀", L"经验倍率", L"伤害倍率", L"移动速度倍率", L"快速冷却", L"金币免消耗", L"游戏加速", L"跳过宝箱动画"};
    constexpr const wchar_t* english_names[]{L"No health loss", L"One-hit kill", L"Experience multiplier", L"Damage multiplier", L"Movement multiplier", L"Fast cooldown", L"No gold spending", L"Game speed", L"Skip chest animation"};
    if (action < vst::client::kFeatureHotkeyCount) return Text(chinese[action], english_names[action]);
    const auto& definition = vst::kNativeStats[action - vst::client::kFeatureHotkeyCount];
    return Text(definition.name_zh, definition.name_en);
}
void CancelHotkeyCapture() { capturing_hotkey = vst::client::kHotkeyCount; capture_notice.clear(); SuspendHotkeys(); }
void BeginHotkeyCapture(size_t action) {
    if (action >= vst::client::kHotkeyCount) return;
    CancelHotkeyCapture(); capturing_hotkey = action;
    SetForegroundWindow(window); SetFocus(window); Refresh();
}
bool CaptureHotkey(UINT key) {
    if (capturing_hotkey >= vst::client::kHotkeyCount) return false;
    if (KeyDown(VK_LWIN) || KeyDown(VK_RWIN)) return true;
    if (key == VK_ESCAPE) { CancelHotkeyCapture(); Refresh(); return true; }
    UINT modifiers = (KeyDown(VK_CONTROL) ? MOD_CONTROL : 0u) | (KeyDown(VK_MENU) ? MOD_ALT : 0u) | (KeyDown(VK_SHIFT) ? MOD_SHIFT : 0u);
    bool clear = !modifiers && (key == VK_DELETE || key == VK_BACK);
    vst::client::HotkeyChord chord = clear ? vst::client::HotkeyChord{} : vst::client::HotkeyChord{key, modifiers};
    if (!vst::client::ValidHotkey(chord)) return true;
    if (vst::client::ConflictingHotkey(client_settings.hotkeys, capturing_hotkey, chord) != vst::client::kHotkeyCount) {
        capture_notice = Text(L"此快捷键已绑定其他功能，请先清除那个绑定。", L"That key is already bound to another action. Clear it there first."); Refresh(); return true;
    }
    if (chord.key) {
        int probe = vst::client::kHotkeyMessageBase + static_cast<int>(vst::client::kHotkeyCount);
        if (!RegisterHotKey(window, probe, chord.modifiers | MOD_NOREPEAT, chord.key)) {
            capture_notice = Text(L"系统无法注册此快捷键，请选择另一组。", L"Windows cannot register that hotkey. Choose another combination."); Refresh(); return true;
        }
        UnregisterHotKey(window, probe);
    }
    client_settings.hotkeys[capturing_hotkey] = chord; MarkSettingsDirty(); CancelHotkeyCapture(); UpdateHotkeys(); Refresh(); return true;
}
void ActivateHotkey(size_t action) {
    Command command{Action::Feature};
    if (action < vst::client::kFeatureHotkeyCount) {
        const size_t feature = kHotkeyFeatures[action];
        if (!FeatureCanToggle(displayed, feature)) return;
        command.feature = feature; command.enabled = !displayed.features[feature].enabled; command.value = 1;
        const size_t target = FeatureLiveTarget(feature);
        if (target != kNoLiveTarget) {
            if (command.enabled && !ReadLiveValue(target, command.value)) return;
            CancelLiveValue(target);
        }
    } else if (action < vst::client::kHotkeyCount) {
        command.action = Action::Stat; command.stat = action - vst::client::kFeatureHotkeyCount;
        command.enabled = !displayed.stats[command.stat].enabled; command.value = displayed.stats[command.stat].value;
        if (command.enabled && !ReadLiveValue(kStatTargetOffset + command.stat, command.value)) return;
        CancelLiveValue(kStatTargetOffset + command.stat);
    } else return;
    worker.Push(std::move(command), &hotkey_pending_sequence);
    SuspendHotkeys();
}

std::wstring ControlText(HWND control, size_t limit = 32767) {
    int length = GetWindowTextLengthW(control);
    if (length < 0 || static_cast<size_t>(length) > limit) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0'); GetWindowTextW(control, text.data(), length + 1); text.resize(length); return text;
}
std::wstring PickGameDirectory(bool for_connection) {
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        MessageBoxW(window, Text(L"无法打开文件夹选择窗口，请重试。", L"The folder picker could not open. Try again.").c_str(), L"Vampire Survivors Trainer", MB_OK);
        return {};
    }
    DWORD options = 0; dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT);
    const auto title = for_connection ? Text(L"选择包含 VampireSurvivors.exe 的文件夹", L"Choose the folder containing VampireSurvivors.exe")
        : Text(L"选择游戏目录", L"Choose the game folder");
    dialog->SetTitle(title.c_str());
    if (for_connection) dialog->SetOkButtonLabel(Text(L"连接", L"Connect").c_str());
    std::wstring selected;
    if (SUCCEEDED(dialog->Show(window))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { selected = path; CoTaskMemFree(path); }
            item->Release();
        }
    }
    dialog->Release();
    return selected;
}
void BeginConnection(std::wstring folder = {}) {
    if (closing || diagnostics_mode || displayed.connected || displayed.connecting_automatically || connection_sequence) return;
    Command command{Action::Connect};
    command.game_directory = std::move(folder);
    CancelAllLiveValues();
    if (!worker.Push(std::move(command), &connection_sequence)) {
        MessageBoxW(window, Text(L"修改器正在处理其他操作，请稍后再试。", L"The trainer is busy. Try again shortly.").c_str(), L"Vampire Survivors Trainer", MB_OK);
    }
    Refresh();
}
void BeginMaintenance(Action action) {
    if (maintenance_busy || closing) return;
    Command command{action}; command.game_directory = ControlText(game_directory_edit);
    if (command.game_directory.empty()) { maintenance_notice = "Choose the installed game folder first."; Refresh(); return; }
    if (action == Action::RestoreBackup) {
        LRESULT selection = SendMessageW(backup_list, LB_GETCURSEL, 0, 0);
        if (selection == LB_ERR || static_cast<size_t>(selection) >= backup_entries.size()) return;
        if (MessageBoxW(window, Text(L"恢复所选备份？当前进度会先建立保护快照。请先退出游戏和 Steam。", L"Restore this backup? A safety snapshot of the current progress is created first. Exit the game and Steam.").c_str(),
            Text(L"恢复备份", L"Restore backup").c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        command.manifest = backup_entries[static_cast<size_t>(selection)].manifest_path;
    }
    if (worker.Push(std::move(command))) { maintenance_busy = true; maintenance_notice.clear(); SuspendHotkeys(); Refresh(); }
}
bool ReadGoldValue(double& value) {
    std::wstring text = ControlText(gold_value_edit, 32); wchar_t* end = nullptr;
    value = std::wcstod(text.c_str(), &end);
    return !text.empty() && end && end != text.c_str() && !*end && std::isfinite(value) && value >= 0 && value <= 999999999
        && value == std::trunc(value) && static_cast<double>(static_cast<float>(value)) == value;
}
void RefreshBackupList() {
    LRESULT previous = SendMessageW(backup_list, LB_GETCURSEL, 0, 0);
    SendMessageW(backup_list, LB_RESETCONTENT, 0, 0);
    for (const auto& backup : backup_entries) {
        std::wstring label = vst::Wide(backup.created_utc) + Text(L"  · 文件数：", L"  · Files: ") + std::to_wstring(backup.file_count);
        SendMessageW(backup_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    if (!backup_entries.empty()) SendMessageW(backup_list, LB_SETCURSEL, previous >= 0 && static_cast<size_t>(previous) < backup_entries.size() ? previous : 0, 0);
}

const vst::client::RewardItem* RewardById(std::string_view id) {
    auto found = std::find_if(reward_catalog.begin(), reward_catalog.end(), [&](const auto& item) { return item.id == id; });
    return found == reward_catalog.end() ? nullptr : &*found;
}
std::wstring RewardDisplayName(const vst::client::RewardItem& item) {
    const auto& name = english ? item.english_name : item.name;
    return name.empty() ? item.wide_id : name;
}
std::wstring RewardName(std::string_view id, const std::wstring& fallback = {}) {
    if (const auto* item = RewardById(id)) return RewardDisplayName(*item);
    return !english && !fallback.empty() ? fallback : id.empty() ? L"—" : vst::Wide(std::string(id));
}
const vst::client::RewardItem* SelectedRewardItem() {
    int row = ListView_GetNextItem(reward_list, -1, LVNI_SELECTED);
    return row >= 0 && static_cast<size_t>(row) < reward_filtered.size() && reward_filtered[static_cast<size_t>(row)] < reward_catalog.size()
        ? &reward_catalog[reward_filtered[static_cast<size_t>(row)]] : nullptr;
}
void FilterRewards() {
    auto query = ControlText(reward_search, 512);
    LRESULT selected_filter = SendMessageW(reward_filter, CB_GETCURSEL, 0, 0);
    auto filter = selected_filter >= 0 && selected_filter <= 3 ? static_cast<vst::client::RewardFilter>(selected_filter) : vst::client::RewardFilter::All;
    auto filtered = vst::client::FilterRewardItems(reward_catalog, query, filter);
    reward_filtering = true;
    SendMessageW(reward_list, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemCountEx(reward_list, 0, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    reward_filtered = std::move(filtered);
    ListView_SetItemCountEx(reward_list, static_cast<int>(reward_filtered.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    for (size_t row = 0; row < reward_filtered.size(); ++row) if (reward_catalog[reward_filtered[row]].id == reward_selected_id) {
        ListView_SetItemState(reward_list, static_cast<int>(row), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); break;
    }
    SendMessageW(reward_list, WM_SETREDRAW, TRUE, 0); InvalidateRect(reward_list, nullptr, FALSE);
    reward_filtering = false;
    reward_last_scroll = GetTickCount64();
}
void InitializeRewardImages() {
    const int size = Scale(24);
    if (reward_images || size < 1 || size > 128) return;
    reward_images = ImageList_Create(size, size, ILC_COLOR32, 32, 32);
    if (reward_images) ListView_SetImageList(reward_list, reward_images, LVSIL_SMALL);
}
void ResetRewardImages() {
    if (reward_images) ImageList_RemoveAll(reward_images);
    reward_image_indices.clear();
    reward_icon_waiting = {};
}
void ClearRewardImages() {
    if (reward_images) {
        if (IsWindow(reward_list)) ListView_SetImageList(reward_list, nullptr, LVSIL_SMALL);
        ImageList_Destroy(reward_images);
        reward_images = nullptr;
    }
    reward_image_indices.clear();
}

int AddRewardImage(const vst::client::RewardIcon& icon) {
    const int size = Scale(24);
    if (size < 1 || size > 128 || icon.rgba.empty()) return I_IMAGENONE;
    if (!reward_images) return I_IMAGENONE;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) { if (bitmap) DeleteObject(bitmap); return I_IMAGENONE; }
    std::memset(pixels, 0, static_cast<size_t>(size * size * 4));
    const int longest = std::max(icon.width, icon.height);
    const int w = std::max(1, icon.width * size / longest), h = std::max(1, icon.height * size / longest);
    const int left = (size - w) / 2, top = (size - h) / 2;
    auto dest = static_cast<unsigned char*>(pixels);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto* rgba = &icon.rgba[static_cast<size_t>((y * icon.height / h * icon.width + x * icon.width / w) * 4)];
            auto* bgra = &dest[((top + y) * size + left + x) * 4];
            bgra[0] = static_cast<unsigned char>((rgba[2] * rgba[3] + 127) / 255);
            bgra[1] = static_cast<unsigned char>((rgba[1] * rgba[3] + 127) / 255);
            bgra[2] = static_cast<unsigned char>((rgba[0] * rgba[3] + 127) / 255);
            bgra[3] = rgba[3];
        }
    }
    int index = ImageList_Add(reward_images, bitmap, nullptr);
    DeleteObject(bitmap);
    return index < 0 ? I_IMAGENONE : index;
}

void ReceiveRewardIcon(RewardIconResult result) {
    if (result.generation != reward_catalog_generation || result.requested_id != reward_icon_pending_id) return;
    reward_icon_pending_id.clear();
    if (!result.ok || (result.icon.id == result.requested_id && result.icon.pending)) return;
    if (result.icon.id != result.requested_id) result.icon = {};
    result.icon.id = std::move(result.requested_id);
    reward_icon_waiting = std::move(result.icon);
}

void WorkRewardIcons(uint64_t now) {
    if (closing || !rewards_page || IsIconic(window) || GetCapture() == reward_list
        || (reward_last_scroll && (now < reward_last_scroll || now - reward_last_scroll < kRewardScrollQuietMs))) return;
    const int first = std::max(0, ListView_GetTopIndex(reward_list));
    const int count = static_cast<int>(reward_filtered.size());
    const int last = std::min(first + ListView_GetCountPerPage(reward_list) + 1, count);
    if (!reward_icon_waiting.id.empty()) {
        const int image = AddRewardImage(reward_icon_waiting);
        reward_image_indices.emplace(reward_icon_waiting.id, image);
        if (image != I_IMAGENONE) for (int row = first; row < last; ++row) {
            if (reward_catalog[reward_filtered[row]].id == reward_icon_waiting.id) {
                ListView_RedrawItems(reward_list, row, row);
                break;
            }
        }
        reward_icon_waiting = {};
    }
    if (diagnostics_mode || !reward_images || !reward_icons_supported || reward_catalog_loading || maintenance_busy
        || !reward_icon_pending_id.empty() || !FreshStatus(displayed) || worker.ControlPending()
        || reward_image_indices.size() >= 512) return;
    auto request = [&](const vst::client::RewardItem& item) {
        if (reward_image_indices.contains(item.id)) return false;
        Command command{Action::RewardIcon};
        command.reward_request.item_id = item.id;
        command.catalog_generation = reward_catalog_generation;
        if (worker.Push(std::move(command))) reward_icon_pending_id = item.id;
        return true;
    };
    const int selected = ListView_GetNextItem(reward_list, -1, LVNI_SELECTED);
    if (selected >= first && selected < last && request(reward_catalog[reward_filtered[selected]])) return;
    for (int row = first; row < std::min(last + 3, count); ++row) {
        if (request(reward_catalog[reward_filtered[row]])) break;
    }
}

void ResetRewardCatalog() {
    ++reward_catalog_generation;
    reward_icons_supported = false;
    reward_icon_pending_id.clear();
    ResetRewardImages();
    KillTimer(window, kRewardCatalogTimer); reward_catalog_request_pending = false;
    reward_catalog_loading = reward_catalog_requested = false; reward_catalog_total = reward_catalog_next = 0;
    reward_catalog.clear(); reward_catalog_error.clear(); reward_operation_error.clear(); reward_error_scope.clear();
    reward_pending_sequence = 0;
    if (reward_list) FilterRewards();
}
bool QueueRewardPage(size_t offset) {
    if (reward_catalog_request_pending) return false;
    Command command{Action::RewardCatalog}; command.reward_request.catalog_offset = offset; command.catalog_generation = reward_catalog_generation;
    reward_catalog_request_pending = worker.Push(std::move(command));
    return reward_catalog_request_pending;
}
void BeginRewardCatalog() {
    if (closing || diagnostics_mode || reward_catalog_loading || reward_pending_sequence || !displayed.connected || !(displayed.reward.supported || displayed.reward.upgrade_supported)) return;
    ++reward_catalog_generation; reward_catalog_requested = true; reward_catalog_loading = true;
    reward_icon_pending_id.clear();
    reward_icon_waiting = {};
    KillTimer(window, kRewardCatalogTimer); reward_catalog_request_pending = false;
    reward_catalog_next = 0; reward_catalog_error.clear();
    if (!QueueRewardPage(0)) { reward_catalog_loading = false; reward_catalog_error = "The item request queue is busy. Try refreshing."; }
    Refresh();
}
void QueueInitialRewardCatalog() {
    if (rewards_page && !IsIconic(window) && !reward_catalog_requested && !reward_catalog_loading && displayed.connected && (displayed.reward.supported || displayed.reward.upgrade_supported)) BeginRewardCatalog();
}
void ApplyRewardCatalogResult(const Status& status) {
    if (!status.catalog_generation || status.catalog_generation != reward_catalog_generation || !reward_catalog_loading) return;
    reward_catalog_request_pending = false;
    if (!status.command_ok || !status.connected || !status.catalog_page || !status.catalog_page->valid) {
        reward_catalog_loading = false;
        reward_catalog_error = !status.error.empty() ? status.error : status.catalog_page && !status.catalog_page->error.empty()
            ? status.catalog_page->error : "The game did not return an item catalog page.";
        return;
    }
    const auto& page = *status.catalog_page;
    auto application = vst::client::ApplyRewardCatalogPage(reward_catalog, reward_catalog_next, status.catalog_offset, page);
    if (application.disposition == vst::client::RewardPageDisposition::Invalid) {
        reward_catalog_loading = false; reward_catalog_error = std::move(application.error); return;
    }
    if (application.disposition == vst::client::RewardPageDisposition::Loading) {
        SetTimer(window, kRewardCatalogTimer, 200, nullptr); return;
    }
    if (!status.catalog_offset) ResetRewardImages();
    reward_icons_supported = page.icons_supported;
    reward_catalog_total = page.total; reward_catalog_next = application.next_offset;
    FilterRewards();
    if (application.complete) {
        reward_catalog_loading = false;
        if (page.total > vst::client::kRewardCatalogLimit) reward_catalog_error = "The item limit was reached; only the first 4096 entries are loaded.";
    } else if (!QueueRewardPage(reward_catalog_next)) {
        reward_catalog_loading = false; reward_catalog_error = "The item request queue is busy. Try refreshing.";
    }
}
void SubmitReward(int chest_enabled, int upgrade_enabled) {
    bool stopping = chest_enabled == 0 && upgrade_enabled == 0;
    if (closing || diagnostics_mode || !displayed.connected || (!stopping && (reward_pending_sequence || !FreshStatus(displayed) || !displayed.player_ready || reward_catalog_loading))) return;
    const auto* selected = SelectedRewardItem();
    if (!stopping && (!selected || (chest_enabled == 1 && !displayed.reward.supported) || (upgrade_enabled == 1 && !displayed.reward.upgrade_supported))) return;
    if (!stopping && (upgrade_enabled == 1 || (upgrade_enabled < 0 && displayed.reward.upgrade_enabled)) && selected->upgrade_known && !selected->upgrade_allowed) return;
    if (!stopping && chest_enabled < 0 && upgrade_enabled < 0 && !(displayed.reward.enabled || displayed.reward.upgrade_enabled)) return;
    Command command{Action::SetReward};
    command.reward_request.enabled = chest_enabled; command.reward_request.upgrade_enabled = upgrade_enabled;
    if (!stopping) command.reward_request.item_id = selected->id;
    reward_operation_error.clear(); reward_error_scope.clear();
    if (!worker.Push(std::move(command), &reward_pending_sequence)) reward_operation_error = "The reward request queue is busy. Try again.";
    SuspendHotkeys(); Refresh();
}
void UpdateRewardLanguage() {
    if (reward_language_initialized && reward_language_english == english) return;
    reward_language_initialized = true; reward_language_english = english;
    LRESULT selected = SendMessageW(reward_filter, CB_GETCURSEL, 0, 0); if (selected < 0 || selected > 3) selected = 0;
    SendMessageW(reward_filter, CB_RESETCONTENT, 0, 0);
    for (const auto& label : {Text(L"全部物品", L"All items"), Text(L"武器", L"Weapons"), Text(L"被动装备", L"Passives"), Text(L"进化装备", L"Evolutions")}) SendMessageW(reward_filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    SendMessageW(reward_filter, CB_SETCURSEL, selected, 0);
    SendMessageW(reward_search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Text(L"搜索中文、英文或 ID", L"Search name or ID").c_str()));
    std::array<std::wstring, 4> titles{Text(L"名称", L"Name"), Text(L"英文名称", L"Other name"), Text(L"类型", L"Type"), L"ID"};
    for (size_t index = 0; index < titles.size(); ++index) { LVCOLUMNW column{}; column.mask = LVCF_TEXT; column.pszText = titles[index].data(); SendMessageW(reward_list, LVM_SETCOLUMNW, index, reinterpret_cast<LPARAM>(&column)); }
    if (!reward_filtered.empty()) ListView_RedrawItems(reward_list, 0, static_cast<int>(reward_filtered.size() - 1));
}
void RefreshRewards() {
    UpdateRewardLanguage();
    const auto& state = displayed.reward;
    const auto* selected = SelectedRewardItem();
    bool active = state.enabled || state.upgrade_enabled;
    bool ready = !closing && !diagnostics_mode && !reward_pending_sequence && !maintenance_busy && !worker.ControlPending() && FreshStatus(displayed);
    bool can_choose = ready && displayed.player_ready && !reward_catalog_loading && selected;
    bool upgrade_allowed = selected && (!selected->upgrade_known || selected->upgrade_allowed);
    SetText(reward_refresh, Text(L"刷新列表", L"Refresh list"));
    SetText(reward_apply, Text(L"应用目标", L"Apply target")); SetText(reward_stop, Text(L"停止指定", L"Stop targeting"));
    SetText(reward_chest, Text(L"仅宝箱", L"Chest only") + (state.enabled && !state.upgrade_enabled ? L" ✓" : L""));
    SetText(reward_upgrade, Text(L"仅升级", L"Upgrade only") + (!state.enabled && state.upgrade_enabled ? L" ✓" : L""));
    SetText(reward_both, Text(L"两者", L"Both") + (state.enabled && state.upgrade_enabled ? L" ✓" : L""));
    SetEnabled(reward_refresh, ready && !reward_catalog_loading && (state.supported || state.upgrade_supported));
    SetEnabled(reward_search, !closing); SetEnabled(reward_filter, !closing); SetEnabled(reward_list, !closing);
    SetEnabled(reward_apply, can_choose && active && selected->id != state.target_id && (!state.upgrade_enabled || upgrade_allowed));
    SetEnabled(reward_chest, can_choose && state.supported); SetEnabled(reward_upgrade, can_choose && state.upgrade_supported && upgrade_allowed);
    SetEnabled(reward_both, can_choose && state.supported && state.upgrade_supported && upgrade_allowed);
    SetEnabled(reward_stop, !closing && !diagnostics_mode && displayed.connected);
    std::wstring summary = reward_catalog_loading ? Text(L"正在读取物品列表… ", L"Loading items… ") + std::to_wstring(reward_catalog.size())
        : !reward_catalog_error.empty() ? Message(reward_catalog_error)
        : !displayed.connected ? Text(L"连接游戏后读取物品列表", L"Connect to read the game's item list")
        : Text(L"显示 ", L"Showing ") + std::to_wstring(reward_filtered.size()) + L" / " + std::to_wstring(reward_catalog.size()) + Text(L" 项 · 来源：当前游戏", L" items · Source: current game");
    if (diagnostics_mode) summary = Text(L"示例物品 · 离线界面预览", L"Sample items · Offline layout preview");
    SetText(reward_summary, summary);
    SetText(reward_selection, selected ? Text(L"当前选择：", L"Selected: ") + RewardDisplayName(*selected) + L"\r\n" + selected->wide_id
        + Text(L" · 最高等级 ", L" · Max level ") + selected->level + (upgrade_allowed ? Text(L" · 宝箱 / 升级", L" · Chest / upgrade") : Text(L" · 仅宝箱", L" · Chest only"))
        : Text(L"当前选择：请从列表选择物品", L"Selected: choose an item from the list"));
    SetText(reward_active, active ? Text(L"当前生效目标：", L"Active target: ") + RewardName(state.target_id, state.target_name) + L"\r\n" + vst::Wide(state.target_id)
        : Text(L"当前生效目标：无", L"Active target: none"));
    std::wstring operation = Text(L"宝箱：", L"Chest: ") + (state.enabled ? Text(L"已开启", L"On") : !state.supported ? Text(L"暂不可用", L"Unavailable") : Text(L"已关闭", L"Off"))
        + Text(L"    升级：", L"    Upgrade: ") + (state.upgrade_enabled ? Text(L"已开启", L"On") : !state.upgrade_supported ? Text(L"暂不可用", L"Unavailable") : Text(L"已关闭", L"Off"));
    if (reward_pending_sequence) operation += Text(L" · 正在提交，等待游戏确认", L" · Waiting for confirmation");
    if (!reward_operation_error.empty()) operation += L"\r\n" + (reward_error_scope == "chest" ? Text(L"宝箱指定：", L"Chest targeting: ")
        : reward_error_scope == "upgrade" ? Text(L"升级指定：", L"Upgrade targeting: ")
        : reward_error_scope == "rewardTarget" ? Text(L"共享目标：", L"Shared target: ")
        : reward_error_scope == "catalog" ? Text(L"物品列表：", L"Item list: ") : Text(L"指定奖励：", L"Reward request: ")) + Message(reward_operation_error);
    else if (selected && !upgrade_allowed) operation += L"\r\n" + Text(L"该目标仅支持宝箱指定。", L"This item supports chest targeting only.");
    else if (!displayed.player_ready) operation += L"\r\n" + Text(L"进入普通单人关卡后可开启。", L"Enter a supported single-player run to enable targeting.");
    SetText(reward_operation, operation);
    SetText(reward_chest_record, Text(L"宝箱实际取得：", L"Chest acquired: ") + std::to_wstring(state.processed) + L"\r\n" + RewardName(state.last_reward_id, state.last_reward_name));
    SetText(reward_upgrade_record, Text(L"升级选项 / 实际取得：", L"Upgrade offers / acquired: ") + std::to_wstring(state.offered) + L" / " + std::to_wstring(state.granted)
        + L"\r\n" + RewardName(state.last_offer_id, state.last_offer_name) + L" / " + RewardName(state.last_granted_id, state.last_granted_name));
}

std::wstring StatNumber(double value, int decimals) {
    wchar_t text[64]{}; std::swprintf(text, std::size(text), L"%.*f", decimals, value);
    std::wstring result(text);
    if (result.find(L'.') != std::wstring::npos) {
        while (!result.empty() && result.back() == L'0') result.pop_back();
        if (!result.empty() && result.back() == L'.') result.pop_back();
    }
    return result;
}
std::wstring StatReading(const vst::NativeStatDefinition& definition, bool known, double value) {
    return known ? StatNumber(value, definition.decimals) + (definition.unit[0] ? L" " + std::wstring(definition.unit) : L"") : L"—";
}

void RefreshCommonPage() {
    SetText(armor_label, Text(L"护甲覆盖", L"Armor override"));
    SetText(values_label, Text(L"正常护甲：", L"Normal armor: ") + (displayed.original_known ? Number(displayed.original) : L"—")
        + Text(L"     最近实际返回：", L"     Latest actual return: ") + (displayed.current_known ? Number(displayed.current) : Text(L"等待读取", L"Waiting for a read")));
    SetText(count_label, Text(L"游戏实际读取次数：", L"Actual game reads: ") + std::to_wstring(displayed.reads));
    constexpr const wchar_t* names_zh[]{L"生命值不减", L"经验倍率", L"伤害倍率", L"移动速度倍率", L"快速冷却 · 基础武器", L"一击必杀", L"金币免消耗", L"游戏加速 · 1–10×", L"跳过宝箱动画"};
    constexpr const wchar_t* names_en[]{L"No health loss", L"Experience multiplier", L"Damage multiplier", L"Movement multiplier", L"Fast cooldown · Basic weapons", L"One-hit kill", L"No gold spending", L"Game speed · 1–10×", L"Skip chest animation"};
    for (size_t f = 0; f < kFeatures.size(); ++f) {
        SetText(feature_labels[f], Text(names_zh[f], names_en[f]));
        SetText(feature_buttons[f], displayed.features[f].enabled ? Text(L"关闭", L"Turn off") : Text(L"开启", L"Turn on"));
        SetText(feature_counts[f], displayed.connected && !displayed.features[f].supported
            ? Text(L"暂不可用", L"Unavailable")
            : f == kGameSpeedFeature && displayed.features[f].enabled && displayed.paused_known && displayed.paused
                ? Text(L"暂停中", L"Paused") : (f == kChestAnimationFeature ? Text(L"跳过次数：", L"Skipped: ") : Text(L"修改次数：", L"Modified: "))
                    + std::to_wstring(displayed.features[f].overrides));
        SetEnabled(feature_buttons[f], !closing && !diagnostics_mode && FeatureCanToggle(displayed, f));
        SetEnabled(feature_edits[f], FeatureLiveTarget(f) != kNoLiveTarget && !closing && !diagnostics_mode);
    }
}
void RefreshStatsPage() {
    for (size_t index = 0; index < vst::kNativeStatCount; ++index) {
        const auto& definition = vst::kNativeStats[index];
        const auto& state = displayed.stats[index];
        SetText(stat_labels[index], Text(definition.name_zh, definition.name_en) + L"\r\n"
            + StatNumber(definition.minimum, definition.decimals) + L"–" + StatNumber(definition.maximum, definition.decimals)
            + (definition.unit[0] ? L" " + std::wstring(definition.unit) : L""));
        SetText(stat_buttons[index], state.enabled ? Text(L"关闭", L"Turn off") : Text(L"开启", L"Turn on"));
        std::wstring readings = Text(L"正常：", L"Normal: ") + StatReading(definition, state.original_known, state.original)
            + Text(L"\r\n最近返回：", L"\r\nLatest: ") + StatReading(definition, state.current_known, state.current);
        if (state.bound_known) readings = Text(L"本局采样：", L"Run sample: ") + StatReading(definition, true, state.bound)
            + Text(L"\r\n最近返回：", L"\r\nLatest: ") + StatReading(definition, state.current_known, state.current);
        if (displayed.connected && !state.supported) readings = Text(L"当前角色或关卡暂不可用", L"Unavailable for the current character or run");
        if (definition.stat == vst::NativeStat::Omni) readings = Text(L"力量 · 弹速 · 持续时间 · 范围\r\n单项设置优先", L"Might / speed / duration / area\r\nIndividual settings take priority");
        SetText(stat_values[index], readings);
        SetText(stat_counts[index], Text(L"实际读取 / 修改：", L"Reads / modified: ") + std::to_wstring(state.reads) + L" / " + std::to_wstring(state.overrides)
            + (state.source == "omni" ? Text(L"  · 万能", L"  · Unified") : L""));
        SetEnabled(stat_edits[index], !closing && !diagnostics_mode);
        SetEnabled(stat_buttons[index], !closing && !diagnostics_mode && displayed.connected && state.supported && (displayed.player_ready || state.enabled));
    }
}
void RefreshHotkeysPage() {
    for (size_t action = 0; action < vst::client::kHotkeyCount; ++action) {
        SetText(hotkey_labels[action], HotkeyActionName(action));
        SetText(hotkey_buttons[action], capturing_hotkey == action ? Text(L"请按新键…", L"Press a key…") : vst::client::HotkeyLabel(client_settings.hotkeys[action], english));
        DWORD error = hotkey_manager.Error(action);
        std::wstring note = capturing_hotkey == action ? Text(L"Esc 取消 · Delete 清除", L"Esc cancels · Delete clears")
            : error == ERROR_HOTKEY_ALREADY_REGISTERED ? Text(L"快捷键已被占用", L"Hotkey already in use")
            : error ? Text(L"注册失败：", L"Registration failed: ") + std::to_wstring(error)
            : client_settings.hotkeys[action].key ? Text(L"仅游戏前台生效", L"Game foreground only") : Text(L"点击设定", L"Click to bind");
        SetText(hotkey_notes[action], note); SetEnabled(hotkey_buttons[action], !closing && !diagnostics_mode);
    }
}
void RefreshMaintenancePage() {
    SetText(game_directory_label, Text(L"游戏目录", L"Game folder"));
    SetText(browse_game_button, Text(L"浏览…", L"Browse…"));
    SetText(create_backup_button, Text(L"创建备份", L"Create backup"));
    SetText(list_backups_button, Text(L"刷新列表", L"Refresh list"));
    SetText(restore_backup_button, Text(L"恢复所选备份", L"Restore selected"));
    SetText(maintenance_label, maintenance_busy ? Text(L"正在处理…", L"Working…") : maintenance_notice.empty()
        ? Text(L"选择备份后可恢复；原备份和恢复前快照都会保留。", L"Select a backup to restore. The original backup and a safety snapshot are kept.") : Message(maintenance_notice));
    for (HWND control : {game_directory_edit, browse_game_button, create_backup_button, list_backups_button, backup_list}) SetEnabled(control, !closing && !diagnostics_mode && !maintenance_busy);
    SetEnabled(restore_backup_button, !closing && !diagnostics_mode && !maintenance_busy && !backup_entries.empty());
}
bool CanSaveGold() {
    return !closing && !diagnostics_mode && !maintenance_busy && !gold_submission.pending && FreshStatus(displayed)
        && displayed.gold_write_ready && !worker.ControlPending();
}
void RefreshGoldPage() {
    SetText(gold_label, Text(L"当前局外金币", L"Current menu gold"));
    SetText(gold_balance_label, displayed.connected && displayed.gold_known ? StatNumber(displayed.gold, 2) : L"—");
    std::wstring readiness;
    if (!displayed.connected) readiness = Text(L"连接游戏后读取余额。", L"Connect to the game to read the balance.");
    else if (displayed.gold_write_ready) readiness = Text(L"普通模式主菜单 · 备份已核验", L"Normal mode main menu · Backup verified");
    else if (displayed.player_ready || (!displayed.scene.empty() && displayed.scene != "MainMenu"))
        readiness = Text(L"请回到普通模式的单人主菜单。", L"Return to the single-player main menu in normal mode.");
    else if (!displayed.backup_authorized)
        readiness = Text(L"请先退出游戏和 Steam，在「维护与备份」创建备份，再启动游戏。", L"Exit the game and Steam, create a backup in Maintenance, then launch the game.");
    else readiness = Text(L"等待普通模式钱包就绪。", L"Waiting for the normal-mode wallet to become available.");
    SetText(gold_readiness_label, readiness);
    SetText(gold_target_label, Text(L"目标金币数", L"Target gold balance"));
    SetText(set_gold_button, Text(L"设置并保存", L"Set and save"));
    SetText(gold_backup_button, Text(L"前往维护与备份", L"Open Maintenance"));
    std::wstring feedback;
    using Result = GoldSubmission::Result;
    if (gold_submission.pending) feedback = Text(L"正在等待游戏保存…", L"Waiting for the game to save…");
    else if (gold_submission.result == Result::Saved) feedback = Text(L"已保存，金币为 ", L"Saved. Gold balance set to ") + StatNumber(gold_submission.value, 0);
    else if (gold_submission.result == Result::Cancelled) feedback = Text(L"提交已取消。", L"The request was cancelled.");
    else if (gold_submission.result == Result::Unconfirmed) feedback = Text(L"未收到完整保存确认，请先核对当前余额。", L"Save confirmation was incomplete. Check the current balance first.");
    else if (gold_submission.result == Result::Failed) feedback = gold_submission.error.empty()
        ? Text(L"游戏未能保存金币。", L"The game could not save the balance.") : Message(gold_submission.error);
    else feedback = Text(L"输入 0–999999999 范围内游戏可精确保存的整数。", L"Enter a whole number from 0 to 999,999,999 that the game can save exactly.");
    SetText(gold_feedback_label, feedback);
    SetEnabled(gold_value_edit, !closing && !diagnostics_mode && !gold_submission.pending);
    SetEnabled(set_gold_button, CanSaveGold() && GetWindowTextLengthW(gold_value_edit) > 0);
    SetEnabled(gold_backup_button, !closing && !diagnostics_mode);
}
void Refresh() {
    SetText(heading, Text(L"吸血鬼幸存者修改器", L"Vampire Survivors Trainer"));
    SetText(description, Text(L"作者：Beason9527  ·  9 项常用功能 / 23 项本局属性  ·  ", L"Beason9527  ·  9 features / 23 run stats  ·  ") + vst::Wide(vst::version::product));
    SetText(connect_button, Text(L"自动连接", L"Auto-connect"));
    SetText(manual_connect_button, Text(L"手动连接…", L"Manual connect…"));
    SetText(toggle_button, displayed.enabled ? Text(L"关闭", L"Turn off") : Text(L"开启", L"Turn on"));
    SetText(disable_button, Text(L"关闭全部", L"Disable all"));
    SetText(about_button, Text(L"说明与许可", L"About & licenses"));
    SetText(common_page_button, Text(L"常用功能", L"Common features"));
    SetText(stats_page_button, Text(L"本局属性", L"Run stats"));
    SetText(hotkeys_page_button, Text(L"快捷键", L"Hotkeys"));
    SetText(maintenance_page_button, Text(L"维护与备份", L"Maintenance"));
    SetText(rewards_page_button, Text(L"指定奖励", L"Rewards"));
    SetText(gold_page_button, Text(L"局外金币", L"Menu gold"));
    const bool common_page = CommonPage();
    SetEnabled(common_page_button, !common_page);
    SetEnabled(stats_page_button, !stats_page);
    SetEnabled(hotkeys_page_button, !hotkeys_page);
    SetEnabled(maintenance_page_button, !maintenance_page);
    SetEnabled(rewards_page_button, !rewards_page);
    SetEnabled(gold_page_button, !gold_page);
    std::wstring status;
    const bool connecting = connection_sequence || displayed.connecting_automatically;
    const bool automatic = worker.AutomaticEnabled();
    if (connecting) status = Text(L"正在核对游戏版本并连接…", L"Verifying and connecting to the game…");
    else {
        if (displayed.connected) {
            status = displayed.player_ready ? Text(L"已连接 · 关卡中", L"Connected · In a run") : Text(L"已连接 · 等待关卡", L"Connected · Waiting for a run");
            if (!displayed.scene.empty()) status += L"  ·  " + vst::Wide(displayed.scene);
        } else if (!automatic) status = Text(L"手动连接", L"Manual connection");
        else if (displayed.game_scan.pid) status = Text(L"已检测到游戏", L"Game detected");
        else if (!displayed.game_scan.error.empty() || diagnostics_mode) status = Text(L"等待游戏", L"Waiting for the game");
        else status = Text(L"正在检测游戏…", L"Looking for the game…");

        if (!displayed.error.empty()) status += L"\r\n" + Message(displayed.error);
        else if (!capture_notice.empty()) status += L"\r\n" + capture_notice;
        else if (!settings_notice.empty()) status += L"\r\n" + Message(settings_notice);
        else if (!displayed.note.empty()) status += L"\r\n" + Message(displayed.note);
        else if (!displayed.connected) {
            if (!automatic) status += Text(L"\r\n选择游戏目录，或恢复自动连接。", L"\r\nChoose the game folder or resume auto-connect.");
            else if (displayed.game_scan.pid) status += Text(L"\r\n正在准备自动连接。", L"\r\nPreparing to connect automatically.");
            else if (displayed.game_scan.error.empty() || displayed.game_scan.error == vst::client::kNoRunningGame)
                status += Text(L"\r\n从 Steam 启动游戏，修改器会自动连接。", L"\r\nStart the game in Steam to connect automatically.");
            else if (displayed.game_scan.error == vst::client::kMultipleGames) status += L"\r\n" + Message(displayed.game_scan.error);
            else status += Text(L"\r\n无法自动检测。可以手动选择游戏目录。", L"\r\nAutomatic detection is unavailable. Choose the game folder manually.");
        }
    }
    SetText(status_label, status);
    const COLORREF indicator = connecting ? ui::gold : !displayed.error.empty() ? ui::warning
        : displayed.connected && displayed.player_ready ? ui::positive : displayed.connected || displayed.game_scan.pid ? ui::gold : ui::muted;
    if (connection_indicator != indicator) {
        connection_indicator = indicator;
        const RECT card = InterfaceBounds(window).StatusCard();
        InvalidateRect(window, &card, TRUE);
    }
    if (common_page) RefreshCommonPage();
    else if (stats_page) RefreshStatsPage();
    else if (hotkeys_page) RefreshHotkeysPage();
    else if (maintenance_page) RefreshMaintenancePage();
    else if (gold_page) RefreshGoldPage();
    else RefreshRewards();
    SetText(guide_label, rewards_page ? Text(L"共用目标，选择后点击按钮；升级生成选项，仍需手动选择。", L"Shared target; select an item, then apply. Upgrade offers need a manual choice.")
        : hotkeys_page ? Text(L"点击绑定后按新键；Delete 清除，Esc 取消。仅游戏前台生效。", L"Click a binding, then press a key. Delete clears; Esc cancels. Game foreground only.")
        : maintenance_page ? Text(L"备份或恢复前先退出游戏和 Steam；恢复前会自动创建保护快照。", L"Exit the game and Steam before backup or restore. Restore first creates a safety snapshot.")
        : gold_page ? Text(L"成功后，余额会写入普通模式的游戏存档。", L"A successful change is saved to your normal-mode progress.") : stats_page
        ? Text(L"开启时改值自动应用；万能覆盖力量、弹速、持续时间和范围，单项优先。", L"Enabled values update automatically. Individual settings override unified bonuses.")
        : Text(L"开启后改数值会自动应用。提示旧后端时，请重启游戏。", L"Enabled values update automatically. Restart the game if an older backend is detected."));
    const bool can_connect = !closing && !diagnostics_mode && !displayed.connected && !connecting;
    SetEnabled(connect_button, can_connect);
    SetEnabled(manual_connect_button, can_connect);
    SetEnabled(toggle_button, !closing && !diagnostics_mode && displayed.connected && displayed.supported && (displayed.player_ready || displayed.enabled));
    SetEnabled(disable_button, !closing && !diagnostics_mode && displayed.connected);
}
void ShowPageControls() {
    const bool common_page = CommonPage();
    for (size_t f = 0; f < kFeatures.size(); ++f) for (HWND control : {feature_labels[f], feature_edits[f], feature_buttons[f], feature_counts[f]})
        if (control) ShowWindow(control, common_page ? SW_SHOWNA : SW_HIDE);
    for (HWND control : {armor_label, number_edit, toggle_button, values_label, count_label}) ShowWindow(control, common_page ? SW_SHOWNA : SW_HIDE);
    ShowWindow(stat_pane, stats_page ? SW_SHOWNA : SW_HIDE);
    ShowWindow(hotkeys_pane, hotkeys_page ? SW_SHOWNA : SW_HIDE);
    for (HWND control : {game_directory_label, game_directory_edit, browse_game_button, create_backup_button, list_backups_button, restore_backup_button, backup_list, maintenance_label})
        ShowWindow(control, maintenance_page ? SW_SHOWNA : SW_HIDE);
    for (HWND control : {gold_label, gold_balance_label, gold_readiness_label, gold_target_label, gold_value_edit, set_gold_button, gold_backup_button, gold_feedback_label})
        ShowWindow(control, gold_page ? SW_SHOWNA : SW_HIDE);
    for (HWND control : {reward_search, reward_filter, reward_refresh, reward_list, reward_summary, reward_selection, reward_active, reward_operation, reward_chest_record, reward_upgrade_record,
            reward_apply, reward_chest, reward_upgrade, reward_both, reward_stop}) ShowWindow(control, rewards_page ? SW_SHOWNA : SW_HIDE);
}
void LayoutStats() {
    RECT rectangle{}; GetClientRect(stat_pane, &rectangle);
    int row_height = Scale(78), content_height = row_height * static_cast<int>(vst::kNativeStatCount);
    stat_scroll = std::clamp(stat_scroll, 0, std::max(0, content_height - static_cast<int>(rectangle.bottom)));
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
    scroll.nMin = 0; scroll.nMax = content_height - 1; scroll.nPage = static_cast<UINT>(rectangle.bottom); scroll.nPos = stat_scroll;
    SetScrollInfo(stat_pane, SB_VERT, &scroll, TRUE);
    int width = rectangle.right;
    HDWP batch = BeginDeferWindowPos(static_cast<int>(vst::kNativeStatCount * 5));
    auto place = [&](HWND control, int x, int y, int w, int h) { if (batch) batch = DeferWindowPos(batch, control, nullptr, Scale(x), y, w, Scale(h), SWP_NOZORDER | SWP_NOACTIVATE); };
    for (size_t index = 0; index < vst::kNativeStatCount; ++index) {
        int y = static_cast<int>(index) * row_height - stat_scroll;
        place(stat_labels[index], 12, y + Scale(9), Scale(214), 56);
        place(stat_edits[index], 235, y + Scale(20), Scale(95), 32);
        place(stat_buttons[index], 342, y + Scale(19), Scale(95), 34);
        place(stat_values[index], 453, y + Scale(3), std::max(Scale(160), width - Scale(461)), 44);
        place(stat_counts[index], 453, y + Scale(49), std::max(Scale(160), width - Scale(461)), 26);
    }
    if (batch) EndDeferWindowPos(batch);
    InvalidateRect(stat_pane, nullptr, TRUE);
}
void LayoutHotkeys() {
    RECT rectangle{}; GetClientRect(hotkeys_pane, &rectangle);
    int row_height = Scale(60), content_height = row_height * static_cast<int>(vst::client::kHotkeyCount);
    hotkey_scroll = std::clamp(hotkey_scroll, 0, std::max(0, content_height - static_cast<int>(rectangle.bottom)));
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
    scroll.nMax = content_height - 1; scroll.nPage = static_cast<UINT>(rectangle.bottom); scroll.nPos = hotkey_scroll;
    SetScrollInfo(hotkeys_pane, SB_VERT, &scroll, TRUE);
    HDWP batch = BeginDeferWindowPos(static_cast<int>(vst::client::kHotkeyCount * 3));
    for (size_t action = 0; action < vst::client::kHotkeyCount; ++action) {
        int y = static_cast<int>(action) * row_height - hotkey_scroll;
        if (batch) batch = DeferWindowPos(batch, hotkey_labels[action], nullptr, Scale(12), y + Scale(17), Scale(260), Scale(30), SWP_NOZORDER | SWP_NOACTIVATE);
        if (batch) batch = DeferWindowPos(batch, hotkey_buttons[action], nullptr, Scale(285), y + Scale(12), Scale(220), Scale(36), SWP_NOZORDER | SWP_NOACTIVATE);
        if (batch) batch = DeferWindowPos(batch, hotkey_notes[action], nullptr, Scale(520), y + Scale(9), std::max(Scale(150), static_cast<int>(rectangle.right) - Scale(528)), Scale(46), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (batch) EndDeferWindowPos(batch);
    InvalidateRect(hotkeys_pane, nullptr, TRUE);
}
void Layout() {
    if (!heading || !reward_list) return;
    const InterfaceBounds bounds(window);
    const int width = bounds.client.right, height = bounds.client.bottom;
    const int left = bounds.left, content = bounds.content;
    const bool common_page = CommonPage();
    HDWP batch = BeginDeferWindowPos(64);
    auto place = [&](HWND control, int x, int y, int w, int h) {
        if (batch && control) batch = DeferWindowPos(batch, control, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    auto at = [&](HWND control, int x, int y, int w, int h) { place(control, x, Scale(y), w, Scale(h)); };
    at(heading, left, 16, content - Scale(190), 40);
    at(description, left, 62, content, 22);
    at(chinese_button, width - left - Scale(171), 24, Scale(91), 32);
    at(english_button, width - left - Scale(73), 24, Scale(73), 32);
    at(status_label, left + Scale(36), 114, content - Scale(49), 58);
    at(connect_button, left, 184, Scale(174), 32);
    at(manual_connect_button, left + Scale(182), 184, Scale(148), 32);
    at(disable_button, left + Scale(338), 184, Scale(120), 32);
    at(about_button, width - left - Scale(140), 184, Scale(140), 32);
    const std::array tabs{common_page_button, stats_page_button, rewards_page_button, hotkeys_page_button, gold_page_button, maintenance_page_button};
    const int tab_width = (content - Scale(8) * static_cast<int>(tabs.size() - 1)) / static_cast<int>(tabs.size());
    for (size_t i = 0; i < tabs.size(); ++i)
        at(tabs[i], left + static_cast<int>(i) * (tab_width + Scale(8)), 227, tab_width, 35);
    if (common_page) {
        for (size_t f = 0; f < kFeatures.size(); ++f) {
            const bool right = f >= 4;
            const auto card = bounds.FeatureCard(static_cast<int>(right ? f - 4 : f), right);
            const int x = card.left + Scale(12), y = card.top;
            const bool numeric = feature_edits[f] != nullptr;
            const int button_x = x + (numeric ? Scale(76) : 0);
            place(feature_labels[f], x, y + Scale(6), bounds.column - Scale(24), Scale(23));
            if (numeric) place(feature_edits[f], x, y + Scale(31), Scale(68), Scale(25));
            place(feature_buttons[f], button_x, y + Scale(30), Scale(92), Scale(26));
            const int count_x = button_x + Scale(104);
            place(feature_counts[f], count_x, y + Scale(35), card.right - count_x - Scale(8), Scale(20));
        }
        const auto armor = bounds.FeatureCard(4, false);
        place(armor_label, armor.left + Scale(12), armor.top + Scale(6), bounds.column - Scale(24), Scale(23));
        place(number_edit, armor.left + Scale(12), armor.top + Scale(31), Scale(68), Scale(25));
        place(toggle_button, armor.left + Scale(88), armor.top + Scale(30), Scale(92), Scale(26));
        const auto readings = bounds.ArmorReadings();
        place(values_label, readings.left + Scale(12), readings.top + Scale(3), content - Scale(24), Scale(21));
        place(count_label, readings.left + Scale(12), readings.top + Scale(24), content - Scale(24), Scale(20));
    } else if (stats_page || hotkeys_page) {
        place(stats_page ? stat_pane : hotkeys_pane, left, Scale(kPageTop), content,
            std::max(Scale(100), height - Scale(kPageTop + 42)));
    } else if (maintenance_page) {
        at(game_directory_label, left, kPageTop, content, 24);
        at(game_directory_edit, left, 305, content - Scale(125), 32);
        at(browse_game_button, width - left - Scale(113), 305, Scale(113), 32);
        at(create_backup_button, left, 351, Scale(155), 32);
        at(list_backups_button, left + Scale(163), 351, Scale(155), 32);
        at(restore_backup_button, left + Scale(326), 351, Scale(185), 32);
        place(backup_list, left, Scale(402), content, std::max(Scale(90), height - Scale(492)));
        place(maintenance_label, left, height - Scale(82), content, Scale(42));
    } else if (gold_page) {
        const int inner_left = left + Scale(16), inner_width = content - Scale(32);
        at(gold_label, inner_left, 290, inner_width, 24);
        at(gold_balance_label, inner_left, 316, inner_width, 42);
        at(gold_readiness_label, inner_left, 368, inner_width, 28);
        at(gold_target_label, inner_left, 430, inner_width, 24);
        at(gold_value_edit, inner_left, 460, Scale(180), 32);
        at(set_gold_button, inner_left + Scale(192), 460, Scale(150), 32);
        at(gold_backup_button, inner_left + inner_width - Scale(168), 460, Scale(168), 32);
        at(gold_feedback_label, inner_left, 512, inner_width, 48);
    } else {
        at(reward_search, left, kPageTop, content - Scale(280), 32);
        at(reward_filter, width - left - Scale(270), kPageTop, Scale(135), 150);
        at(reward_refresh, width - left - Scale(125), kPageTop, Scale(125), 32);
        at(reward_summary, left, 313, content, 22);
        const auto details = bounds.RewardDetails();
        place(reward_list, left, Scale(338), content, std::max(Scale(90), static_cast<int>(details.top) - Scale(346)));
        const int inner_left = details.left + Scale(12), inner_width = content - Scale(24);
        const int half = (inner_width - Scale(16)) / 2, right = inner_left + half + Scale(16);
        const int top = details.top;
        place(reward_selection, inner_left, top + Scale(7), half, Scale(40));
        place(reward_active, right, top + Scale(7), half, Scale(40));
        const int action_width = (inner_width - Scale(24)) / 5;
        const std::array actions{reward_apply, reward_chest, reward_upgrade, reward_both, reward_stop};
        for (size_t i = 0; i < actions.size(); ++i)
            place(actions[i], inner_left + static_cast<int>(i) * (action_width + Scale(6)), top + Scale(50), action_width, Scale(29));
        place(reward_operation, inner_left, top + Scale(85), inner_width, Scale(38));
        place(reward_chest_record, inner_left, top + Scale(125), half, Scale(40));
        place(reward_upgrade_record, right, top + Scale(125), half, Scale(40));
    }
    place(guide_label, left, height - Scale(33), content, Scale(25));
    if (batch) EndDeferWindowPos(batch);
    if (stats_page) LayoutStats();
    if (hotkeys_page) LayoutHotkeys();
    if (rewards_page) {
        int list_width = std::max(Scale(200), content - Scale(24));
        const std::array widths{list_width * 30 / 100, list_width * 30 / 100, list_width * 15 / 100, list_width - list_width * 75 / 100};
        for (size_t i = 0; i < widths.size(); ++i)
            if (ListView_GetColumnWidth(reward_list, static_cast<int>(i)) != widths[i]) ListView_SetColumnWidth(reward_list, static_cast<int>(i), widths[i]);
    }
    InvalidateRect(window, nullptr, TRUE);
}
void SetPage(int page) {
    bool show_stats = page == kStatsPage;
    HWND from = stats_page ? stat_edits[kArmorIndex] : number_edit;
    HWND to = show_stats ? stat_edits[kArmorIndex] : number_edit;
    wchar_t text[128]{}; GetWindowTextW(from, text, 128);
    syncing_edits = true; SetText(to, text); syncing_edits = false;
    stats_page = show_stats; hotkeys_page = page == kHotkeysPage; maintenance_page = page == kMaintenancePage; rewards_page = page == kRewardsPage;
    gold_page = page == kGoldPage;
    if (rewards_page && !diagnostics_mode) SetTimer(window, kRewardIconTimer, 180, nullptr);
    else KillTimer(window, kRewardIconTimer);
    CancelHotkeyCapture();
    Refresh(); ShowPageControls(); Layout(); WakeLiveValues();
    QueueInitialRewardCatalog();
}
void EnsureStatVisible(size_t index) {
    if (!stats_page || index >= vst::kNativeStatCount) return;
    RECT rectangle{}; GetClientRect(stat_pane, &rectangle);
    int top = static_cast<int>(index) * Scale(78);
    if (top < stat_scroll) stat_scroll = top;
    else if (top + Scale(78) > stat_scroll + rectangle.bottom) stat_scroll = top + Scale(78) - rectangle.bottom;
    else return;
    LayoutStats();
}
void EnsureHotkeyVisible(size_t index) {
    if (!hotkeys_page || index >= vst::client::kHotkeyCount) return;
    RECT rectangle{}; GetClientRect(hotkeys_pane, &rectangle);
    int top = static_cast<int>(index) * Scale(60);
    if (top < hotkey_scroll) hotkey_scroll = top;
    else if (top + Scale(60) > hotkey_scroll + rectangle.bottom) hotkey_scroll = top + Scale(60) - rectangle.bottom;
    else return;
    LayoutHotkeys();
}
LRESULT CALLBACK StatPaneProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND || message == WM_PRINTCLIENT) {
        PaintScrollablePane(hwnd, reinterpret_cast<HDC>(wparam));
        return message == WM_ERASEBKGND ? 1 : 0;
    }
    if (const auto color = ThemedControlColor(message, wparam, lparam, true)) return color;
    if (message == WM_NOTIFY) return DrawThemedControl(reinterpret_cast<NMHDR*>(lparam));
    if (message == WM_COMMAND) return SendMessageW(window, message, wparam, lparam);
    if (message == WM_VSCROLL || message == WM_MOUSEWHEEL) {
        SCROLLINFO scroll{sizeof(scroll), SIF_ALL}; GetScrollInfo(hwnd, SB_VERT, &scroll);
        bool keys = hwnd == hotkeys_pane;
        int next = keys ? hotkey_scroll : stat_scroll;
        if (message == WM_MOUSEWHEEL) next -= static_cast<short>(HIWORD(wparam)) * Scale(keys ? 60 : 78) / WHEEL_DELTA;
        else switch (LOWORD(wparam)) {
            case SB_LINEUP: next -= Scale(39); break;
            case SB_LINEDOWN: next += Scale(39); break;
            case SB_PAGEUP: next -= static_cast<int>(scroll.nPage); break;
            case SB_PAGEDOWN: next += static_cast<int>(scroll.nPage); break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: next = scroll.nTrackPos; break;
            case SB_TOP: next = 0; break;
            case SB_BOTTOM: next = scroll.nMax; break;
        }
        if (keys) { hotkey_scroll = next; LayoutHotkeys(); }
        else { stat_scroll = next; LayoutStats(); }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
LRESULT CALLBACK LicenseProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CREATE) {
        auto edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(edit, EM_SETLIMITTEXT, 1024 * 1024, 0);
        std::wstring text = Text(L"吸血鬼幸存者修改器 · 原生版\r\n作者 Beason9527\r\n\r\n", L"Vampire Survivors Trainer · Native edition\r\nAuthor Beason9527\r\n\r\n");
        text += vst::Wide(vst::embedded::licenses);
        SetWindowTextW(edit, text.c_str()); return 0;
    }
    if (message == WM_SIZE) { MoveWindow(GetDlgItem(hwnd, 1), 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE); return 0; }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND || message == WM_PRINTCLIENT) {
        PaintInterface(hwnd, reinterpret_cast<HDC>(wparam));
        return message == WM_ERASEBKGND ? 1 : 0;
    }
    if (const auto color = ThemedControlColor(message, wparam, lparam)) return color;
    if (message == WM_SETTINGCHANGE || message == WM_SYSCOLORCHANGE) {
        UpdateTheme();
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }
    if (message == WM_CREATE) {
        window = hwnd;
        syncing_edits = true;
        using DpiQuery = UINT(WINAPI*)(HWND);
        auto query_dpi = reinterpret_cast<DpiQuery>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        dpi = query_dpi ? query_dpi(hwnd) : 96; UpdateFonts();
        auto add = [&](const wchar_t* cls, DWORD style, int id = 0, const wchar_t* text = L"", HWND parent = nullptr) {
            const bool edit = std::wcscmp(cls, L"EDIT") == 0;
            HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style | (edit ? WS_BORDER : 0), 0, 0, 1, 1, parent ? parent : hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            if (edit) SendMessageW(child, EM_SETLIMITTEXT, 127, 0);
            return child;
        };
        heading = add(L"STATIC", SS_LEFT); SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(title_font), FALSE);
        description = add(L"STATIC", SS_LEFT); status_label = add(L"STATIC", SS_LEFT); values_label = add(L"STATIC", SS_LEFT); count_label = add(L"STATIC", SS_LEFT); guide_label = add(L"STATIC", SS_LEFT);
        armor_label = add(L"STATIC", SS_LEFT);
        constexpr DWORD language_style = BS_AUTORADIOBUTTON | BS_PUSHLIKE;
        chinese_button = add(L"BUTTON", WS_GROUP | language_style, kLanguageChinese, L"简体中文");
        english_button = add(L"BUTTON", language_style, kLanguageEnglish, L"English");
        SendMessageW(english ? english_button : chinese_button, BM_SETCHECK, BST_CHECKED, 0);
        connect_button = add(L"BUTTON", WS_GROUP | WS_TABSTOP | BS_PUSHBUTTON, kConnect);
        manual_connect_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kManualConnect);
        disable_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kDisable);
        about_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kAbout);
        common_page_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kCommonPage); stats_page_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kStatsPage);
        rewards_page_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kRewardsPage);
        hotkeys_page_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kHotkeysPage); gold_page_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kGoldPage);
        maintenance_page_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kMaintenancePage);
        for (size_t f = 0; f < kFeatures.size(); ++f) {
            feature_labels[f] = add(L"STATIC", SS_LEFT);
            const size_t target = FeatureLiveTarget(f);
            if (target != kNoLiveTarget) feature_edits[f] = add(L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL,
                kFeatureValue + static_cast<int>(f), StatNumber(client_settings.values[target], 2).c_str());
            feature_buttons[f] = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kFeatureToggle + static_cast<int>(f));
            feature_counts[f] = add(L"STATIC", SS_LEFT);
        }
        number_edit = add(L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL, 110, StatNumber(client_settings.values[kStatTargetOffset + kArmorIndex], vst::kNativeStats[kArmorIndex].decimals).c_str());
        toggle_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kToggle);
        stat_pane = CreateWindowExW(WS_EX_CONTROLPARENT, L"VST.Native.StatPane", L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 1, 1, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        for (size_t index = 0; index < vst::kNativeStatCount; ++index) {
            stat_labels[index] = add(L"STATIC", SS_LEFT, 0, L"", stat_pane);
            stat_edits[index] = add(L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL, kStatValue + static_cast<int>(index), StatNumber(client_settings.values[kStatTargetOffset + index], vst::kNativeStats[index].decimals).c_str(), stat_pane);
            stat_buttons[index] = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON | BS_NOTIFY, kStatToggle + static_cast<int>(index), L"", stat_pane);
            stat_values[index] = add(L"STATIC", SS_LEFT, 0, L"", stat_pane);
            stat_counts[index] = add(L"STATIC", SS_LEFT, 0, L"", stat_pane);
        }
        hotkeys_pane = CreateWindowExW(WS_EX_CONTROLPARENT, L"VST.Native.StatPane", L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 1, 1, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        for (size_t action = 0; action < vst::client::kHotkeyCount; ++action) {
            hotkey_labels[action] = add(L"STATIC", SS_LEFT, 0, L"", hotkeys_pane);
            hotkey_buttons[action] = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON | BS_NOTIFY, kHotkeyCapture + static_cast<int>(action), L"", hotkeys_pane);
            hotkey_notes[action] = add(L"STATIC", SS_LEFT, 0, L"", hotkeys_pane);
        }
        game_directory_label = add(L"STATIC", SS_LEFT); game_directory_edit = add(L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL, kGameDirectory);
        SendMessageW(game_directory_edit, EM_SETLIMITTEXT, 32767, 0);
        browse_game_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kBrowseGame);
        create_backup_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kCreateBackup); list_backups_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kListBackups);
        restore_backup_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kRestoreBackup);
        backup_list = add(L"LISTBOX", WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, kBackupList);
        maintenance_label = add(L"STATIC", SS_LEFT);
        gold_label = add(L"STATIC", SS_LEFT); gold_balance_label = add(L"STATIC", SS_LEFT); gold_readiness_label = add(L"STATIC", SS_LEFT);
        gold_target_label = add(L"STATIC", SS_LEFT); gold_value_edit = add(L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL, kGoldValue);
        SendMessageW(gold_value_edit, EM_SETLIMITTEXT, 32, 0);
        set_gold_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kSetGold);
        gold_backup_button = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kGoldBackup); gold_feedback_label = add(L"STATIC", SS_LEFT);
        reward_search = add(L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL, kRewardSearch); SendMessageW(reward_search, EM_SETLIMITTEXT, 512, 0);
        reward_filter = add(L"COMBOBOX", WS_TABSTOP | CBS_DROPDOWNLIST, kRewardFilter); reward_refresh = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kRewardRefresh);
        reward_summary = add(L"STATIC", SS_LEFT);
        reward_list = add(WC_LISTVIEWW, WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS, kRewardList);
        ListView_SetExtendedListViewStyle(reward_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        for (int index = 0; index < 4; ++index) { LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.cx = Scale(100); column.pszText = const_cast<wchar_t*>(L""); SendMessageW(reward_list, LVM_INSERTCOLUMNW, index, reinterpret_cast<LPARAM>(&column)); }
        InitializeRewardImages();
        reward_selection = add(L"STATIC", SS_LEFT); reward_active = add(L"STATIC", SS_LEFT); reward_operation = add(L"STATIC", SS_LEFT);
        reward_chest_record = add(L"STATIC", SS_LEFT); reward_upgrade_record = add(L"STATIC", SS_LEFT);
        reward_apply = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kRewardApply); reward_chest = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kRewardChest);
        reward_upgrade = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kRewardUpgrade); reward_both = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kRewardBoth); reward_stop = add(L"BUTTON", WS_TABSTOP | BS_PUSHBUTTON, kRewardStop);
        ApplyFonts(); UpdateTheme();
        syncing_edits = false; Refresh(); ShowPageControls(); Layout(); return 0;
    }
    if (message == WM_SIZE) {
        if (wparam == SIZE_MINIMIZED) {
            status_refresh_deferred = true;
            KillTimer(hwnd, kRewardIconTimer);
        } else {
            Layout();
            if (status_refresh_deferred && !closing) {
                status_refresh_deferred = false;
                Refresh();
                QueueInitialRewardCatalog();
                if (rewards_page && !diagnostics_mode) SetTimer(hwnd, kRewardIconTimer, 180, nullptr);
            }
        }
        return 0;
    }
    if (message == WM_DPICHANGED) { dpi = HIWORD(wparam); ClearRewardImages(); InitializeRewardImages(); UpdateFonts(); auto rect = reinterpret_cast<RECT*>(lparam); SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE); Layout(); return 0; }
    if (message == WM_GETMINMAXINFO) { auto info = reinterpret_cast<MINMAXINFO*>(lparam); info->ptMinTrackSize = {Scale(850), Scale(750)}; return 0; }
    if (message == kStatusMessage) {
        auto status = reinterpret_cast<Status*>(lparam);
        if (displayed.connected && (!status->connected || displayed.pid != status->pid)) ResetRewardCatalog();
        if (status->reward_result) {
            reward_operation_error = status->command_ok ? "" : status->error.empty() ? "The game did not confirm the reward request." : status->error;
            reward_error_scope = status->error_scope;
        }
        if (!status->connected || !status->player_ready) CancelAllLiveValues();
        else for (size_t target = 0; target < kLiveTargetCount; ++target)
            if (LiveTargetEnabled(displayed, target) && !LiveTargetEnabled(*status, target)) CancelLiveValue(target);
        gold_submission.Accept(*status);
        displayed = std::move(*status); delete status;
        worker.Acknowledge(displayed.client_sequence);
        if (connection_sequence && displayed.client_sequence >= connection_sequence) connection_sequence = 0;
        if (hotkey_pending_sequence && displayed.client_sequence >= hotkey_pending_sequence) hotkey_pending_sequence = 0;
        if (reward_pending_sequence && displayed.client_sequence >= reward_pending_sequence) reward_pending_sequence = 0;
        if (!displayed.game_directory.empty() && GetWindowTextLengthW(game_directory_edit) == 0) SetText(game_directory_edit, displayed.game_directory);
        if (!closing) {
            ApplyRewardCatalogResult(displayed);
            if (IsIconic(hwnd)) status_refresh_deferred = true;
            else Refresh();
            WakeLiveValues(); UpdateHotkeys(); QueueInitialRewardCatalog();
        }
        return 0;
    }
    if (message == kRewardIconMessage) {
        std::unique_ptr<RewardIconResult> result(reinterpret_cast<RewardIconResult*>(lparam));
        if (!closing && result) ReceiveRewardIcon(std::move(*result));
        return 0;
    }
    if (message == kSettingsMessage) { settings_notice = wparam ? "" : "Settings were not saved; changes apply only in this window."; if (!closing) Refresh(); return 0; }
    if (message == kMaintenanceMessage) {
        std::unique_ptr<MaintenanceResult> result(reinterpret_cast<MaintenanceResult*>(lparam)); maintenance_busy = false;
        if (!result) maintenance_notice = "The native client operation failed.";
        else {
            if (result->ok) {
                backup_entries = std::move(result->entries); RefreshBackupList();
                maintenance_notice = result->action == Action::CreateBackup ? "Backup created. Exit the game before restoring it."
                    : result->action == Action::RestoreBackup ? "Backup restored. The previous progress has a safety snapshot." : "Backup list refreshed.";
            } else maintenance_notice = result->error + (result->restored.rolled_back ? " Current progress was restored after the failure." : "");
        }
        if (!closing) Refresh(); return 0;
    }
    if (message == kStoppedMessage) { DestroyWindow(hwnd); return 0; }
    if (message == WM_TIMER && wparam == kLiveValueTimer) {
        for (size_t target = 0; target < kLiveTargetCount; ++target) FlushLiveValue(target, false);
        WakeLiveValues(); return 0;
    }
    if (message == WM_TIMER && wparam == kHotkeyTimer) { UpdateHotkeys(); return 0; }
    if (message == WM_TIMER && wparam == kSettingsTimer) { QueueSettingsSave(); return 0; }
    if (message == WM_TIMER && wparam == kRewardSearchTimer) { KillTimer(hwnd, kRewardSearchTimer); FilterRewards(); Refresh(); return 0; }
    if (message == WM_TIMER && wparam == kRewardIconTimer) { WorkRewardIcons(GetTickCount64()); return 0; }
    if (message == WM_TIMER && wparam == kRewardCatalogTimer) {
        KillTimer(hwnd, kRewardCatalogTimer);
        if (!closing && !diagnostics_mode && displayed.connected && reward_catalog_loading && !reward_catalog_request_pending)
            if (!QueueRewardPage(reward_catalog_next)) SetTimer(hwnd, kRewardCatalogTimer, 200, nullptr);
        return 0;
    }
    if (message == WM_HOTKEY) {
        size_t action = static_cast<size_t>(wparam - vst::client::kHotkeyMessageBase);
        vst::client::HotkeyChord chord{HIWORD(lparam), static_cast<UINT>(LOWORD(lparam) & (MOD_ALT | MOD_CONTROL | MOD_SHIFT))};
        if (HotkeysAllowed() && hotkey_manager.Accept(action, chord, displayed.pid, true, EligibleHotkeys(displayed), KeyDown)) ActivateHotkey(action);
        return 0;
    }
    if (message == WM_MOUSEWHEEL && (stats_page || hotkeys_page)) return SendMessageW(stats_page ? stat_pane : hotkeys_pane, message, wparam, lparam);
    if (message == WM_NOTIFY) {
        auto header = reinterpret_cast<NMHDR*>(lparam);
        if (header && header->code == NM_CUSTOMDRAW) return DrawThemedControl(header);
        if (header && header->hwndFrom == reward_list && (header->code == LVN_BEGINSCROLL || header->code == LVN_ENDSCROLL)) {
            reward_last_scroll = GetTickCount64();
            return 0;
        }
        if (header && header->hwndFrom == reward_list && header->code == LVN_GETDISPINFOW) {
            auto info = reinterpret_cast<NMLVDISPINFOW*>(lparam); int row = info->item.iItem;
            const wchar_t* value = L"";
            if (info->item.mask & LVIF_IMAGE) info->item.iImage = I_IMAGENONE;
            if (row >= 0 && static_cast<size_t>(row) < reward_filtered.size() && reward_filtered[static_cast<size_t>(row)] < reward_catalog.size()) {
                const auto& item = reward_catalog[reward_filtered[static_cast<size_t>(row)]];
                if ((info->item.mask & LVIF_IMAGE) && info->item.iSubItem == 0) {
                    auto image = reward_image_indices.find(item.id);
                    if (image != reward_image_indices.end()) info->item.iImage = image->second;
                }
                if (info->item.iSubItem == 0) { const auto& name = english ? item.english_name : item.name; value = name.empty() ? item.wide_id.c_str() : name.c_str(); }
                else if (info->item.iSubItem == 1) { const auto& name = english ? item.name : item.english_name; value = name.empty() ? L"—" : name.c_str(); }
                else if (info->item.iSubItem == 2) value = item.kind == "passive" ? item.evolution ? english ? L"Evolved passive" : L"进化被动" : english ? L"Passive" : L"被动装备"
                    : item.evolution ? english ? L"Evolution" : L"进化武器" : english ? L"Weapon" : L"武器";
                else if (info->item.iSubItem == 3) value = item.wide_id.c_str();
            }
            if (info->item.mask & LVIF_TEXT) info->item.pszText = const_cast<wchar_t*>(value);
            return 0;
        }
        if (header && header->hwndFrom == reward_list && !reward_filtering && (header->code == LVN_ITEMCHANGED || header->code == LVN_ODSTATECHANGED)) {
            if (header->code == LVN_ITEMCHANGED) {
                auto change = reinterpret_cast<NMLISTVIEW*>(lparam);
                if (!(change->uChanged & LVIF_STATE) || !((change->uOldState ^ change->uNewState) & LVIS_SELECTED)) return 0;
            } else {
                auto change = reinterpret_cast<NMLVODSTATECHANGE*>(lparam);
                if (!((change->uOldState ^ change->uNewState) & LVIS_SELECTED)) return 0;
            }
            if (const auto* selected = SelectedRewardItem()) {
                reward_selected_id = selected->id;
                if (client_settings.reward_target_id != selected->id) { client_settings.reward_target_id = selected->id; MarkSettingsDirty(); }
            }
            if (rewards_page) RefreshRewards();
            return 0;
        }
    }
    if (message == WM_COMMAND && !closing) {
        int id = LOWORD(wparam);
        if (id == kLanguageChinese || id == kLanguageEnglish) {
            bool use_english = id == kLanguageEnglish;
            if (HIWORD(wparam) == BN_CLICKED && english != use_english) {
                english = client_settings.english = use_english;
                MarkSettingsDirty();
                Refresh();
            }
            return 0;
        }
        if (id == kAbout) { CreateWindowExW(0, L"VST.Native.Licenses", Text(L"说明与许可 · Beason9527", L"About & licenses · Beason9527").c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 820, 580, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr); return 0; }
        if (id >= kCommonPage && id <= kGoldPage) { SetPage(id); return 0; }
        if (diagnostics_mode) return 0;
        if (id == kRewardSearch && HIWORD(wparam) == EN_CHANGE) { SetTimer(hwnd, kRewardSearchTimer, 90, nullptr); return 0; }
        if (id == kRewardFilter && HIWORD(wparam) == CBN_SELCHANGE) { FilterRewards(); Refresh(); return 0; }
        if (id == kRewardRefresh) { BeginRewardCatalog(); return 0; }
        if (id >= kRewardApply && id <= kRewardStop) {
            SubmitReward(id == kRewardApply ? -1 : id == kRewardChest || id == kRewardBoth ? 1 : 0,
                id == kRewardApply ? -1 : id == kRewardUpgrade || id == kRewardBoth ? 1 : 0); return 0;
        }
        if (id >= kHotkeyCapture && id < kHotkeyCapture + static_cast<int>(vst::client::kHotkeyCount)) {
            size_t action = static_cast<size_t>(id - kHotkeyCapture);
            if (HIWORD(wparam) == BN_SETFOCUS) EnsureHotkeyVisible(action);
            else if (HIWORD(wparam) == BN_CLICKED) BeginHotkeyCapture(action);
            return 0;
        }
        if (id == kBrowseGame) {
            auto folder = PickGameDirectory(false);
            if (!folder.empty()) SetText(game_directory_edit, folder);
            return 0;
        }
        if (id == kCreateBackup || id == kListBackups || id == kRestoreBackup) { BeginMaintenance(id == kCreateBackup ? Action::CreateBackup : id == kListBackups ? Action::ListBackups : Action::RestoreBackup); return 0; }
        if (id == kBackupList && HIWORD(wparam) == LBN_SELCHANGE) { Refresh(); return 0; }
        if (id == kGoldBackup) { SetPage(kMaintenancePage); return 0; }
        if (id == kGoldValue && HIWORD(wparam) == EN_CHANGE) {
            if (gold_page && !gold_submission.pending && !syncing_edits) { gold_submission = {}; RefreshGoldPage(); }
            return 0;
        }
        if (id == kSetGold) {
            double value = 0;
            if (!gold_page || !CanSaveGold()) return 0;
            gold_submission = {};
            if (!ReadGoldValue(value)) {
                MessageBoxW(hwnd, Text(L"请输入 0–999999999 范围内可精确保存的整数，例如 100000000。", L"Enter a whole number from 0 to 999,999,999 that the game can save exactly, such as 100000000.").c_str(), L"Vampire Survivors", MB_OK);
            } else if (!worker.Push({Action::SetGold, value}, &gold_submission.pending)) {
                gold_submission.result = GoldSubmission::Result::Failed;
                gold_submission.error = "The native client is busy. Try again shortly.";
            } else { gold_submission.value = value; SuspendHotkeys(); }
            RefreshGoldPage(); return 0;
        }
        size_t live_target = LiveTargetForId(id);
        if (live_target != kNoLiveTarget) {
            if (HIWORD(wparam) == EN_CHANGE) ChangedLiveValue(live_target);
            else if (HIWORD(wparam) == EN_KILLFOCUS) { FlushLiveValue(live_target, true); WakeLiveValues(); }
            else if (HIWORD(wparam) == EN_SETFOCUS && id >= kStatValue) EnsureStatVisible(static_cast<size_t>(id - kStatValue));
            return 0;
        }
        if (id == kConnect) { BeginConnection(); return 0; }
        if (id == kManualConnect) {
            if (!closing && !diagnostics_mode && !displayed.connected && !displayed.connecting_automatically && !connection_sequence && worker.BeginGameSelection()) {
                auto folder = PickGameDirectory(true);
                if (!folder.empty()) BeginConnection(std::move(folder));
                worker.EndGameSelection();
            }
            return 0;
        }
        if (id == kDisable) {
            CancelAllLiveValues();
            worker.Push({Action::Disable}); return 0;
        }
        if (id == kToggle) {
            size_t target_id = kStatTargetOffset + kArmorIndex;
            if (displayed.enabled) { CancelLiveValue(target_id); worker.Push({Action::Toggle, displayed.target, 0, false}); return 0; }
            double target = 0;
            if (!ReadLiveValue(target_id, target)) { MessageBoxW(hwnd, Text(L"护甲目标值应在 0 到 10000 之间。", L"Armor must be a finite value from 0 to 10000.").c_str(), L"Vampire Survivors", MB_OK); return 0; }
            CancelLiveValue(target_id);
            worker.Push({Action::Toggle, target, 0, true}); return 0;
        }
        if (id >= kFeatureToggle && id < kFeatureToggle + static_cast<int>(kFeatures.size())) {
            size_t feature = static_cast<size_t>(id - kFeatureToggle);
            double value = 1;
            const size_t target = FeatureLiveTarget(feature);
            if (target != kNoLiveTarget) {
                if (!displayed.features[feature].enabled && !ReadLiveValue(target, value)) {
                    std::wstring reason = feature == kGameSpeedFeature ? Message("Game speed must be a finite number from 1 to 10.")
                        : feature == 3 ? Text(L"移动倍率应在 0.25 到 10 之间。", L"Movement multiplier must be from 0.25 to 10.")
                        : Text(L"倍率应在 1 到 1000 之间。", L"Multiplier must be from 1 to 1000.");
                    MessageBoxW(hwnd, reason.c_str(), L"Vampire Survivors", MB_OK);
                    return 0;
                }
                CancelLiveValue(target);
            }
            worker.Push({Action::Feature, value, feature, !displayed.features[feature].enabled}); return 0;
        }
        if (id >= kStatToggle && id < kStatToggle + static_cast<int>(vst::kNativeStatCount)) {
            size_t index = static_cast<size_t>(id - kStatToggle), target = kStatTargetOffset + index;
            if (HIWORD(wparam) == BN_SETFOCUS) { EnsureStatVisible(index); return 0; }
            if (HIWORD(wparam) != BN_CLICKED) return 0;
            bool enabling = !displayed.stats[index].enabled;
            double value = displayed.stats[index].value;
            const auto& definition = vst::kNativeStats[index];
            if (enabling && !ReadLiveValue(target, value)) {
                std::wstring text = Text(definition.name_zh, definition.name_en) + Text(L"应在 ", L" must be from ")
                    + StatNumber(definition.minimum, definition.decimals) + L"–" + StatNumber(definition.maximum, definition.decimals)
                    + (definition.unit[0] ? L" " + std::wstring(definition.unit) : L"")
                    + (definition.decimals == 0 ? Text(L"，且为整数。", L", in whole numbers.") : L"。");
                MessageBoxW(hwnd, text.c_str(), L"Vampire Survivors", MB_OK); return 0;
            }
            CancelLiveValue(target);
            Command command{Action::Stat, value}; command.stat = index; command.enabled = enabling;
            worker.Push(command); return 0;
        }
    }
    if (message == WM_CLOSE) {
        if (diagnostics_mode) DestroyWindow(hwnd);
        else if (!closing) { CancelHotkeyCapture(); closing = true; CancelAllLiveValues(); Refresh(); SetText(status_label, Text(L"正在断开，等待当前操作结束…", L"Disconnecting, waiting for the current operation…")); Command command{Action::Stop}; if (settings_writable && settings_dirty) command.settings = std::make_shared<vst::client::ClientSettings>(client_settings); worker.Push(std::move(command)); }
        return 0;
    }
    if (message == WM_DESTROY) { SuspendHotkeys(); KillTimer(hwnd, kLiveValueTimer); KillTimer(hwnd, kHotkeyTimer); KillTimer(hwnd, kSettingsTimer); KillTimer(hwnd, kRewardSearchTimer); KillTimer(hwnd, kRewardCatalogTimer); KillTimer(hwnd, kRewardIconTimer); ClearRewardImages(); PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
bool CheckLanguageButtons() {
    auto shows_language = [](bool use_english) {
        return english == use_english && client_settings.english == use_english
            && SendMessageW(chinese_button, BM_GETCHECK, 0, 0) == (use_english ? BST_UNCHECKED : BST_CHECKED)
            && SendMessageW(english_button, BM_GETCHECK, 0, 0) == (use_english ? BST_CHECKED : BST_UNCHECKED)
            && ControlText(rewards_page_button) == (use_english ? L"Rewards" : L"指定奖励");
    };
    for (bool use_english : {true, false}) {
        for (int pass = 0; pass < 12; ++pass) {
            for (HWND button : {chinese_button, english_button}) {
                SendMessageW(button, WM_MOUSEMOVE, 0, MAKELPARAM(5, 5));
                SendMessageW(button, WM_MOUSELEAVE, 0, 0);
                if (!shows_language(!use_english)) return false;
            }
        }
        SendMessageW(use_english ? english_button : chinese_button, BM_CLICK, 0, 0);
        if (!shows_language(use_english)) return false;
    }
    return true;
}
bool CheckInterfaceLayout(HWND hwnd) {
    SetWindowPos(hwnd, nullptr, -32000, -32000, Scale(850), Scale(750), SWP_NOZORDER | SWP_NOACTIVATE);
    RECT client{};
    GetClientRect(hwnd, &client);
    auto area = [&](HWND control) {
        RECT rect{};
        GetWindowRect(control, &rect);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&rect), 2);
        return rect;
    };
    auto inside = [&](HWND control) {
        const RECT rect = area(control);
        return IsWindowVisible(control) && rect.left >= client.left && rect.top >= client.top
            && rect.right <= client.right && rect.bottom <= client.bottom && rect.right > rect.left && rect.bottom > rect.top;
    };
    SetPage(kCommonPage);
    if (!inside(connect_button) || !inside(manual_connect_button) || !inside(number_edit) || !inside(feature_buttons[kChestAnimationFeature])
        || area(connect_button).right > area(manual_connect_button).left || area(manual_connect_button).right > area(disable_button).left
        || !inside(count_label) || area(count_label).bottom > area(guide_label).top
        || area(heading).right > area(chinese_button).left || area(chinese_button).right > area(english_button).left) return false;
    if (feature_edits[0] || !inside(feature_edits[kGameSpeedFeature])) return false;

    SetPage(kStatsPage);
    if (!inside(stat_pane) || IsWindowVisible(number_edit)) return false;
    SetPage(kHotkeysPage);
    if (!inside(hotkeys_pane) || IsWindowVisible(stat_pane)) return false;
    SetPage(kMaintenancePage);
    if (!inside(backup_list) || area(backup_list).bottom > area(maintenance_label).top) return false;
    SetPage(kGoldPage);
    if (!inside(gold_page_button) || !inside(gold_balance_label) || !inside(gold_value_edit) || !inside(set_gold_button)
        || !inside(gold_backup_button) || !inside(gold_feedback_label) || IsWindowVisible(backup_list)
        || area(set_gold_button).right > area(gold_backup_button).left || area(gold_feedback_label).bottom > area(guide_label).top) return false;
    SetPage(kRewardsPage);
    return inside(reward_list) && inside(reward_selection) && inside(reward_stop) && inside(guide_label)
        && area(reward_list).bottom < area(reward_selection).top
        && area(reward_upgrade_record).bottom < area(guide_label).top;
}
bool CheckGoldPage() {
    SetPage(kGoldPage);
    double value = 0;
    SetText(gold_value_edit, L"0");
    if (!ReadGoldValue(value) || value != 0) return false;
    SetText(gold_value_edit, L"100000000");
    if (!ReadGoldValue(value) || value != 100000000) return false;
    SetText(gold_value_edit, L"999999999");
    if (ReadGoldValue(value)) return false;
    SetText(gold_value_edit, L"");
    if (ReadGoldValue(value)) return false;

    using Result = GoldSubmission::Result;
    Status reply;
    reply.connected = reply.gold_known = true;
    reply.gold = 321;
    reply.client_sequence = 11;
    GoldSubmission pending{10, 321};
    pending.Accept(reply);
    if (pending.pending != 10 || pending.result != Result::None) return false;
    reply.gold_reply_sequence = 10;
    pending.Accept(reply);
    if (pending.pending || pending.result != Result::Saved) return false;
    pending = {10, 321}; reply.gold = 300;
    pending.Accept(reply);
    if (pending.result != Result::Unconfirmed) return false;
    pending = {10, 321}; reply.command_ok = false; reply.error = "The native wallet balance is unavailable.";
    pending.Accept(reply);
    if (pending.result != Result::Failed || pending.error != reply.error) return false;
    pending = {10, 321}; reply.gold_cancelled = true;
    pending.Accept(reply);
    return pending.result == Result::Cancelled && GetWindowTextLengthW(gold_value_edit) == 0;
}
bool CheckRewardIconScroll() {
    SetPage(kRewardsPage);
    if (!reward_images || ListView_GetImageList(reward_list, LVSIL_SMALL) != reward_images) return false;
    for (int row = 0; row < 96; ++row) {
        vst::client::RewardItem item;
        item.id = "SCROLL_" + std::to_string(row);
        item.name = L"物品 " + std::to_wstring(row);
        item.english_name = L"Item " + std::to_wstring(row);
        item.kind = "weapon";
        vst::client::PrepareRewardItem(item);
        reward_catalog.push_back(std::move(item));
    }
    FilterRewards();
    RECT bounds{};
    if (!ListView_GetItemRect(reward_list, 0, &bounds, LVIR_BOUNDS)) return false;
    const int row_height = bounds.bottom - bounds.top;
    reward_last_scroll = 0;
    if (row_height <= 0 || !ListView_Scroll(reward_list, 0, row_height * 10) || !reward_last_scroll) return false;
    const int first = ListView_GetTopIndex(reward_list);
    if (first <= 0 || first >= static_cast<int>(reward_filtered.size())) return false;
    const std::string id = reward_catalog[reward_filtered[first]].id;
    vst::client::RewardIcon icon;
    icon.id = id;
    icon.width = icon.height = 2;
    icon.rgba.assign(16, 255);
    ++reward_catalog_generation;
    reward_icon_pending_id = id;
    ReceiveRewardIcon({reward_catalog_generation - 1, id, true, icon});
    if (reward_icon_pending_id != id || !reward_icon_waiting.id.empty()) return false;
    SendMessageW(window, kRewardIconMessage, 0, reinterpret_cast<LPARAM>(
        new RewardIconResult{reward_catalog_generation, id, true, std::move(icon)}));
    if (!reward_icon_pending_id.empty() || reward_icon_waiting.id != id) return false;
    for (int step = 0; step < 6; ++step) {
        WorkRewardIcons(reward_last_scroll + kRewardScrollQuietMs - 1);
        if (!reward_image_indices.empty() || ImageList_GetImageCount(reward_images) != 0) return false;
        const int top = ListView_GetTopIndex(reward_list);
        Refresh();
        if (ListView_GetTopIndex(reward_list) != top || !ListView_Scroll(reward_list, 0, row_height * 2)) return false;
    }
    const int final_top = ListView_GetTopIndex(reward_list);
    if (final_top <= first) return false;
    WorkRewardIcons(reward_last_scroll + kRewardScrollQuietMs);
    auto cached = reward_image_indices.find(id);
    return cached != reward_image_indices.end() && cached->second >= 0 && reward_icon_waiting.id.empty()
        && ImageList_GetImageCount(reward_images) == 1 && ListView_GetTopIndex(reward_list) == final_top
        && ListView_GetItemRect(reward_list, final_top, &bounds, LVIR_BOUNDS) && bounds.bottom - bounds.top == row_height;
}

bool SavePreview(HWND hwnd, const std::wstring& path) {
    RECT rectangle{}; GetWindowRect(hwnd, &rectangle); int width = rectangle.right - rectangle.left, height = rectangle.bottom - rectangle.top;
    HDC source = GetWindowDC(hwnd), memory = CreateCompatibleDC(source); HBITMAP bitmap = CreateCompatibleBitmap(source, width, height);
    HGDIOBJ prior = SelectObject(memory, bitmap);
    // WM_PRINT paints this owned GDI window and its children even offscreen;
    // PrintWindow can report success while returning an entirely black client.
    RECT canvas{0, 0, width, height};
    FillRect(memory, &canvas, GetSysColorBrush(COLOR_WINDOW));
    SendMessageW(hwnd, WM_PRINT, reinterpret_cast<WPARAM>(memory), PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    bool okay = true;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    SelectObject(memory, prior);
    okay = okay && GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS) != 0;
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(hwnd, source);
    if (!okay) return false;
    BITMAPFILEHEADER file_header{}; file_header.bfType = 0x4d42; file_header.bfOffBits = sizeof(file_header) + sizeof(BITMAPINFOHEADER); file_header.bfSize = file_header.bfOffBits + static_cast<DWORD>(pixels.size());
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)); DWORD written = 0;
    return file && WriteFile(file.get(), &file_header, sizeof(file_header), &written, nullptr) && WriteFile(file.get(), &info.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr) && WriteFile(file.get(), pixels.data(), static_cast<DWORD>(pixels.size()), &written, nullptr);
}
int RunGui(const std::wstring& preview, bool smoke, std::wstring_view fixture = {}) {
    diagnostics_mode = smoke || !preview.empty();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES}; if (!InitCommonControlsEx(&controls)) return 1;
    struct ComScope { HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); } } com;
    if (!diagnostics_mode) LoadClientSettings();
    reward_selected_id = client_settings.reward_target_id;
    if (!fixture.empty()) {
        if (!diagnostics_mode) return 1;
        displayed = {};
        if (fixture == L"detected") displayed.game_scan.pid = 33;
        else if (fixture == L"old-loader") displayed.error = "The old game loader is active. Exit the game and uninstall the old companion plugin before this native test.";
        else if (fixture == L"old-native") displayed.error = "A different native build is already loaded. Exit the game before updating the pilot.";
        else if (fixture == L"connected" || fixture == L"stats" || fixture == L"stats-bottom" || fixture == L"hotkeys" || fixture == L"hotkeys-bottom" || fixture == L"maintenance" || fixture == L"backup" || fixture == L"gold" || fixture == L"gold-needs-backup"
            || fixture == L"rewards" || fixture == L"rewards-offer" || fixture == L"rewards-search" || fixture == L"rewards-chest-only" || fixture == L"rewards-error") {
            displayed.connected = true; displayed.player_ready = true; displayed.supported = true;
            displayed.received_at = GetTickCount64(); displayed.backup_authorized = true; displayed.wallet_ready = true;
            displayed.gold_known = true; displayed.gold = 123456; displayed.gold_write_supported = true;
            displayed.original_known = true; displayed.original = 3; displayed.reads = 42;
            for (auto& feature : displayed.features) feature.supported = true;
            displayed.features[3].enabled = true; displayed.features[3].value = 2; displayed.features[3].overrides = 42;
            for (size_t index = 0; index < vst::kNativeStatCount; ++index) {
                auto& state = displayed.stats[index]; const auto& definition = vst::kNativeStats[index];
                state.supported = true; state.value = definition.initial;
                state.original_known = definition.stat != vst::NativeStat::Omni;
                state.original = definition.stat == vst::NativeStat::Armor ? 3 : 0;
                state.reads = 42;
                if (index >= static_cast<size_t>(vst::NativeStat::Revivals)) { state.bound_known = true; state.bound = definition.initial; }
            }
            displayed.stats[static_cast<size_t>(vst::NativeStat::Might)].enabled = true;
            displayed.stats[static_cast<size_t>(vst::NativeStat::Might)].current_known = true;
            displayed.stats[static_cast<size_t>(vst::NativeStat::Might)].current = 100;
            displayed.stats[static_cast<size_t>(vst::NativeStat::Might)].overrides = 42;
            stats_page = fixture == L"stats" || fixture == L"stats-bottom";
            if (fixture == L"stats-bottom") stat_scroll = std::numeric_limits<int>::max();
            hotkeys_page = fixture == L"hotkeys" || fixture == L"hotkeys-bottom";
            if (fixture == L"hotkeys-bottom") hotkey_scroll = std::numeric_limits<int>::max();
            maintenance_page = fixture == L"maintenance" || fixture == L"backup";
            gold_page = fixture == L"gold" || fixture == L"gold-needs-backup";
            if (gold_page) {
                displayed.player_ready = false;
                displayed.scene = "MainMenu";
                displayed.note = "Offline layout preview. No game connection.";
                displayed.backup_authorized = displayed.gold_write_ready = fixture == L"gold";
            }
            if (maintenance_page) {
                displayed.player_ready = false; displayed.gold_write_ready = true;
                for (int index = 0; index < 3; ++index) { vst::maintenance::BackupInfo backup; backup.id = "preview-" + std::to_string(index); backup.created_utc = "2026-11-17T08:30:00Z"; backup.file_count = 3; backup_entries.push_back(std::move(backup)); }
            }
            rewards_page = fixture.starts_with(L"rewards");
            if (rewards_page) {
                displayed.note = "Offline layout preview. No game connection.";
                auto add_reward = [&](const char* id, const wchar_t* name, const wchar_t* en, const char* kind, bool evolution, bool upgrade) {
                    vst::client::RewardItem item; item.id = id; item.name = name; item.english_name = en; item.kind = kind; item.evolution = evolution;
                    item.max_level = evolution ? 1 : 8; item.upgrade_known = true; item.upgrade_allowed = upgrade;
                    if (!upgrade) item.upgrade_reason = Text(L"该进化被动仅支持箱子指定。", L"This evolved passive supports chest targeting only.");
                    vst::client::PrepareRewardItem(item); reward_catalog.push_back(std::move(item));
                };
                add_reward("TEST_WAND", L"魔杖（示例）", L"Magic Wand (sample)", "weapon", false, true);
                add_reward("TEST_SPINACH", L"菠菜（示例）", L"Spinach (sample)", "passive", false, true);
                add_reward("TEST_EVOLVED_WAND", L"圣魔杖（示例）", L"Holy Wand (sample)", "weapon", true, true);
                add_reward("TEST_EVOLVED_PASSIVE", L"进化被动（示例）", L"Evolved Passive (sample)", "passive", true, false);
                reward_catalog_requested = true; reward_catalog_total = reward_catalog.size(); reward_selected_id = "TEST_WAND";
                auto& reward = displayed.reward; reward.supported = reward.upgrade_supported = true; reward.enabled = reward.upgrade_enabled = true;
                reward.target_id = reward.last_reward_id = reward.last_offer_id = reward.last_granted_id = "TEST_WAND";
                reward.processed = 2; reward.offered = 4; reward.granted = 1;
                if (fixture == L"rewards-offer") { reward.enabled = false; reward.processed = reward.granted = 0; reward.last_reward_id.clear(); reward.last_granted_id.clear(); }
                if (fixture == L"rewards-chest-only") { reward_selected_id = reward.target_id = "TEST_EVOLVED_PASSIVE"; reward.upgrade_enabled = false; }
                if (fixture == L"rewards-error") { reward_selected_id = "TEST_EVOLVED_PASSIVE"; reward_operation_error = "The selected item does not support upgrade targeting."; reward_error_scope = "upgrade"; }
            }
        } else return 1;
    }
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); cls.lpfnWndProc = WindowProc; cls.lpszClassName = L"VST.Native.Pilot";
    cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(1)); if (!cls.hIcon) cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&cls); WNDCLASSW license = cls; license.lpfnWndProc = LicenseProc; license.lpszClassName = L"VST.Native.Licenses"; RegisterClassW(&license);
    WNDCLASSW pane = cls; pane.lpfnWndProc = StatPaneProc; pane.lpszClassName = L"VST.Native.StatPane"; RegisterClassW(&pane);
    HDC screen = GetDC(nullptr); dpi = screen ? static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSY)) : 96; if (screen) ReleaseDC(nullptr, screen);
    HWND hwnd = CreateWindowExW(0, cls.lpszClassName, L"Vampire Survivors Trainer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, diagnostics_mode ? -32000 : CW_USEDEFAULT, diagnostics_mode ? -32000 : CW_USEDEFAULT, Scale(980), Scale(780), nullptr, nullptr, cls.hInstance, nullptr);
    if (!hwnd) return 1;
    if (maintenance_page) RefreshBackupList();
    if (rewards_page) { if (fixture == L"rewards-search") SetText(reward_search, L"wand"); FilterRewards(); Refresh(); }
    ShowWindow(hwnd, diagnostics_mode ? SW_SHOWNOACTIVATE : SW_SHOW); UpdateWindow(hwnd);
    if (!diagnostics_mode) { worker.Start(); if (settings_dirty) SetTimer(hwnd, kSettingsTimer, 750, nullptr); }
    if (diagnostics_mode) {
        MSG message{}; while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        for (int width : {850, 1100, 980}) { SetWindowPos(hwnd, nullptr, -32000, -32000, Scale(width), Scale(780), SWP_NOZORDER | SWP_NOACTIVATE); UpdateWindow(hwnd); }
        bool okay = smoke ? CheckLanguageButtons() && CheckInterfaceLayout(hwnd) && CheckGoldPage() && CheckRewardIconScroll() : SavePreview(hwnd, preview);
        DestroyWindow(hwnd); DeleteFonts();
        WriteOutput(okay ? "{\"ok\":true,\"gameConnected\":false,\"userDataWritten\":false}\n" : "{\"ok\":false}\n"); return okay ? 0 : 1;
    }
    MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && capturing_hotkey < vst::client::kHotkeyCount) {
            CaptureHotkey(static_cast<UINT>(message.wParam)); continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
            size_t target = LiveTargetForId(GetDlgCtrlID(message.hwnd));
            if (target != kNoLiveTarget) { FlushLiveValue(target, true); WakeLiveValues(); continue; }
            if (message.hwnd == gold_value_edit || message.hwnd == game_directory_edit) continue;
            if (message.hwnd == reward_search || message.hwnd == reward_list) { KillTimer(hwnd, kRewardSearchTimer); FilterRewards(); Refresh(); continue; }
        }
        if (!IsDialogMessageW(hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    worker.Join(); DeleteFonts(); return 0;
}
std::string StatusJson(const Status& value) {
    auto doc = yyjson_mut_doc_new(nullptr); auto root = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "connected", value.connected); yyjson_mut_obj_add_bool(doc, root, "ok", value.command_ok && value.error.empty());
    yyjson_mut_obj_add_bool(doc, root, "playerReady", value.player_ready); yyjson_mut_obj_add_bool(doc, root, "armorSupported", value.supported); yyjson_mut_obj_add_bool(doc, root, "armorEnabled", value.enabled);
    yyjson_mut_obj_add_bool(doc, root, "backupAuthorized", value.backup_authorized); yyjson_mut_obj_add_bool(doc, root, "walletReady", value.wallet_ready);
    yyjson_mut_obj_add_bool(doc, root, "goldWriteSupported", value.gold_write_supported); yyjson_mut_obj_add_bool(doc, root, "goldWriteReady", value.gold_write_ready);
    if (value.gold_known) yyjson_mut_obj_add_real(doc, root, "gold", value.gold); else yyjson_mut_obj_add_null(doc, root, "gold");
    yyjson_mut_obj_add_real(doc, root, "target", value.target); yyjson_mut_obj_add_uint(doc, root, "actualReads", value.reads);
    if (value.original_known) yyjson_mut_obj_add_real(doc, root, "original", value.original); else yyjson_mut_obj_add_null(doc, root, "original");
    if (value.current_known) yyjson_mut_obj_add_real(doc, root, "current", value.current); else yyjson_mut_obj_add_null(doc, root, "current");
    yyjson_mut_obj_add_strcpy(doc, root, "scene", value.scene.c_str()); yyjson_mut_obj_add_strcpy(doc, root, "error", value.error.c_str());
    yyjson_mut_obj_add_strcpy(doc, root, "armorReason", value.armor_reason.c_str());
    auto reward = yyjson_mut_obj(doc); yyjson_mut_obj_add_val(doc, root, "chestReward", reward);
    yyjson_mut_obj_add_bool(doc, reward, "supported", value.reward.supported); yyjson_mut_obj_add_bool(doc, reward, "enabled", value.reward.enabled);
    yyjson_mut_obj_add_bool(doc, reward, "upgradeSupported", value.reward.upgrade_supported); yyjson_mut_obj_add_bool(doc, reward, "upgradeEnabled", value.reward.upgrade_enabled);
    yyjson_mut_obj_add_strcpy(doc, reward, "targetItemId", value.reward.target_id.c_str());
    yyjson_mut_obj_add_uint(doc, reward, "processedCount", value.reward.processed); yyjson_mut_obj_add_uint(doc, reward, "upgradeProcessedCount", value.reward.offered);
    yyjson_mut_obj_add_uint(doc, reward, "upgradeGrantedCount", value.reward.granted);
    yyjson_mut_obj_add_strcpy(doc, reward, "lastRewardItemId", value.reward.last_reward_id.c_str());
    yyjson_mut_obj_add_strcpy(doc, reward, "lastUpgradeItemId", value.reward.last_offer_id.c_str()); yyjson_mut_obj_add_strcpy(doc, reward, "lastUpgradeGrantedItemId", value.reward.last_granted_id.c_str());
    auto features = yyjson_mut_arr(doc); yyjson_mut_obj_add_val(doc, root, "features", features);
    for (size_t f = 0; f < kFeatures.size(); ++f) {
        auto feature = yyjson_mut_obj(doc); yyjson_mut_arr_add_val(features, feature);
        yyjson_mut_obj_add_str(doc, feature, "id", kFeatures[f]);
        yyjson_mut_obj_add_bool(doc, feature, "supported", value.features[f].supported);
        yyjson_mut_obj_add_bool(doc, feature, "enabled", value.features[f].enabled);
        yyjson_mut_obj_add_real(doc, feature, "value", value.features[f].value);
        yyjson_mut_obj_add_uint(doc, feature, "interceptedCalls", value.features[f].calls);
        yyjson_mut_obj_add_uint(doc, feature, "actualCalls", value.features[f].actual_calls);
        yyjson_mut_obj_add_uint(doc, feature, "modifiedCalls", value.features[f].overrides);
    }
    auto stats = yyjson_mut_arr(doc); yyjson_mut_obj_add_val(doc, root, "stats", stats);
    for (size_t index = 0; index < vst::kNativeStatCount; ++index) {
        const auto& state = value.stats[index];
        auto stat = yyjson_mut_obj(doc); yyjson_mut_arr_add_val(stats, stat);
        yyjson_mut_obj_add_str(doc, stat, "id", vst::kNativeStats[index].id);
        yyjson_mut_obj_add_bool(doc, stat, "supported", state.supported); yyjson_mut_obj_add_bool(doc, stat, "enabled", state.enabled);
        yyjson_mut_obj_add_real(doc, stat, "value", state.value);
        if (state.original_known) yyjson_mut_obj_add_real(doc, stat, "originalValue", state.original); else yyjson_mut_obj_add_null(doc, stat, "originalValue");
        if (state.current_known) yyjson_mut_obj_add_real(doc, stat, "currentValue", state.current); else yyjson_mut_obj_add_null(doc, stat, "currentValue");
        if (state.bound_known) yyjson_mut_obj_add_real(doc, stat, "boundValue", state.bound); else yyjson_mut_obj_add_null(doc, stat, "boundValue");
        yyjson_mut_obj_add_uint(doc, stat, "actualReads", state.reads); yyjson_mut_obj_add_uint(doc, stat, "modifiedReads", state.overrides);
        yyjson_mut_obj_add_strcpy(doc, stat, "reason", state.reason.c_str()); yyjson_mut_obj_add_strcpy(doc, stat, "source", state.source.c_str());
    }
    size_t length = 0; char* text = yyjson_mut_write(doc, 0, &length); std::string result = text ? std::string(text, length) + "\n" : "{}\n"; free(text); yyjson_mut_doc_free(doc); return result;
}
int MaintenanceCli(const std::vector<std::wstring>& args) {
    vst::maintenance::Options options; vst::maintenance::BackupInfo backup; vst::maintenance::RestoreResult restored;
    std::vector<vst::maintenance::BackupInfo> entries; std::string error;
    bool valid = vst::maintenance::MakeDefaultOptions(args[1], L"", options, error);
    if (valid) {
        if (args[0] == L"--backup") valid = vst::maintenance::CreateBackup(options, backup, error);
        else if (args[0] == L"--restore") valid = vst::maintenance::RestoreBackup(options, args[2], restored, error);
        else valid = vst::maintenance::ListBackups(options, entries, error);
    }
    auto document = yyjson_mut_doc_new(nullptr); if (!document) { WriteOutput("{\"ok\":false}\n"); return 1; }
    auto root = yyjson_mut_obj(document); yyjson_mut_doc_set_root(document, root);
    yyjson_mut_obj_add_bool(document, root, "ok", valid); yyjson_mut_obj_add_strcpy(document, root, "error", error.c_str());
    auto add_backup = [&](yyjson_mut_val* target, const vst::maintenance::BackupInfo& item) {
        yyjson_mut_obj_add_strcpy(document, target, "id", item.id.c_str());
        yyjson_mut_obj_add_strcpy(document, target, "manifest", vst::Utf8(item.manifest_path).c_str());
        yyjson_mut_obj_add_strcpy(document, target, "proof", vst::Utf8(item.proof_path).c_str());
        yyjson_mut_obj_add_strcpy(document, target, "createdUtc", item.created_utc.c_str());
        yyjson_mut_obj_add_uint(document, target, "fileCount", item.file_count);
    };
    if (args[0] == L"--backup" && valid) add_backup(root, backup);
    else if (args[0] == L"--restore") {
        yyjson_mut_obj_add_uint(document, root, "restoredFileCount", restored.restored_file_count);
        yyjson_mut_obj_add_bool(document, root, "rolledBack", restored.rolled_back);
        if (!restored.safety_backup.id.empty()) { auto safety = yyjson_mut_obj(document); yyjson_mut_obj_add_val(document, root, "safetyBackup", safety); add_backup(safety, restored.safety_backup); }
    } else if (valid) {
        auto array = yyjson_mut_arr(document); yyjson_mut_obj_add_val(document, root, "backups", array);
        for (const auto& item : entries) { auto entry = yyjson_mut_obj(document); yyjson_mut_arr_add_val(array, entry); add_backup(entry, item); }
    }
    size_t length = 0; char* json = yyjson_mut_write(document, 0, &length); WriteOutput(json ? std::string(json, length) + "\n" : "{\"ok\":false}\n");
    free(json); yyjson_mut_doc_free(document); return valid ? 0 : 2;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) try {
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc); if (!argv) return 1;
    std::vector<std::wstring> args; for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]); LocalFree(argv);
    if (args.empty()) return RunGui({}, false);
    if (((args[0] == L"--backup" || args[0] == L"--list-backups") && args.size() == 2) || (args[0] == L"--restore" && args.size() == 3)) return MaintenanceCli(args);
    if (args[0] == L"--licenses" && args.size() == 1) { WriteOutput(std::string(vst::embedded::licenses) + "\n"); return 0; }
    if (args[0] == L"--self-test" && args.size() == 1) {
        std::string error, hash; std::wstring path;
        Channel response_fixture;
        bool valid = worker.CheckQueuePolicy() && CheckLiveValuePolicy() && response_fixture.CheckResponsePolicy()
            && vst::client::CheckHotkeyPolicy() && vst::client::CheckClientSettingsPolicy() && vst::client::CheckRewardModelPolicy()
            && vst::client::CheckGameDiscoveryPolicy() && ValidBundle(error)
            && vst::Sha256("abc", 3, hash, error) && hash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
            && !vst::LocalPath(L"\\\\server\\share\\file", path, error)
            && !vst::LocalPath(L"C:\\file:stream", path, error)
            && !vst::LocalPath(L"relative.dll", path, error);
        vst::GameIdentity identity;
        valid = valid && !vst::VerifyCurrentGame(identity, error);
        WriteOutput(valid ? "{\"ok\":true,\"checks\":13,\"gameConnected\":false,\"userDataWritten\":false}\n" : "{\"ok\":false}\n"); return valid ? 0 : 1;
    }
    if (args[0] == L"--check-bundle" && args.size() == 1) {
        std::string error; bool valid = ValidBundle(error);
        WriteOutput(valid ? "{\"ok\":true,\"backendBytes\":" + std::to_string(vst::embedded::backend_size)
            + ",\"backendSha256\":\"" + vst::embedded::backend_sha256 + "\",\"payloadBytes\":" + std::to_string(sizeof(vst::embedded::backend_payload))
            + ",\"payloadSha256\":\"" + vst::embedded::payload_sha256 + "\"}\n" : "{\"ok\":false}\n"); return valid ? 0 : 2;
    }
    if (args[0] == L"--check-backend-file" && args.size() == 2) {
        Handle lock;
        std::string error;
        bool valid = LockVerifiedBackend(args[1], lock, error);
        WriteOutput(valid ? "{\"ok\":true}\n" : "{\"ok\":false}\n"); return valid ? 0 : 2;
    }
    if (args[0] == L"--inspect" && args.size() == 2) {
        std::string error; bool valid = vst::VerifyGameDirectory(args[1], error);
        Status status; status.command_ok = valid; status.error = error; WriteOutput(StatusJson(status)); return valid ? 0 : 2;
    }
    if (args[0] == L"--ui-smoke" && args.size() == 1) return RunGui({}, true);
    if (args[0] == L"--render-preview" && args.size() >= 2 && args.size() <= 4) {
        if (args.size() >= 3 && args[2] != L"en" && args[2] != L"zh-CN") return 1;
        english = args.size() >= 3 && args[2] == L"en";
        return RunGui(args[1], false, args.size() == 4 ? std::wstring_view(args[3]) : std::wstring_view{});
    }
    if ((args[0] == L"--status" || args[0] == L"--connect") && args.size() == 1) {
        Channel channel; Status status; bool connected = channel.Connect(args[0] == L"--connect", status);
        WriteOutput(StatusJson(status)); if (connected) channel.Exchange("disableAll", false, false, 0, status); channel.Close(); return connected ? 0 : 1;
    }
    WriteOutput("Usage: VampireSurvivorsTrainer.Native.exe [--inspect <game-directory> | --backup <game-directory> | --list-backups <game-directory> | --restore <game-directory> <manifest-or-proof> | --status | --connect | --check-bundle | --self-test | --licenses | --ui-smoke | --render-preview <bmp-path> [zh-CN|en] [detected|old-loader|old-native|connected|stats|stats-bottom|hotkeys|hotkeys-bottom|maintenance|backup|gold|gold-needs-backup|rewards|rewards-offer|rewards-search|rewards-chest-only|rewards-error]]\n"); return 1;
} catch (...) { WriteOutput("{\"ok\":false,\"error\":\"Native client operation failed.\"}\n"); return 1; }
