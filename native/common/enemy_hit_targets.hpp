#pragma once

#include <algorithm>
#include <cstdint>

namespace vst {

// Audited against the pinned EnemyType metadata, base data and all seven DLC assets.
// IsBullet() also matches zero-XP enemies. Keep actual projectiles and the
// Directer encounter controller out of the HP-based route.
inline constexpr int32_t kExcludedInstantKillTypes[]{
    125, // BULLET_1
    126, // BULLET_2
    127, // BULLET_3
    128, // BULLET_W
    129, // BULLET_Z
    196, // MOON_BAT_PROJECTILE
    246, // test
    248, // BULLET_SCYTHE
    273, // DIRECTER
    274, // BULLET_EGG
    275, // BULLET_EYE
    688, // FB_BULLET_FIREBALL
    689, // FB_BULLET_EYELASER
    690, // FB_BULLET_BOMB
    692, // FB_BULLET
    818, // BULLET_SM1
    819, // BULLET_SM2
    1053, // TP_BONE_BULLET
    1127, // TP_ADV_PHANTOM_BULLET
    1210, // EX_BULLET_COIN
    1212, // EX_SHOOTING_ENEMIES_COUNTER
};

inline constexpr int32_t kZeroXpEnemyTypes[]{
    60, // BOSS_XLDEATH
    61, // BOSS_XLSTALKER
    62, // BOSS_XLDROWNER
    71, // BOSS_XLLEDA
    164, // MOLISANO_ANFORA
    165, // MOON_ANFORA
    189, // MOON_TRINACRIA_X
    195, // MOON_BAT
    198, // BOSS_XLMADDENER
    199, // MASK_GOLD
    200, // MASK_SILVER
    201, // MASK_LEFT
    202, // MASK_RIGHT
    203, // BOSS_NOOB
    205, // BOSS_MADDENER_NORMAL
    211, // BOSS_DROWNER_NORMAL
    218, // BOSS_STALKER_NORMAL
    225, // BOSS_TRICKSTER_NORMAL
    238, // TRAINEE_R
    245, // BOSS_XLDEATH2
    247, // BOSS_ENDER
    249, // BOSS_XLTRICKSTER
    261, // UNDEADHEAD
    262, // UNDEADEYES
    263, // D_CLUSTER_COINS
    264, // D_CLUSTER_GEMS
    265, // D_MASK_SUN
    266, // D_MASK_MOON
    267, // D_MASK_CITY
    268, // D_MASK_VOLCANO
    269, // D_MASK_WINDS
    270, // D_MASK_GREED
    271, // D_MASK_VOID
    272, // D_SKULL
    276, // D_WEAK_REAPER_A
    277, // D_WEAK_REAPER_B
    278, // D_WEAK_REAPER_C
    279, // D_WEAK_REAPER_D
    280, // D_WEAK_REAPER_E
    281, // D_EYE
    283, // BOSS_SHARD_INFERNAS
    330, // MS_EVILSPIRIT
    354, // FS_MARI
    361, // FS_GLORY
    396, // FS_BOSS_JENEVIV
    430, // FS_SNAKBOMB
    431, // FS_SNAKSHIELD
    437, // FS_CRATE_HP
    452, // FS_BOSS_MEATBALL_29
    535, // CHAL_ANIMEXXL_TENTACLE_LEFT
    536, // CHAL_ANIMEXXL_TENTACLE_RIGHT
    568, // CHAL_ANIMEXXL_TENTACLE_LEFT2
    569, // CHAL_ANIMEXXL_TENTACLE_RIGHT2
    624, // EX_CUBE_HITS_STERILE
    803, // EX_CART_REAPER
    804, // EX_CART_TRICKSTER
    805, // EX_CART_STALKER
    806, // EX_CART_DROWNER
    807, // EX_CART_MADDENER
    842, // EX_CART_BLINDER
    1001, // TP_BOSS_LEGION_SECTION
    1002, // TP_BOSS_LEGION_ZOMBIE
    1004, // TP_BOSS_BEELZEBUB_SECTION
    1005, // TP_BOSS_BEELZEBUB_BEE
    1032, // TP_BOSS_DOPPLEGANGER
    1079, // TP_BOSS_DEATH_SCYTHE
    1080, // TP_BOSS_DEATH_SCYTHEBIG
    1160, // TP_ADV_MINOTAUR_SCALING
    1200, // EX_ECTOGOLD
    1201, // EX_GOLDPILE
    1202, // EX_JELLYBAG_RED
    1203, // EX_JELLYBAG_GREEN
    1204, // EX_JELLYBAG_PURPLE
    1205, // EX_XLDRAGON_GOLD
    1206, // EX_XLDRAGON_GOLD_FLAG
    1207, // EX_XLMANTIS_GOLD
    1249, // MOLISANO_ANFORA_GOLD
    1423, // EX_BATDRAKELET_CIRCLING
    1431, // EX_BATDRAKELET_SHADOW
    1443, // EX_BOSS_DROWNER_LYCAEUM
    1445, // EX_SIGRABLU_SWARM
    1446, // EX_SIGRABLU_EXPLO
    1447, // EX_BATDRAKELET_STATIONARY
    1826, // UN_BMN_ENEMY_026
    1827, // BMN_TENTACLE
    1830, // BMN_TENTACLE_MOVING
    1832, // BMN_SHADOW_SWARM
};

constexpr bool IsInstantKillEnemyType(int32_t type, bool game_classifies_as_bullet) noexcept {
    if (type < 0 || std::ranges::binary_search(kExcludedInstantKillTypes, type)) return false;
    return !game_classifies_as_bullet || std::ranges::binary_search(kZeroXpEnemyTypes, type);
}

} // namespace vst
