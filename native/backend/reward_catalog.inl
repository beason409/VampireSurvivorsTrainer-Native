#pragma once

namespace vst::detail {

template <class Host, class Api>
void RewardBackend<Host, Api>::ResolveLocalization() {
    const void* image = A().FindImage("l2localization.dll");
    if (!image) return;
    localization_class = C(image, "I2.Loc", "LocalizationManager");
    language_source_class = C(image, "I2.Loc", "LanguageSourceData");
    language_class = C(image, "I2.Loc", "LanguageData");
    term_class = C(image, "I2.Loc", "TermData");
    current_language = M(localization_class, "get_CurrentLanguage", "System.String", {}, 100663596, true);
    translation = M(localization_class, "GetTranslation", "System.String",
        {"System.String", "System.Boolean", "System.Int32", "System.Boolean", "System.Boolean", "UnityEngine.GameObject", "System.String", "System.Boolean"},
        100663647, true);
    language_sources = F(localization_class, "Sources", "System.Collections.Generic.List<I2.Loc.LanguageSourceData>", true);
    languages = F(language_source_class, "mLanguages", "System.Collections.Generic.List<I2.Loc.LanguageData>");
    language_name = F(language_class, "Name", "System.String");
    language_code = F(language_class, "Code", "System.String");
    get_term = M(language_source_class, "GetTermData", "I2.Loc.TermData", {"System.String", "System.Boolean"}, 100663520);
    term_languages = F(term_class, "Languages", "System.String[]");
    term_translation = M(term_class, "GetTranslation", "System.String", {"System.Int32", "System.String", "System.Boolean"}, 100663851);
}

template <class Host, class Api>
void RewardBackend<Host, Api>::RefreshEnglish() {
    std::vector<EnglishSlot> found;
    if (!language_sources || !languages || !language_code || !language_name || !get_term || !term_languages
        || !term_translation || !A().array_length) { english_slots.clear(); return; }
    auto english = [](std::string value) {
        for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
        return value == "en" || value.starts_with("en-") || value.starts_with("en_")
            || value == "english" || value.starts_with("english (");
    };
    void* registered = nullptr;
    A().field_static_get_value(language_sources, &registered);
    if (registered) {
        Root sources(A(), registered);
        const int32_t source_count = ListCount(sources.Get(), "System.Collections.Generic.List<I2.Loc.LanguageSourceData>", 256);
        for (int32_t source_index = 0; source_index < source_count; ++source_index) {
            try {
                Root source(A(), ListObject(sources.Get(), "System.Collections.Generic.List<I2.Loc.LanguageSourceData>", "I2.Loc.LanguageSourceData", source_index));
                if (!A().IsA(source.Get(), language_source_class)) continue;
                Root entries(A(), Field<void*>(source.Get(), languages));
                if (!entries.Get()) continue;
                const int32_t count = ListCount(entries.Get(), "System.Collections.Generic.List<I2.Loc.LanguageData>", 512);
                for (int32_t index = 0; index < count; ++index) {
                    Root language(A(), ListObject(entries.Get(), "System.Collections.Generic.List<I2.Loc.LanguageData>", "I2.Loc.LanguageData", index));
                    if (!A().IsA(language.Get(), language_class)) continue;
                    if (english(Text(Field<void*>(language.Get(), language_code))) || english(Text(Field<void*>(language.Get(), language_name)))) {
                        if (found.size() == 256) Fail("The English reward language sources exceed their bound.");
                        found.push_back({source, index});
                    }
                }
            } catch (const std::exception&) {
                // Localization is optional. One inactive source must not hide
                // equipment from other already loaded sources.
            }
        }
    }
    english_slots.swap(found);
}

template <class Host, class Api>
std::string RewardBackend<Host, Api>::LocalName(void* header, int32_t type, bool english) {
    if (!A().IsA(header, header_class)) return {};
    Root term;
    std::string key;
    try {
        void* args[]{&type};
        term = Root(A(), Object(name_term, header, args));
        key = Text(term.Get());
    } catch (const std::exception&) {}
    if (!key.empty() && term.Get()) {
        if (english) {
            for (const auto& candidate : english_slots) {
                const auto value = EnglishName(term.Get(), key, candidate);
                if (!value.empty()) return value;
            }
            return {};
        }
        if (translation) {
            try {
                bool fix_rtl = true, ignore_numbers = true, apply_parameters = false, localized_parameters = true;
                int32_t max_line = 0;
                void* args[]{term.Get(), &fix_rtl, &max_line, &ignore_numbers, &apply_parameters, nullptr, nullptr, &localized_parameters};
                Root translated(A(), Object(translation, nullptr, args));
                std::string value = Text(translated.Get());
                if (!Untranslated(value, key)) {
                    value = Normalize(value);
                    if (!value.empty()) return value;
                }
            } catch (const std::exception&) {}
        }
    }
    if (english) return {};
    std::string value = Text(Field<void*>(header, header_name));
    if (!Untranslated(value, {})) {
        value = Normalize(value);
        if (!value.empty()) return value;
    }
    const int32_t group = Field<int32_t>(header, header_group);
    if (group == group_base || group == group_extra)
        for (const auto& entry : enum_items) if (entry.type == type) return entry.id;
    return {};
}

template <class Host, class Api>
std::string RewardBackend<Host, Api>::EnglishName(void* term, const std::string& key, const EnglishSlot& candidate) {
    try {
        if (!term || key.empty() || !get_term || !term_languages || !term_translation || !A().array_length) return {};
        bool allow_category_mismatch = false;
        void* term_args[]{term, &allow_category_mismatch};
        Root data(A(), Object(get_term, candidate.source.Get(), term_args));
        if (!A().IsA(data.Get(), term_class)) return {};
        Root values(A(), Field<void*>(data.Get(), term_languages));
        if (!values.Get() || candidate.index < 0) return {};
        const uint32_t length = A().array_length(values.Get());
        if (length > 10000 || static_cast<uint32_t>(candidate.index) >= length) return {};
        int32_t index = candidate.index;
        bool edit_mode = false;
        void* args[]{&index, nullptr, &edit_mode};
        Root translated(A(), Object(term_translation, data.Get(), args));
        std::string value = Text(translated.Get());
        return Untranslated(value, key) ? std::string{} : Normalize(value);
    } catch (const std::exception&) { return {}; }
}

template <class Host, class Api>
void* RewardBackend<Host, Api>::DataSource() {
    uintptr_t owner = 0; void* manager = nullptr;
    if (Context(owner, manager)) {
        Root current(A(), manager);
        void* data = Object(manager_data, current.Get());
        if (A().IsA(data, data_class)) return data;
        Fail("The current run's reward data is not ready.");
    }
    void* options = nullptr;
    if (!h.MenuContext(nullptr, options)) Fail("Wait for the single-player menu or run before reading equipment.");
    Root checked_options(A(), options);
    const void* type = A().class_get_type(h.menu_class);
    Root managed_type(A(), type ? A().type_get_object(type) : nullptr);
    if (!managed_type.Get()) Fail("The native menu type is unavailable.");
    bool include_inactive = true;
    void* args[]{managed_type.Get(), &include_inactive};
    Root page(A(), Object(h.find_menu_method, nullptr, args));
    if (!A().IsA(page.Get(), h.menu_class) || Object(h.menu_options_method, page.Get()) != checked_options.Get())
        Fail("The single-player menu changed while reading equipment.");
    void* data = Field<void*>(page.Get(), menu_data);
    if (!A().IsA(data, data_class)) Fail("The menu's reward data is still loading.");
    return data;
}

template <class Host, class Api>
bool RewardBackend<Host, Api>::RefreshCatalog() {
    if (!catalog_ready || enum_items.size() > 4096) Fail("The native equipment catalog is unavailable.");
    Root data(A(), DataSource());
    Root source(A(), Object(converted, data.Get()));
    const int32_t count = DictCount(source.Get(), kConverted);
    if (!count) Fail("The game's equipment catalog is still loading.");
    std::string language;
    try { if (current_language) language = Text(Object(current_language, nullptr)); } catch (const std::exception&) {}
    const uint64_t now = GetTickCount64();
    const bool cached = catalog_owner.Get() == data.Get() && catalog_count == count && catalog_language == language && !catalog.empty();
    if (cached && (catalog_english > 0 || now < catalog_retry_at)) return true;
    if (catalog_build && catalog_build->owner.Get() == data.Get() && catalog_build->source.Get() == source.Get()
        && catalog_build->count == count && catalog_build->language == language) return cached;
    if (now < catalog_retry_at && !catalog_error.empty()) {
        if (cached) return true;
        Fail(catalog_error.c_str());
    }
    auto build = std::make_shared<CatalogBuild>();
    build->owner = std::move(data); build->source = std::move(source);
    build->count = count; build->language = std::move(language);
    build->destruction = h.destructions.load(std::memory_order_acquire);
    build->actor = h.local_player.load(std::memory_order_acquire); build->scene = h.scene;
    catalog_build = std::move(build); catalog_error.clear();
    return cached;
}

template <class Host, class Api>
void RewardBackend<Host, Api>::AdvanceCatalog() noexcept {
    auto build = catalog_build;
    if (!build) return;
    const uint64_t started = GetTickCount64();
    const auto slice_started = std::chrono::steady_clock::now();
    auto current = [&] {
        return catalog_build == build && !h.stopping.load(std::memory_order_acquire)
            && h.unity_thread.load() == GetCurrentThreadId()
            && h.destructions.load(std::memory_order_acquire) == build->destruction
            && h.local_player.load(std::memory_order_acquire) == build->actor && h.scene == build->scene;
    };
    try {
        const auto state = h.runtime.Snapshot(started);
        // paused_known is suppressed by Runtime after its existing one-second
        // observation deadline, including in the playerless main menu.
        if (!current() || !state.verified || !state.safe_mode || !state.paused_known || state.scene != build->scene) {
            if (catalog_build == build) catalog_build.reset();
            return;
        }
        using Stage = typename CatalogBuild::Stage;
        auto next_item = [&] {
            build->levels = {}; build->header = {}; build->term = {};
            build->term_key.clear(); build->item = {}; build->stage = Stage::Entry;
        };
        auto append_item = [&] { build->items.push_back(std::move(build->item)); next_item(); };
        auto english_key = [](std::string value) {
            for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
            return value == "en" || value.starts_with("en-") || value.starts_with("en_")
                || value == "english" || value.starts_with("english (");
        };
        // Each step performs at most a small fixed group of native reads.
        // Level lists and English-source candidates never loop inside a step.
        for (unsigned steps = 0; steps < 32 && current(); ++steps) {
            if (steps && std::chrono::steady_clock::now() - slice_started >= std::chrono::milliseconds(2)) break;
            try {
                switch (build->stage) {
                case Stage::Sources: {
                    build->stage = Stage::Entry;
                    if (!language_sources || !languages || !language_code || !language_name || !get_term || !term_languages
                        || !term_translation || !A().array_length) break;
                    void* sources = nullptr; A().field_static_get_value(language_sources, &sources);
                    build->sources = Root(A(), sources);
                    if (!sources) break;
                    build->source_count = ListCount(sources, "System.Collections.Generic.List<I2.Loc.LanguageSourceData>", 256);
                    build->stage = Stage::Source; break;
                }
                case Stage::Source: {
                    if (build->source_index >= build->source_count) { build->stage = Stage::Entry; break; }
                    build->language_source = Root(A(), ListObject(build->sources.Get(),
                        "System.Collections.Generic.List<I2.Loc.LanguageSourceData>", "I2.Loc.LanguageSourceData", build->source_index++));
                    if (!A().IsA(build->language_source.Get(), language_source_class)) break;
                    build->language_entries = Root(A(), Field<void*>(build->language_source.Get(), languages));
                    if (!build->language_entries.Get()) break;
                    build->language_count = ListCount(build->language_entries.Get(), "System.Collections.Generic.List<I2.Loc.LanguageData>", 512);
                    build->language_index = 0; build->stage = Stage::Language; break;
                }
                case Stage::Language: {
                    if (build->language_index >= build->language_count) { build->stage = Stage::Source; break; }
                    Root language(A(), ListObject(build->language_entries.Get(), "System.Collections.Generic.List<I2.Loc.LanguageData>",
                        "I2.Loc.LanguageData", build->language_index++));
                    if (!A().IsA(language.Get(), language_class)) break;
                    if (english_key(Text(Field<void*>(language.Get(), language_code))) || english_key(Text(Field<void*>(language.Get(), language_name)))) {
                        if (build->english.size() >= 256) Fail("The English reward language sources exceed their bound.");
                        build->english.push_back({build->language_source, build->language_index - 1});
                    }
                    break;
                }
                case Stage::Entry: {
                    if (build->enum_index >= enum_items.size()) { build->stage = Stage::VerifyOwner; break; }
                    const auto& entry = enum_items[build->enum_index++];
                    if (!Named(entry.id)) break;
                    build->item.item.id = entry.id; build->item.type = entry.type;
                    build->levels = Root(A(), DictObject(build->source.Get(), kConverted, kDataList, entry.type));
                    if (!build->levels.Get()) { next_item(); break; }
                    build->length = ListCount(build->levels.Get(), kDataList, 1000);
                    build->total_levels += static_cast<size_t>(build->length);
                    if (build->total_levels > 32768) Fail("The native equipment levels exceed their bound.");
                    build->level_index = build->highest = 0; build->header = {};
                    build->stage = Stage::Level; break;
                }
                case Stage::Level: {
                    if (build->level_index >= build->length) { build->stage = Stage::Details; break; }
                    Root level(A(), ListObject(build->levels.Get(), kDataList, kWeaponData, build->level_index++));
                    if (A().IsA(level.Get(), header_class)) {
                        if (!build->header.Get()) build->header = level;
                        build->highest = std::max(build->highest, Field<int32_t>(level.Get(), header_level));
                    }
                    break;
                }
                case Stage::Details: {
                    if (!build->header.Get()) { next_item(); break; }
                    void* header = build->header.Get();
                    auto& item = build->item.item;
                    item.is_evolution = Field<bool>(header, header_evolution);
                    if (Field<bool>(header, header_always_hidden)
                        || (!item.is_evolution && (Field<bool>(header, header_hidden) || Field<bool>(header, header_special)))
                        || Normalize(Text(Field<void*>(header, header_frame))).empty()) { next_item(); break; }
                    int32_t group = Field<int32_t>(header, header_group);
                    if (group != group_base && group != group_extra) {
                        void* args[]{&group}; if (!Scalar<bool>(dlc_loaded, nullptr, args)) { next_item(); break; }
                    }
                    item.name = LocalName(header, build->item.type, false);
                    if (item.name.empty()) { next_item(); break; }
                    item.kind = Field<bool>(header, header_passive) ? "passive" : "weapon";
                    item.max_level = std::clamp(std::max(build->highest, build->length), 1, 1000);
                    item.upgrade_allowed = !item.is_evolution || item.kind != "passive";
                    if (!item.upgrade_allowed) item.upgrade_unsupported_reason = "Evolved passive upgrades are not supported; chest rewards remain available.";
                    try {
                        void* args[]{&build->item.type}; build->term = Root(A(), Object(name_term, header, args));
                        build->term_key = Text(build->term.Get());
                    } catch (const std::exception&) { build->term = {}; build->term_key.clear(); }
                    build->english_index = 0; build->stage = Stage::English; break;
                }
                case Stage::English: {
                    if (build->term_key.empty() || !build->term.Get() || build->english_index >= build->english.size()) { append_item(); break; }
                    auto translated = EnglishName(build->term.Get(), build->term_key, build->english[build->english_index++]);
                    if (!translated.empty()) { build->item.item.english_name = std::move(translated); append_item(); }
                    break;
                }
                case Stage::VerifyOwner: {
                    Root owner(A(), DataSource());
                    if (owner.Get() != build->owner.Get()) Fail("The equipment data owner changed while loading.");
                    build->stage = Stage::VerifySource; break;
                }
                case Stage::VerifySource: {
                    Root source(A(), Object(converted, build->owner.Get()));
                    if (source.Get() != build->source.Get() || DictCount(source.Get(), kConverted) != build->count)
                        Fail("The equipment source changed while loading.");
                    build->stage = Stage::VerifyLanguage; break;
                }
                case Stage::VerifyLanguage: {
                    std::string language;
                    if (current_language) language = Text(Object(current_language, nullptr));
                    if (language != build->language) Fail("The game's language changed while loading equipment.");
                    build->stage = Stage::Commit; break;
                }
                case Stage::Commit: {
                    if (build->items.empty()) Fail("No loaded weapons or passive items are available yet.");
                    std::sort(build->items.begin(), build->items.end(), [](const CatalogItem& left, const CatalogItem& right) {
                        if (left.item.kind != right.item.kind) return left.item.kind < right.item.kind;
                        if (left.item.name != right.item.name) return left.item.name < right.item.name;
                        return left.item.id < right.item.id;
                    });
                    std::unordered_map<std::string, size_t> index; size_t english = 0;
                    for (size_t item = 0; item < build->items.size(); ++item) {
                        index.emplace(build->items[item].item.id, item);
                        if (!build->items[item].item.english_name.empty()) ++english;
                    }
                    if (!current()) return;
                    catalog.swap(build->items); by_id.swap(index); english_slots.swap(build->english);
                    icons.clear();
                    catalog_owner = build->owner; catalog_count = build->count; catalog_language = build->language; catalog_english = english;
                    const uint64_t now = GetTickCount64();
                    catalog_retry_at = now <= UINT64_MAX - 1000 ? now + 1000 : UINT64_MAX;
                    catalog_error.clear(); catalog_build.reset(); return;
                }
                }
            } catch (const std::exception&) {
                if (build->stage == Stage::Sources || build->stage == Stage::Source || build->stage == Stage::Language) {
                    build->english.clear(); build->stage = Stage::Entry;
                } else if (build->stage == Stage::Entry || build->stage == Stage::Level || build->stage == Stage::Details || build->stage == Stage::English) {
                    if (build->total_levels > 32768) throw;
                    next_item();
                } else throw;
            }
        }
        if (!current() && catalog_build == build) catalog_build.reset();
    } catch (const std::exception& error) {
        if (catalog_build == build) {
            try { catalog_error = error.what(); } catch (...) {}
            catalog_build.reset();
            const uint64_t now = GetTickCount64(); catalog_retry_at = now <= UINT64_MAX - 1000 ? now + 1000 : UINT64_MAX;
        }
    } catch (...) {
        if (catalog_build == build) {
            try { catalog_error = "The incremental native equipment read failed."; } catch (...) {}
            catalog_build.reset();
        }
    }
}

template <class Host, class Api>
auto RewardBackend<Host, Api>::Validate(const std::string& id, bool upgrade, bool run_required) -> CatalogItem& {
    if (!Named(id)) Fail("Select a named, loaded equipment item.");
    if (!RefreshCatalog()) Fail("The equipment catalog is loading. Try again shortly.");
    const auto found = by_id.find(id);
    if (found == by_id.end()) Fail("The selected equipment is unavailable; refresh the catalog.");
    auto& target = catalog[found->second];
    if (upgrade && !target.item.upgrade_allowed) Fail("Evolved passive items cannot be forced into an upgrade choice.");
    if (!run_required) return target;
    uintptr_t owner = 0; void* manager = nullptr;
    const uint64_t epoch = h.runtime.RewardEpoch();
    if (!Context(owner, manager, epoch)) Fail("Enter a ready single-player run before enabling specified rewards.");
    Root current(A(), Equipment(target.type));
    if (current.Get() && Scalar<bool>(equipment_max, current.Get())) Fail("The selected equipment is already at its maximum level.");
    if (upgrade) {
        Root data(A(), Object(manager_data, manager));
        if (!A().IsA(data.Get(), data_class)) Fail("The current run's upgrade data is unavailable.");
        Root source(A(), Object(converted, data.Get()));
        Root levels(A(), DictObject(source.Get(), kConverted, kDataList, target.type));
        if (!levels.Get() || !ListCount(levels.Get(), kDataList, 1000)) Fail("The selected item has no native upgrade levels.");
        Root header(A(), ListObject(levels.Get(), kDataList, kWeaponData, 0));
        if (!A().IsA(header.Get(), header_class)) Fail("The selected item has no valid native upgrade header.");
        ValidateEvolution(header.Get(), target.type, data.Get());
    }
    if (!Current(owner, epoch)) Fail("The local reward context changed during validation.");
    return target;
}

template <class Host, class Api>
void* RewardBackend<Host, Api>::Equipment(int32_t type) {
    uintptr_t owner = 0; void* manager = nullptr;
    const uint64_t epoch = h.runtime.RewardEpoch();
    if (!Context(owner, manager, epoch)) Fail("The local equipment owner is unavailable.");
    Root player(A(), reinterpret_cast<void*>(owner));
    Root weapons(A(), Object(h.weapons_method, player.Get()));
    Root passives(A(), Object(accessories, player.Get()));
    for (const auto& family : {std::pair<void*, const void*>{weapons.Get(), get_weapon}, {passives.Get(), get_accessory}}) {
        if (!family.first) continue;
        for (bool hidden : {false, true}) {
            void* args[]{&type, &hidden};
            Root equipment(A(), Object(family.second, family.first, args));
            if (!equipment.Get()) continue;
            if (!A().IsA(equipment.Get(), h.equipment_class)
                || Field<void*>(equipment.Get(), h.equipment_owner_field) != player.Get()
                || Field<int32_t>(equipment.Get(), h.equipment_type_field) != type || !Current(owner, epoch))
                Fail("The returned equipment does not belong to the current local character.");
            return equipment.Get();
        }
    }
    if (!Current(owner, epoch)) Fail("The local equipment owner changed during lookup.");
    return nullptr;
}

template <class Host, class Api>
void RewardBackend<Host, Api>::ValidateEvolution(void* header, int32_t type, void* data) {
    if (!A().IsA(header, header_class) || !A().IsA(data, data_class)) Fail("The native evolution data is unavailable.");
    if (!Field<bool>(header, header_evolution)) return;
    if (Field<bool>(header, header_passive)) Fail("Evolved passive upgrades are not supported.");
    uintptr_t owner = 0; void* manager = nullptr;
    const uint64_t epoch = h.runtime.RewardEpoch();
    if (!Context(owner, manager, epoch)) Fail("The single-player evolution context is unavailable.");
    Root original_data(A(), Object(raw, data));
    if (!original_data.Get() || !DictCount(original_data.Get(), kRaw)) Fail("The raw evolution catalog is unavailable.");
    Root levels(A(), DictObject(original_data.Get(), kRaw, "Newtonsoft.Json.Linq.JArray", type));
    if (!levels.Get()) Fail("The selected evolution has no raw level data.");
    Root current(A(), Equipment(type));
    int32_t index = current.Get() ? Scalar<int32_t>(equipment_level, current.Get()) : 0;
    if (index < 0 || index > 10000) Fail("The current evolution level exceeds its bound.");
    void* next = nullptr;
    void* level_args[]{levels.Get(), &index, &next};
    if (!Scalar<bool>(data_for_level, nullptr, level_args)) Fail("The game cannot parse the next evolution level.");
    Root next_data(A(), next);
    if (!A().IsA(next_data.Get(), header_class)) Fail("The next evolution level has no native equipment data.");
    Root facade(A(), Object(weapons_facade, manager));
    if (!facade.Get()) Fail("The loaded weapon facade is unavailable.");
    Root factory(A(), Field<void*>(facade.Get(), facade_factory));
    if (!A().IsA(factory.Get(), weapon_factory_class)) Fail("The loaded weapon factory is unavailable.");
    int32_t actual_type = ~type;
    void* prefab_args[]{&type, &actual_type};
    Root prefab(A(), Object(weapon_prefab, factory.Get(), prefab_args));
    if (!A().IsA(prefab.Get(), h.weapon_class) || actual_type != type) Fail("The selected evolution has no exact loaded weapon prefab.");
    if (!Current(owner, epoch)) Fail("The evolution context changed while reading loaded data.");
}

} // namespace vst::detail
