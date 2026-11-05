/*
 * Damage priorities for healer specs when co +healer dps is enabled.
 * Keep these short dungeon-assist lines (not full healer sims).
 */

#include "BotRotationLists.h"
#include "Player.h"
#include "Unit.h"

namespace BotRotation
{
uint32 SelectHolyPaladinDps(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;

    enum Spells : uint32
    {
        SEAL_OF_TRUTH       = 31801,
        SEAL_OF_RIGHTEOUSNESS = 20154,
        JUDGMENT            = 20271,
        CRUSADER_STRIKE     = 35395,
        EXORCISM            = 879,
        HOLY_SHOCK          = 20473,
        DENOUNCE            = 2812,
        HAMMER_OF_WRATH     = 24275,
        HOLY_PRISM          = 114165,
        AVENGING_WRATH      = 31884,
    };

    if (ctx.enemies >= 3)
    {
        if (!HasAuraUp(bot, SEAL_OF_RIGHTEOUSNESS) && CanTryCast(bot, SEAL_OF_RIGHTEOUSNESS))
            return SEAL_OF_RIGHTEOUSNESS;
    }
    else if (!HasAuraUp(bot, SEAL_OF_TRUTH) && !HasAuraUp(bot, SEAL_OF_RIGHTEOUSNESS)
        && !HasAuraUp(bot, 20165))
    {
        if (CanTryCast(bot, SEAL_OF_TRUTH))
            return SEAL_OF_TRUTH;
        if (CanTryCast(bot, SEAL_OF_RIGHTEOUSNESS))
            return SEAL_OF_RIGHTEOUSNESS;
        if (CanTryCast(bot, 20165)) // Seal of Insight
            return 20165;
    }

    if (WantsCooldowns(ctx) && CanTryCast(bot, AVENGING_WRATH))
        return AVENGING_WRATH;
    if (ctx.targetHealthPct <= 20.0f && CanTryCast(bot, HAMMER_OF_WRATH))
        return HAMMER_OF_WRATH;
    if (CanTryCast(bot, HOLY_PRISM))
        return HOLY_PRISM;
    // Holy Shock and Judgment are ranged. Crusader Strike is 5 yards, and the
    // healer-dps paladin closes to melee so it and the seal swings actually land.
    if (CanTryCast(bot, HOLY_SHOCK))
        return HOLY_SHOCK;
    if (CanTryCast(bot, JUDGMENT))
        return JUDGMENT;
    if (CanTryCast(bot, CRUSADER_STRIKE))
        return CRUSADER_STRIKE;
    if (CanTryCast(bot, EXORCISM))
        return EXORCISM;
    if (CanTryCast(bot, DENOUNCE))
        return DENOUNCE;
    return 0;
}

uint32 SelectDisciplineDps(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;

    enum Spells : uint32
    {
        INNER_FIRE          = 588,
        SHADOWFORM          = 15473, // not typically disc; skip
        SMITE               = 585,
        HOLY_FIRE           = 14914,
        PENANCE             = 47540,
        SHADOW_WORD_PAIN    = 589,
        HOLY_WORD_CHASTISE  = 88625,
        CASCADE             = 121135,
        HALO                = 120517,
        DIVINE_STAR         = 110744,
        POWER_INFUSION      = 10060,
    };

    if (!HasAuraUp(bot, INNER_FIRE) && CanTryCast(bot, INNER_FIRE))
        return INNER_FIRE;
    if (WantsCooldowns(ctx) && CanTryCast(bot, POWER_INFUSION))
        return POWER_INFUSION;

    if (ctx.enemies >= 3)
    {
        if (CanTryCast(bot, HALO))
            return HALO;
        if (CanTryCast(bot, CASCADE))
            return CASCADE;
        if (CanTryCast(bot, DIVINE_STAR))
            return DIVINE_STAR;
    }

    // Keep OUR SW:P up — do not thrash refresh every GCD.
    if (NeedsMyAuraRefresh(bot, target, SHADOW_WORD_PAIN, 3.0f) && CanTryCast(bot, SHADOW_WORD_PAIN))
        return SHADOW_WORD_PAIN;
    if (CanTryCast(bot, PENANCE))
        return PENANCE;
    if (CanTryCast(bot, HOLY_FIRE))
        return HOLY_FIRE;
    if (CanTryCast(bot, HOLY_WORD_CHASTISE))
        return HOLY_WORD_CHASTISE;
    if (CanTryCast(bot, SMITE))
        return SMITE;
    return 0;
}

uint32 SelectHolyPriestDps(Context const& ctx)
{
    // Holy shares most of the Disc damage toolkit in MoP.
    return SelectDisciplineDps(ctx);
}

uint32 SelectRestorationShamanDps(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;

    enum Spells : uint32
    {
        // Keep Water Shield while DPSing — Lightning Shield replaced it every
        // healer-dps GCD (worse under save-mana when heal GCDs are skipped).
        WATER_SHIELD        = 52127,
        FLAME_SHOCK         = 8050,
        LAVA_BURST          = 51505,
        LIGHTNING_BOLT      = 403,
        CHAIN_LIGHTNING     = 421,
        EARTH_SHOCK         = 8042,
        ELEMENTAL_BLAST     = 117014,
        UNLEASH_ELEMENTS    = 73680,
    };

    if (!HasAuraUp(bot, WATER_SHIELD) && CanTryCast(bot, WATER_SHIELD))
        return WATER_SHIELD;

    if (ctx.enemies >= 3 && CanTryCast(bot, CHAIN_LIGHTNING))
        return CHAIN_LIGHTNING;

    if ((!HasAuraUp(target, FLAME_SHOCK) || AuraRemains(target, FLAME_SHOCK) <= 3.0f)
        && CanTryCast(bot, FLAME_SHOCK))
        return FLAME_SHOCK;
    if (HasAuraUp(target, FLAME_SHOCK) && CanTryCast(bot, LAVA_BURST))
        return LAVA_BURST;
    if (CanTryCast(bot, ELEMENTAL_BLAST))
        return ELEMENTAL_BLAST;
    if (CanTryCast(bot, UNLEASH_ELEMENTS))
        return UNLEASH_ELEMENTS;
    if (CanTryCast(bot, EARTH_SHOCK))
        return EARTH_SHOCK;
    if (CanTryCast(bot, LIGHTNING_BOLT))
        return LIGHTNING_BOLT;
    return 0;
}

uint32 SelectRestorationDruidDps(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;

    enum Spells : uint32
    {
        MARK_OF_THE_WILD    = 1126,
        CAT_FORM            = 768,
        RAKE                = 1822,
        SWIPE_CAT           = 62078,
        FEROCIOUS_BITE      = 22568,
        MANGLE_CAT          = 33876,
        MOONFIRE            = 8921,
        WRATH               = 5176,
        HURRICANE           = 16914,
    };

    if (!HasAuraUp(bot, MARK_OF_THE_WILD) && CanTryCast(bot, MARK_OF_THE_WILD))
        return MARK_OF_THE_WILD;

    // Cat Form is level 8. Before that, stay a caster (Wrath / Moonfire).
    if (!bot->HasSpell(CAT_FORM))
    {
        if (ctx.enemies >= 3 && CanTryCast(bot, HURRICANE))
            return HURRICANE;
        if (NeedsMyAuraRefresh(bot, target, MOONFIRE, 2.0f) && CanTryCast(bot, MOONFIRE))
            return MOONFIRE;
        if (CanTryCast(bot, WRATH))
            return WRATH;
        return 0;
    }

    if (ReadyToShapeshift(bot, CAT_FORM))
        return CAT_FORM;

    // Resto does not learn Shred, Rip, or Savage Roar. Rake builds combo points.
    if (NeedsMyAuraRefresh(bot, target, RAKE, 3.0f) && CanTryCast(bot, RAKE))
        return RAKE;
    if (ctx.enemies >= 3 && CanTryCast(bot, SWIPE_CAT))
        return SWIPE_CAT;
    if (ctx.comboPoints >= 5 && CanTryCast(bot, FEROCIOUS_BITE))
        return FEROCIOUS_BITE;
    if (CanTryCast(bot, MANGLE_CAT))
        return MANGLE_CAT;
    if (ctx.comboPoints >= 1 && ctx.targetHealthPct <= 20.0f && CanTryCast(bot, FEROCIOUS_BITE))
        return FEROCIOUS_BITE;
    if (CanTryCast(bot, RAKE))
        return RAKE;
    return 0;
}

uint32 SelectMistweaverDps(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;

    enum Spells : uint32
    {
        STANCE_WISE_SERPENT = 115070,
        JAB                 = 100780,
        TIGER_PALM          = 100787,
        BLACKOUT_KICK       = 100784,
        RISING_SUN_KICK     = 107428,
        SPINNING_CRANE_KICK = 101546,
        EXPEL_HARM          = 115072,
        CRACKLING_JADE_LIGHTNING = 117952,
        TOUCH_OF_DEATH      = 115080,
        DEATH_NOTE          = 121125,
        CHI_WAVE            = 115098,
        ZEN_SPHERE          = 124081,
        CHI_BURST           = 123986,
        MANA_TEA            = 115294,
        MANA_TEA_GLYPHED    = 123761,
        MANA_TEA_STACKS     = 115867,
        TIGER_POWER         = 125359,
        SERPENTS_ZEAL       = 127722,
        MUSCLE_MEMORY       = 139597,
    };

    // Every Mistweaver heal needs Wise Serpent, and Jab/Tiger Palm/Blackout Kick need a stance.
    if (ReadyToShapeshift(bot, STANCE_WISE_SERPENT))
        return STANCE_WISE_SERPENT;
    if (!target)
        return 0;

    uint32 const chi = bot->GetPower(POWER_CHI);
    uint32 const maxChi = bot->GetMaxPower(POWER_CHI);
    float const manaPct = bot->GetMaxPower(POWER_MANA)
        ? (100.0f * float(bot->GetPower(POWER_MANA)) / float(bot->GetMaxPower(POWER_MANA))) : 100.0f;
    float const hpPct = bot->GetMaxHealth()
        ? (100.0f * float(bot->GetHealth()) / float(bot->GetMaxHealth())) : 100.0f;
    bool const inMelee = bot->IsWithinMeleeRange(target);
    bool const muscleMemory = HasAuraUp(bot, MUSCLE_MEMORY);

    if (HasAuraUp(bot, DEATH_NOTE) && CanTryCast(bot, TOUCH_OF_DEATH))
        return TOUCH_OF_DEATH;

    uint32 const teaStacks = AuraStacks(bot, MANA_TEA_STACKS);
    if (manaPct < 80.0f && teaStacks >= 2)
    {
        if (CanTryCast(bot, MANA_TEA_GLYPHED))
            return MANA_TEA_GLYPHED;
        if (CanTryCast(bot, MANA_TEA))
            return MANA_TEA;
    }

    // Level 30 talents damage the target and heal the group.
    if (CanTryCast(bot, CHI_WAVE))
        return CHI_WAVE;
    if (!HasAuraUp(bot, ZEN_SPHERE) && CanTryCast(bot, ZEN_SPHERE))
        return ZEN_SPHERE;
    if (CanTryCast(bot, CHI_BURST))
        return CHI_BURST;

    if (!inMelee)
    {
        if (CanTryCast(bot, CRACKLING_JADE_LIGHTNING))
            return CRACKLING_JADE_LIGHTNING;
        return 0;
    }

    // Tiger Power and Serpent's Zeal carry the fistweaving damage and healing.
    if (chi >= 1 && AuraRemains(bot, TIGER_POWER) <= 3.0f && CanTryCast(bot, TIGER_PALM))
        return TIGER_PALM;
    if (chi >= 2 && AuraRemains(bot, SERPENTS_ZEAL) <= 3.0f && CanTryCast(bot, BLACKOUT_KICK))
        return BLACKOUT_KICK;
    if (chi >= 2 && CanTryCast(bot, RISING_SUN_KICK))
        return RISING_SUN_KICK;

    if (ctx.enemies >= 3 && CanTryCast(bot, SPINNING_CRANE_KICK))
        return SPINNING_CRANE_KICK;

    // Muscle Memory (from Jab) refunds mana on the next Tiger Palm / Blackout Kick.
    if (muscleMemory)
    {
        if (chi >= 2 && CanTryCast(bot, BLACKOUT_KICK))
            return BLACKOUT_KICK;
        if (chi >= 1 && CanTryCast(bot, TIGER_PALM))
            return TIGER_PALM;
    }

    if (chi < maxChi)
    {
        if (hpPct < 90.0f && CanTryCast(bot, EXPEL_HARM))
            return EXPEL_HARM;
        if (CanTryCast(bot, JAB))
            return JAB;
    }

    if (chi >= 2 && CanTryCast(bot, BLACKOUT_KICK))
        return BLACKOUT_KICK;
    if (chi >= 1 && CanTryCast(bot, TIGER_PALM))
        return TIGER_PALM;
    return 0;
}

} // namespace BotRotation
