# Native specified-reward routes

Target: Steam Build `25984009`, game `1.16.111`, Unity `6000.0.62f1`, Windows x64. This is an ABI and migration audit, not a live acceptance record. Existing local actor, owned-equipment, main-thread, session, freshness and file-identity protection remains required.

## Evidence and ABI boundary

Read-only Mono.Cecil inspection of the generated runtime and mscorlib wrappers verified the declarations below. Game tokens come from wrapper static initializers calling `GetIl2CppMethodByToken`; wrapper method-definition tokens are not used. Generated field accessors establish exact native field names and scalar/reference types. They do not expose complete native method bodies or prove that a declared native route is executed in every gameplay mode.

`TreasurePrizeTypePair`, `Treasure`, `WeaponData`, factories and UI pages are reference classes. In particular, the prize pair is **not** a C++ value struct or a hidden structure-return argument. `WeaponType`, `PrizeType`, `ItemType` and `ContentGroupType` have `Int32` enum storage. Generic enumerators and dictionary entries are IL2CPP value types even though generated CLR wrappers can inherit the reference-shaped `Il2CppSystem.ValueType`; CLR `IsValueType` alone cannot establish their native ABI.

## Chest generation and native claim

| Declaring type | Instance method | Return / explicit parameters | Game token |
| --- | --- | --- | ---: |
| `VampireSurvivors.TreasureFactory` | `GenerateNewPrizes` | `List<TreasurePrizeTypePair> (Data.Stage.Treasure)` | 100663781 |
| `TreasureFactory` | `MakePrizePairFromAvailablePowerUps` | `TreasurePrizeTypePair (PrizeType, WeaponType, CharacterController, bool)` | 100663782 |
| `VampireSurvivors.Framework.LevelUpFactory` | `PullExisting` | `WeaponType (WeaponType)` | 100695604 |
| `VampireSurvivors.Data.Stage.Treasure` | `ClaimPrizes` | `void (CharacterController, bool)` | 100732031 |

Native `TreasurePrizeTypePair` fields are `prizeType: PrizeType`, `prizeItem: ItemType`, `prizeWeapon: WeaponType`, and `Level: Int32`. `Level` is a field, not a native `get_Level` method. Treasure fields are `openingPlayer: CharacterController`, `prizes: List<TreasurePrizeTypePair>`, and `accumulatedWeaponPrizes: List<WeaponType>`.

The current `PrizeType` values are `POWERUP=0`, `EXISTING_WEAPON=1`, `EXISTING_ANY=2`, `FILLER=3`, `RANDOM=4`, `FIXED=5`, `EVOLUTION=6`, `EVO_ARCANA=7`, `NEW_WEAPON=8`, and `SURVAROT=9`. Resolve and verify the exact enum rather than relying only on these numbers.

The managed route in [ChestHooks.cs](../../src/Trainer.Plugin/ChestHooks.cs) supplies the behavioral contract:

1. Every generation invocation saves and clears both thread-local generation/pair contexts, including unrelated nested calls. Eligibility requires the current local opening character, factory, current target and safe run.
2. Replace at most one eligible pair by passing `FIXED` and the target into the original native pair factory. Do not replace Arcana or Survarot card rewards and do not construct or patch a display-only pair afterward.
3. Override `PullExisting` only inside that exact forced-pair invocation, for that target. The legacy route avoids the native FIXED fallback selecting a random currently owned weapon when the requested target is not in its store.
4. After generation, verify both the native pair and `accumulatedWeaponPrizes`. Keep strong references to the treasure and its exact prizes list for pending claim observation. A reused treasure template supersedes an unclaimed generation; an already claiming entry must not be replaced. Bound the pending map at 256 entries.
5. Let original `ClaimPrizes` perform acquisition/leveling and rewards. Count an actual grant only when the target equipment newly exists, changes instance or increases level. A generation receipt is not a grant receipt. Never compensate a failed claim with an extra grant.

An already generated chest can retain its native prize after a later control change. Disabling affects future forced generation; it must not roll back already acquired equipment or silently rewrite a pending native claim.

### Five-reward chests

In the pinned build, the local treasure screen path assigns the opening character and calls `GenerateNewPrizes` before the open-treasure page caches the tier. `TreasureFactory.MakePrizes` (100663783) reads the treasure tier and calls the native pair factory once, three times or five times for tiers 1, 2 and 3. The factory checks every slot index against the number of present entries in `List<Nullable<PrizeType>>`, so setting the tier alone would be unsafe for short templates.

The independent toggle selects tier 3 for ordinary local chests. It preserves their existing prize kinds and fixed weapon list. For a short template it builds a temporary list padded with the game's `EXISTING_ANY` prize kind, then restores the exact original list after generation. The game decides which weapons can evolve or level up and uses its normal filler rewards when applicable. Arcana and Survarot card routes are excluded. The native UI reads the promoted tier both when caching the treasure and when starting playback. Pending tiers are tracked by the exact rooted chest and restored after its native claim, at same-object reuse, or at teardown. Generation of another chest leaves earlier pending tiers intact. The count records confirmed five-entry native generation. The latest equipment count reflects the generated weapon and passive entries among those five slots, before claim. The existing animation skip and targeted reward controls are independent.

## Ordinary upgrade page

| Declaring type | Instance method | Return / explicit parameters | Game token |
| --- | --- | --- | ---: |
| `VampireSurvivors.UI.LevelUpPage` | `Populate` | `void ()` | 100669619 |
| `LevelUpPage` | `PickRandomLevelUps` | `void ()` | 100669621 |
| `LevelUpPage` | `ResetLevelUpViewsAfterReRoll` | `void ()` | 100669622 |
| `LevelUpPage` | `SpawnWeapon` | `void (WeaponData, WeaponType, int)` | 100669629 |
| `LevelUpPage` | `SelectWeapon` | `void (WeaponType, LevelUpItemUI)` | 100669571 |
| `LevelUpPage` | `GetCharacterControllingUi` | `CharacterController ()` | 100669606 |
| `LevelUpPage` | `get_LevelUpItems` | `List<LevelUpItemUI> ()` | 100669546 |
| `Framework.LevelUpFactory` | `HasPowerupsInStore` | `bool (CharacterController)` | 100695598 |
| `Framework.LevelUpFactory` | `FindFriendshipAmuletTargets` | `List<CharacterController> (bool)` | 100695630 |
| `VampireSurvivors.LevelUpItemUI` | `get_Index` | `int ()` | 100666046 |
| `LevelUpItemUI` | `GetWeaponType` | `WeaponType ()` | 100666057 |

Page native fields: `_currentWeapons: List<WeaponType>`, `_weaponData: Dictionary<WeaponType,List<WeaponData>>`, `_data: DataManager`, `_levelUpFactory: Framework.LevelUpFactory`, `_hasSelected: bool`, `_isBanishMode: bool`. `LevelUpItems` requires its getter; there is no corresponding same-name native field. Item UI fields include `_data: WeaponData`, `_index: int`, and `_isLimitBreak: bool`.

[UpgradeRewardHooks.cs](../../src/Trainer.Plugin/UpgradeRewardHooks.cs) preserves distinct initial, reroll, population and selection scopes. Important ordering and limitations:

- Observe the game's own `HasPowerupsInStore` call inside `Populate`. Do not call it to validate a target: the legacy audit identifies weight updates and lucky-gift RNG side effects. A false availability result leaves the coin, food or limit-break menu unchanged and disables the upgrade override.
- Initial selection changes the first candidate after `FindFriendshipAmuletTargets(true)` returns, before original enumeration. At that point the list is assigned/shuffled and the optional friendship slot has not removed its tail. A postfix on `PickRandomLevelUps` is too late for correct native buttons.
- The reroll path changes the retained candidates before `ResetLevelUpViewsAfterReRoll`. Validate count `1..32`, the exact page/factory/actor, and live source data. If the target already occurs once later in the list, swap it with the first candidate; reject duplicates. Change no more than one candidate otherwise.
- Verify `SpawnWeapon` produces exactly one target button, with matching target data, index, weapon ID and non-limit-break binding. Keep the exact page/list/button/actor/manager identities; bound offer tracking at 32 pages.
- `SelectWeapon` stays original. Choosing another item, skip, banish or close must not grant the target. Banish mode and already-selected pages cannot count as ordinary acquisition. Record offered choices separately from confirmed equipment change.

Nested unsupported calls must clear their inner context and restore the outer context afterward. Root retained references; do not retain unrooted page, list, prefab or data pointers across frames. Native exception/unwind behavior needs separate verification: a C++ scope guard is not by itself proof of equivalent Harmony finalizer coverage.

## Catalog, ownership and evolution

| Declaring type | Method | Return / parameters | Game token |
| --- | --- | --- | ---: |
| `Framework.GameManager` | `get_DataManager` | `DataManager ()` | 100694957 |
| `Framework.GameManager` | `get_WeaponsFacade` | `Framework.WeaponsFacade ()` | 100694971 |
| `Data.DataManager` | `GetConvertedWeapons` | `Dictionary<WeaponType,List<WeaponData>> ()` | 100731220 |
| `Data.DataManager` | `get_AllWeaponData` | `Dictionary<WeaponType,Newtonsoft.Json.Linq.JArray> ()` | 100731144 |
| `Tools.DataHelper` | static `GetWeaponDataForLevel` | `bool (JArray, int, out WeaponData)` | 100666712 |
| `Framework.WeaponFactory` | `GetWeaponPrefab` | `Weapon (WeaponType, out WeaponType)` | 100695918 |
| `Data.ContentGroupMethods` | static `IsDlcLoadedForContentGroup` | `bool (ContentGroupType)` | 100731768 |
| `CharacterController` | `get_WeaponsManager` | `CharacterWeaponsManager ()` | 100689533 |
| `CharacterController` | `get_AccessoriesManager` | `CharacterAccessoriesManager ()` | 100689534 |
| `CharacterWeaponsManager` | `GetWeaponByType` | `Weapon (WeaponType, bool searchHidden)` | 100672065 |
| `CharacterAccessoriesManager` | `GetAccessoryByType` | `Accessory (WeaponType, bool searchHidden)` | 100672055 |
| `Equipment` | `get_Level` / `IsMaxLevel` | `int ()` / `bool ()` | 100672029 / 100672048 |

`WeaponsFacade._weaponFactory` is a `Framework.WeaponFactory` reference. Active and hidden equipment must be searched separately with `false` and `true`; one is not a recursive superset of the other. Removed/evolved-away base equipment is not currently held equipment.

WeaponData's native fields use backing names: `<hidden>k__BackingField`, `<alwaysHidden>k__BackingField`, `<level>k__BackingField`, `<name>k__BackingField`, `<texture>k__BackingField`, `<frameName>k__BackingField`, `<isEvolution>k__BackingField`, `<isSpecialOnly>k__BackingField`, `<evolvesFrom>k__BackingField`, `<isPowerUp>k__BackingField`, `<contentGroup>k__BackingField`. Boolean fields are Boolean, `level` is Int32, names/texture keys are String, `evolvesFrom` is `List<WeaponType>`, and content group is its Int32 enum. Read offsets from current metadata, not from a fixed object layout.

The current WeaponType metadata contains 688 named, distinct literal values. Enumerating those names and querying the live converted dictionary avoids dictionary enumerator/value-struct ABI entirely. Accept only exact named IDs of at most 128 letters/digits/underscores; reject numeric enum input and `VOID`, `CANDYBOX`, `CANDYBOX2`, `ARMADIO`.

Retain [RewardCatalog.cs](../../src/Trainer.Plugin/RewardCatalog.cs) filters: skip empty level lists, `alwaysHidden`, hidden/special-only non-evolutions and missing frame keys. BASE and EXTRA are free content (`0`, `1`) and bypass the DLC helper; an absent helper mapping does not mean base content is unloaded. Other groups require the game's loaded-content result. Unlock, slot and banish state do not hide valid cheat targets, but max-level ownership is checked again before activation/generation.

For an evolved upgrade target, reject evolved passives. `AllWeaponData` is raw **JArray**, not the converted WeaponData list. Validate the raw next level with `GetWeaponDataForLevel` and the loaded prefab with `GetWeaponPrefab`, requiring the exact returned actual type. Read bounded `evolvesFrom` (at most 64) only for receipt evidence; original selection performs actual base removal and evolution. Do not manually spawn a prefab, remove a base weapon, bypass DLC loading, or grant a second item if original evolution fails.

### Read-only weapon recipes

The separate Recipes tab reads `evolvesFrom`, `requires` and `requiresMax` as verified `List<WeaponType>` fields from the same loaded equipment data. It joins and deduplicates materials by equipment type. Earlier steps appear as materials for later evolutions. References can retain catalog items for display and icon lookup without making them eligible for reward targeting. The local base and DLC asset audit found 158 declarative recipes with all material IDs present in the combined game data. The number available in the app depends on the content the game loads.

The pinned build's unique checks supply the alternate Penshin materials and the max-level equipment requirements for Rings of Calamity and Alucard Shield. Script-driven conditions without a fixed material list are labeled as special in the table. Chinese and English names come from the game's localization sources without changing its language. The layout displays names in both searchable languages and keeps internal identifiers as join and icon lookup keys. When a translation is unavailable it uses a readable game name from the other language.

The catalog scan remains sliced on the game thread. The table draws visible rows and shares the reward icon cache, with requests paused during scrolling. The standalone executable contains no copied game sprites or asset bundles.

Catalog generation can make hundreds of native calls. Cache by verified data owner/count/language and build with a bounded main-thread work budget; UI search should operate on the resulting client cache. Names come from loaded game data, not a bundled game catalog. English names use the real I2 language-source English slot without changing or saving CurrentLanguage. A fixed string override of `English` is not equivalent to the legacy implementation.

Read-only inspection of the generated `l2localization` wrapper established these localization routes:

| Declaring type | Method | Return / full parameters | Game token |
| --- | --- | --- | ---: |
| `I2.Loc.LocalizationManager` | static `get_CurrentLanguage` | `string ()` | 100663596 |
| `LocalizationManager` | static `GetTranslation` | `string (string term, bool fixRTL=true, int maxRTLLength=0, bool ignoreRTLNumbers=true, bool applyParameters=false, GameObject root=null, string overrideLanguage=null, bool localizedParameters=true)` | 100663647 |
| `I2.Loc.LanguageSourceData` | `GetTermData` | `TermData (string term, bool allowCategoryMistmatch=false)` | 100663520 |
| `I2.Loc.TermData` | `GetTranslation` | `string (int index, string specialization=null, bool editMode=false)` | 100663851 |

Native fields: static `LocalizationManager.Sources: List<LanguageSourceData>`; instance `LanguageSourceData.mLanguages: List<LanguageData>`; instance `LanguageData.Name` and `Code: String`; instance `TermData.Languages: String[]`. These are plain field names, not backing-field guesses. Scan actual English code/name slots and bounds-check against Languages before reading their translation. Missing English data remains unknown and a later catalog refresh retries. C# optional parameters are real native parameters: the one-argument source-level calls are not one-argument ABI signatures.

## Generic collections and marshaling

The mscorlib definition tokens are:

| Definition | Method | Token |
| --- | --- | ---: |
| `List<T>` | `get_Count()` | 100675784 |
| `List<T>` | `get_Item(int)` | 100675790 |
| `List<T>` | `set_Item(int,T)` | 100675791 |
| `List<T>` | `Contains(T)` | 100675804 |
| `Dictionary<TKey,TValue>` | `get_Count()` | 100675641 |
| `Dictionary<TKey,TValue>` | `get_Item(TKey)` | 100675648 |
| `Dictionary<TKey,TValue>` | `ContainsKey(TKey)` | 100675655 |
| `Dictionary<TKey,TValue>` | `TryGetValue(TKey,out TValue)` | 100675669 |

Definition signatures contain `VAR` for T/TKey/TValue. Only an independently verified closed owner/element family and exact token may permit that generic parameter in method validation; do not globally accept arbitrary VAR methods. `List._items` is an array of its actual element type, `_size` and `_version` are Int32. Dictionary `_entries` elements contain `hashCode`, `next`, `key`, `value`, but their storage, free entries, strides and generic value sizes must not be inferred from CLR wrapper layout. Avoid directly calling `GetEnumerator` or returning KeyValuePair/Enumerator through a guessed C++ signature.

For `runtime_invoke`, by-value managed references are raw object pointers, scalar/enum arguments point to their storage, and an out reference points to a writable object-pointer variable. In particular, `GetWeaponDataForLevel` receives the JArray pointer directly and `&outWeaponData`; `GetWeaponPrefab` receives `&weaponType` and `&outActualType`. `List<WeaponType>.set_Item` receives `&index` and `&enumValue`; enum returns from invocation are boxed and must be unboxed as Int32. Root reference results before further native calls.

## Icon boundary

The legacy encoder in [RewardIcons.cs](../../src/Trainer.Plugin/RewardIcons.cs) is more than a raw texture read. It obtains the atlas frame through `Graphics.SpriteManager.GetSprite(string spriteName,string textureName,bool ignoreExtension=true)`, token `100666971`. The C# call with two arguments relies on a compiler-supplied third argument; there is no corresponding two-String native overload.

It validates `Sprite.textureRect`, packing rotation and bounds; uses a temporary GPU target for non-readable atlases; preserves/restores `RenderTexture.active` and `GL.sRGBWrite`; releases its own RenderTexture/Texture2D; never destroys shared game sprites or textures. Vector2/Rect are value-type marshaling boundaries. Preserve the 32-pixel, aspect-preserving, point-filtered icon, 32768-byte PNG cap, bounded cache and normal missing-picture fallback. An unavailable icon must not disable an unrelated effect or reward target. No game art is added to the distribution. These Unity image routes still need their own verified tokens/value layouts before native capability is advertised.

## Pending acceptance matrix

| Area | Required observed result |
| --- | --- |
| Catalog | Base/free and loaded DLC only; normal/evolved flags and localized names; safe empty/loading state; English slot without changing game language. |
| Chest | Unowned normal weapon, existing weapon upgrade, passive, multi-prize chest with exactly one forced eligible pair; original accumulated list and actual claim agree. |
| Chest scope | Arcana/Survarot unchanged; repeated treasure/template reuse; nested unrelated generation; repeated claim and unsupported/full-level target fail without extra grant. |
| Upgrade | Initial page and reroll each bind one target candidate/button; already-offered target stays unique; selection produces one native acquisition/level increase. |
| Upgrade exclusions | Another choice, skip, banish and close do not grant target; coin/food/limit-break fallback remains native; standalone manual selection pages remain unsupported. |
| Evolution | Valid evolved weapon uses raw next-level data and loaded exact prefab; native base removal is observed; evolved passive, missing data/prefab or unloaded content rejects. |
| Lifecycle | Off/target change/session replacement, 1.5-second queued expiry, 1-second observation freshness, 3-second heartbeat, actor/scene/teardown and revival waiting never revive old forced scopes or cause late grants. |
| Evidence | Separate offered/generated counts from actual grants and getter/sample evidence; verify inventory/level change after original claim/selection, not merely a changed UI label. |
| Icons | Non-readable atlas, packing flips, missing sprite, repeated search/cache and generation change remain bounded; original graphics state is restored even on failure. |

This audit did not start the game, grant equipment, inspect saves, change language or install tools. Runtime mutation and native evolution claims require controlled game tests after the corresponding native capability has passed its checks.

## Item icons

The Windows client requests one icon at a time for the selected item or a visible row. The backend resolves the ID against the current catalog, reads the verified `WeaponData` atlas and frame keys, and calls the game's `SpriteManager.GetSprite` overload (token `100666971`). The request shares the catalog's main-thread queue and session boundaries.

The backend crops the atlas with Unity's graphics API, using a temporary render texture at most 32 pixels per side. It restores the previous render target and releases temporary objects on success or failure. Readback verifies the Color32 array type, element count and byte length before copying. Pixels travel as bounded top-down RGBA in the local pipe. The client validates dimensions and payload length before constructing a Win32 image. The backend limits uncached reads to one per 160 ms and both sides cap their caches at 512 entries. Missing sprites and unsupported atlas packing use the existing text row.
