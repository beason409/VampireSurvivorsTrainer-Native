#pragma once

#include "reward_model.hpp"
#include "ui_theme.hpp"
#include <unordered_map>

namespace vst::client {

struct RecipeName {
    std::string id;
    std::wstring chinese, english;

    const std::wstring& Select(bool use_english) const noexcept {
        const auto& chosen = use_english ? english : chinese;
        return chosen.empty() ? (use_english ? chinese : english) : chosen;
    }
};

inline RecipeName MakeRecipeName(const RewardItem& item) {
    const auto readable = [&](const std::wstring& value) -> std::wstring {
        if (value.empty() || value == item.wide_id || value == L"." || value == L"-"
            || value.starts_with(L"lang/") || value.starts_with(L"weaponlang/")) return {};
        return value;
    };
    RecipeName name{item.id, readable(item.chinese_name), readable(item.english_name)};
    const auto current = readable(item.name);
    if (name.chinese.empty()) name.chinese = current;
    if (name.english.empty()) name.english = current;
    return name;
}

struct RecipePart {
    RecipeName name;
    bool maximum = false;
};

struct RecipeRow {
    RecipeName result;
    std::vector<RecipePart> materials;
    std::wstring search;
    RecipeRequirement requirement = RecipeRequirement::None;
};

inline const wchar_t* RecipeRequirementText(RecipeRequirement requirement, bool english) {
    switch (requirement) {
    case RecipeRequirement::Special: return english ? L"Special evolution conditions" : L"特殊进化条件";
    case RecipeRequirement::AnyMaterial: return english ? L"Any one of the listed materials" : L"下列材料任选其一";
    case RecipeRequirement::FiveMaxPassives: return english ? L"Also requires five max-level passives" : L"另需 5 件满级被动";
    case RecipeRequirement::SixMaxEvolutionsAndPassives:
        return english ? L"Six max-level evolutions and six max-level passives" : L"6 件满级进化武器 + 6 件满级被动";
    default: return L"";
    }
}

class RecipeTable {
    std::vector<RecipeRow> rows_;
    std::vector<size_t> filtered_;
    std::vector<int> offsets_{0};
    int position_ = 0, width_ = 0, height_ = 0, columns_ = 2;
    UINT dpi_ = 96;

    int Px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    int Header() const { return Px(30); }
    int MaterialHeight() const { return Px(55); }
    int ResultWidth() const { return std::clamp(width_ * 28 / 100, Px(180), Px(258)); }
    int BodyHeight() const { return std::max(0, height_ - Header()); }
    int Limit() const { return std::max(0, offsets_.back() - BodyHeight()); }
    int RowHeight(const RecipeRow& row) const {
        const int materials = static_cast<int>(row.materials.size());
        const int notes = row.requirement == RecipeRequirement::None ? 0 : Px(21);
        return Px(14) + std::max(MaterialHeight(), MaterialHeight() * ((materials + columns_ - 1) / columns_) + notes);
    }

    void SetScrollBar(HWND pane) const {
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
        info.nMax = std::max(0, offsets_.back() - 1);
        info.nPage = static_cast<UINT>(BodyHeight());
        info.nPos = position_;
        SetScrollInfo(pane, SB_VERT, &info, TRUE);
    }

    void Reflow(HWND pane) {
        offsets_.assign(1, 0);
        offsets_.reserve(filtered_.size() + 1);
        for (auto row : filtered_) offsets_.push_back(offsets_.back() + RowHeight(rows_[row]));
        position_ = std::clamp(position_, 0, Limit());
        SetScrollBar(pane);
        InvalidateRect(pane, nullptr, FALSE);
    }

    std::pair<size_t, size_t> VisibleRange() const {
        if (!BodyHeight() || filtered_.empty()) return {};
        const size_t first = static_cast<size_t>(std::upper_bound(offsets_.begin(), offsets_.end(), position_) - offsets_.begin() - 1);
        const size_t last = static_cast<size_t>(std::lower_bound(offsets_.begin(), offsets_.end(), position_ + BodyHeight()) - offsets_.begin());
        return {first, std::min(last, filtered_.size())};
    }

public:
    void Load(const std::vector<RewardItem>& catalog) {
        std::unordered_map<std::string_view, RecipeName> names;
        for (const auto& item : catalog) names.emplace(item.id, MakeRecipeName(item));
        rows_.clear();
        for (const auto& item : catalog) {
            if (item.recipe.empty() && item.recipe_requirement == RecipeRequirement::None) continue;
            const auto& result = names.at(item.id);
            if (result.chinese.empty() && result.english.empty() && item.reference_only) continue;
            RecipeRow row{result, {}, result.chinese + L" " + result.english, item.recipe_requirement};
            for (const auto& material : item.recipe) {
                const auto found = names.find(material.id);
                RecipeName name = found == names.end() ? RecipeName{} : found->second;
                row.search += L" " + name.chinese + L" " + name.english;
                row.materials.push_back({std::move(name), material.maximum});
            }
            row.search = FoldRewardSearch(row.search);
            rows_.push_back(std::move(row));
        }
        filtered_.clear();
        offsets_.assign(1, 0);
        position_ = 0;
    }

    void Filter(HWND pane, std::wstring_view query) {
        while (!query.empty() && iswspace(query.front())) query.remove_prefix(1);
        while (!query.empty() && iswspace(query.back())) query.remove_suffix(1);
        const auto search = FoldRewardSearch(query);
        filtered_.clear();
        for (size_t i = 0; i < rows_.size(); ++i)
            if (search.empty() || rows_[i].search.find(search) != std::wstring::npos) filtered_.push_back(i);
        position_ = 0;
        Reflow(pane);
    }

    size_t Count() const noexcept { return filtered_.size(); }
    size_t Total() const noexcept { return rows_.size(); }

    void Resize(HWND pane, UINT dpi) {
        RECT rect{}; GetClientRect(pane, &rect);
        const int width = rect.right, height = rect.bottom;
        if (width_ == width && height_ == height && dpi_ == dpi) return;
        width_ = width; height_ = height; dpi_ = dpi;
        columns_ = std::clamp((width_ - ResultWidth() - Px(12)) / Px(166), 1, 4);
        Reflow(pane);
    }

    bool Scroll(HWND pane, int command, int delta = 0) {
        int next = position_;
        switch (command) {
        case SB_LINEUP: next -= Px(42); break;
        case SB_LINEDOWN: next += Px(42); break;
        case SB_PAGEUP: next -= BodyHeight(); break;
        case SB_PAGEDOWN: next += BodyHeight(); break;
        case SB_TOP: next = 0; break;
        case SB_BOTTOM: next = Limit(); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO info{sizeof(info), SIF_TRACKPOS};
            if (GetScrollInfo(pane, SB_VERT, &info)) next = info.nTrackPos;
            break;
        }
        default: next -= MulDiv(delta, Px(72), WHEEL_DELTA); break;
        }
        next = std::clamp(next, 0, Limit());
        if (next == position_) return false;
        position_ = next;
        SetScrollBar(pane);
        InvalidateRect(pane, nullptr, FALSE);
        return true;
    }

    template<class Visit>
    bool ForEachVisibleIcon(Visit visit) const {
        const auto [first, last] = VisibleRange();
        for (size_t index = first; index < last; ++index) {
            const auto& row = rows_[filtered_[index]];
            if (!row.result.id.empty() && visit(row.result.id)) return true;
            for (const auto& part : row.materials)
                if (!part.name.id.empty() && visit(part.name.id)) return true;
        }
        return false;
    }

    void Paint(HDC dc, HFONT font, bool english, bool high_contrast, HIMAGELIST images,
        const std::unordered_map<std::string, int>& icons) const {
        const COLORREF paper = high_contrast ? GetSysColor(COLOR_WINDOW) : ui::surface;
        const COLORREF ink = high_contrast ? GetSysColor(COLOR_WINDOWTEXT) : ui::ink;
        const COLORREF muted = high_contrast ? ink : ui::muted;
        const COLORREF rule = high_contrast ? GetSysColor(COLOR_WINDOWTEXT) : ui::edge;
        ui::Fill(dc, {0, 0, width_, height_}, paper);
        const int saved = SaveDC(dc);
        SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        auto text = [&](const wchar_t* label, RECT rect, COLORREF color, UINT format) {
            SetTextColor(dc, color);
            DrawTextW(dc, label, -1, &rect, format | DT_NOPREFIX);
        };
        const int start = ResultWidth();
        ui::Fill(dc, {0, 0, width_, Header()}, high_contrast ? paper : ui::soft_accent);
        text(english ? L"Result" : L"合成结果", {Px(14), 0, start, Header()}, ink, DT_SINGLELINE | DT_VCENTER);
        text(english ? L"Materials" : L"合成材料", {start + Px(12), 0, width_, Header()}, ink, DT_SINGLELINE | DT_VCENTER);
        ui::Fill(dc, {0, Header() - 1, width_, Header()}, rule);
        IntersectClipRect(dc, 0, Header(), width_, height_);
        const int material_width = (width_ - start - Px(8)) / columns_;
        auto draw_name = [&](const RecipeName& name, int x, int y, int w, bool maximum) {
            auto index = icons.find(name.id);
            if (images && index != icons.end() && index->second >= 0)
                ImageList_Draw(images, index->second, dc, x, y + Px(4), ILD_TRANSPARENT);
            x += Px(31);
            const auto& label = name.Select(english);
            text(label.empty() ? (english ? L"Name unavailable" : L"名称暂不可用") : label.c_str(),
                {x, y, x + w - Px(33), y + Px(33)}, ink, DT_WORDBREAK | DT_END_ELLIPSIS);
            if (maximum) text(english ? L"Max level" : L"满级",
                {x, y + Px(32), x + w - Px(33), y + MaterialHeight()}, muted,
                DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        };
        const auto [first, last] = VisibleRange();
        for (size_t index = first; index < last; ++index) {
            const auto& row = rows_[filtered_[index]];
            const int top = Header() + offsets_[index] - position_;
            const int bottom = Header() + offsets_[index + 1] - position_;
            if ((index & 1) && !high_contrast) ui::Fill(dc, {0, top, width_, bottom}, ui::row_alt);
            ui::Fill(dc, {0, bottom - 1, width_, bottom}, rule);
            draw_name(row.result, Px(14), top + Px(8), start - Px(17), false);
            const int note_height = row.requirement == RecipeRequirement::None ? 0 : Px(21);
            if (note_height) text(RecipeRequirementText(row.requirement, english),
                {start + Px(12), top + Px(8), width_ - Px(6), bottom}, muted, DT_SINGLELINE | DT_END_ELLIPSIS);
            for (size_t i = 0; i < row.materials.size(); ++i) {
                const auto& material = row.materials[i];
                const int column = static_cast<int>(i) % columns_, line = static_cast<int>(i) / columns_;
                draw_name(material.name, start + Px(12) + column * material_width,
                    top + Px(8) + note_height + line * MaterialHeight(), material_width - Px(4), material.maximum);
            }
        }
        RestoreDC(dc, saved);
    }
};

} // namespace vst::client
