/*
 * Wave 4 healer priorities - simplified MoP / Hekili dungeon lines.
 *
 * Triage thresholds (ally HP%):
 *   < 25%  critical  — CDs / instant saves
 *   < 40%  urgent    — Flash / Surge / Regrowth
 *   < 65%  big heal  — Greater Heal / Holy Light / GHW / Healing Touch
 *   < 90%  maintenance — HoTs, shields, PoM, efficient small heals
 * Above ~90% SelectHealTarget will not pick them at all.
 */

#include "BotRotationLists.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Unit.h"

namespace BotRotation
{
namespace
{
    // Expensive triage heals are skipped while save-mana is active and mana is
    // below threshold, unless the ally is critically low.
    bool AllowExpensiveHeal(HealContext const& ctx)
    {
        if (!ctx.saveMana)
            return true;
        if (ctx.manaPct >= ctx.saveManaThreshold)
            return true;
        return ctx.healTargetHealthPct < 30.0f;
    }

    bool IsCritical(HealContext const& ctx) { return ctx.healTargetHealthPct < 25.0f; }
    bool IsUrgent(HealContext const& ctx) { return ctx.healTargetHealthPct < 40.0f; }
    bool NeedsBigHeal(HealContext const& ctx) { return ctx.healTargetHealthPct < 65.0f; }
    bool NeedsMaintenance(HealContext const& ctx) { return ctx.healTargetHealthPct < 90.0f; }
}

uint32 SelectHolyPaladin(HealContext const& ctx)
{
    Player* bot = ctx.bot;
    Player* ally = ctx.healTarget;
    uint32 const hp = bot->GetPower(POWER_HOLY_POWER);
    bool const urgent = IsUrgent(ctx);
    bool const big = NeedsBigHeal(ctx);

    enum HolyPalaSpells : uint32
    {
        SEAL_OF_INSIGHT     = 20165,
        BLESSING_OF_KINGS   = 20217,
        BEACON_OF_LIGHT     = 53563,
        DIVINE_PLEA         = 54428,
        DIVINE_FAVOR        = 31842,
        AVENGING_WRATH      = 31884,
        HOLY_SHOCK          = 20473,
        FLASH_OF_LIGHT      = 19750,
        HOLY_LIGHT          = 635,
        ETERNAL_FLAME       = 114163,
        WORD_OF_GLORY       = 85673,
        LIGHT_OF_DAWN       = 85222,
        HOLY_RADIANCE       = 82327,
        DIVINE_PURPOSE      = 90174,
    };

    if (!HasAuraUp(bot, SEAL_OF_INSIGHT) && CanTryCast(bot, SEAL_OF_INSIGHT))
        return SEAL_OF_INSIGHT;
    if (!HasAuraUp(bot, BLESSING_OF_KINGS) && CanTryCast(bot, BLESSING_OF_KINGS))
        return BLESSING_OF_KINGS;
    if (!HasAuraUp(ally, BEACON_OF_LIGHT) && CanTryCast(bot, BEACON_OF_LIGHT))
        return BEACON_OF_LIGHT;

    if (ctx.manaPct < 60.0f && CanTryCast(bot, DIVINE_PLEA))
        return DIVINE_PLEA;
    if (NeedsMaintenance(ctx) && CanTryCast(bot, DIVINE_FAVOR))
        return DIVINE_FAVOR;
    if (NeedsMaintenance(ctx) && CanTryCast(bot, AVENGING_WRATH))
        return AVENGING_WRATH;

    if ((hp >= 3 || HasAuraUp(bot, DIVINE_PURPOSE)))
    {
        if (ctx.injuredAllies >= 3 && CanTryCast(bot, LIGHT_OF_DAWN))
            return LIGHT_OF_DAWN;
        if (NeedsMaintenance(ctx) && CanTryCast(bot, ETERNAL_FLAME))
            return ETERNAL_FLAME;
        if (NeedsMaintenance(ctx) && CanTryCast(bot, WORD_OF_GLORY))
            return WORD_OF_GLORY;
    }

    if (ctx.injuredAllies >= 3 && CanTryCast(bot, HOLY_RADIANCE))
        return HOLY_RADIANCE;

    if (urgent && AllowExpensiveHeal(ctx) && CanTryCast(bot, FLASH_OF_LIGHT))
        return FLASH_OF_LIGHT;
    if (NeedsMaintenance(ctx) && CanTryCast(bot, HOLY_SHOCK))
        return HOLY_SHOCK;
    if (big && AllowExpensiveHeal(ctx) && CanTryCast(bot, HOLY_LIGHT))
        return HOLY_LIGHT;
    // Holy Light is level 34.
    if (big && !bot->HasSpell(HOLY_LIGHT) && AllowExpensiveHeal(ctx) && CanTryCast(bot, FLASH_OF_LIGHT))
        return FLASH_OF_LIGHT;

    return 0;
}

uint32 SelectDiscipline(HealContext const& ctx)
{
    Player* bot = ctx.bot;
    Player* ally = ctx.healTarget;
    bool const urgent = IsUrgent(ctx);
    bool const big = NeedsBigHeal(ctx);

    enum DiscSpells : uint32
    {
        INNER_FIRE          = 588,
        POWER_WORD_FORT     = 21562,
        POWER_WORD_SHIELD   = 17,
        PENANCE             = 47540,
        FLASH_HEAL          = 2061,
        HEAL                = 2050,
        GREATER_HEAL        = 2060,
        PRAYER_OF_MENDING   = 33076,
        RENEW               = 139,
        SPIRIT_SHELL        = 109964,
        PAIN_SUPPRESSION    = 33206,
        POWER_INFUSION      = 10060,
    };

    if (!HasAuraUp(bot, INNER_FIRE) && CanTryCast(bot, INNER_FIRE))
        return INNER_FIRE;
    if (!HasAuraUp(bot, POWER_WORD_FORT) && CanTryCast(bot, POWER_WORD_FORT))
        return POWER_WORD_FORT;

    if (IsCritical(ctx) && CanTryCast(bot, PAIN_SUPPRESSION))
        return PAIN_SUPPRESSION;
    if (NeedsMaintenance(ctx) && CanTryCast(bot, POWER_INFUSION))
        return POWER_INFUSION;
    if (NeedsMaintenance(ctx) && CanTryCast(bot, SPIRIT_SHELL))
        return SPIRIT_SHELL;

    if (urgent && !HasAuraUp(ally, POWER_WORD_SHIELD) && CanTryCast(bot, POWER_WORD_SHIELD))
        return POWER_WORD_SHIELD;
    if (NeedsMaintenance(ctx) && !HasAuraUp(ally, PRAYER_OF_MENDING) && CanTryCast(bot, PRAYER_OF_MENDING))
        return PRAYER_OF_MENDING;

    if (NeedsMaintenance(ctx) && CanTryCast(bot, PENANCE))
        return PENANCE;

    if (urgent && AllowExpensiveHeal(ctx) && CanTryCast(bot, FLASH_HEAL))
        return FLASH_HEAL;
    // Greater Heal only when someone is actually hurt — never as a top-off.
    if (big && AllowExpensiveHeal(ctx) && CanTryCast(bot, GREATER_HEAL))
        return GREATER_HEAL;
    // Greater Heal is level 34 and Heal 28.
    if (big && !bot->HasSpell(GREATER_HEAL) && AllowExpensiveHeal(ctx))
    {
        if (CanTryCast(bot, HEAL))
            return HEAL;
        if (CanTryCast(bot, FLASH_HEAL))
            return FLASH_HEAL;
    }
    if (NeedsMaintenance(ctx) && !HasAuraUp(ally, RENEW) && CanTryCast(bot, RENEW))
        return RENEW;

    return 0;
}

uint32 SelectHolyPriest(HealContext const& ctx)
{
    Player* bot = ctx.bot;
    Player* ally = ctx.healTarget;
    bool const urgent = IsUrgent(ctx);
    bool const critical = IsCritical(ctx);
    bool const big = NeedsBigHeal(ctx);

    enum HolyPriestSpells : uint32
    {
        INNER_FIRE          = 588,
        POWER_WORD_FORT     = 21562,
        RENEW               = 139,
        PRAYER_OF_MENDING   = 33076,
        FLASH_HEAL          = 2061,
        HEAL                = 2050,
        GREATER_HEAL        = 2060,
        CIRCLE_OF_HEALING   = 34861,
        PRAYER_OF_HEALING   = 596,
        GUARDIAN_SPIRIT     = 47788,
        DIVINE_HYMN         = 64843,
        HOLY_WORD_SERENITY  = 88684,
        BINDING_HEAL        = 32546,
    };

    if (!HasAuraUp(bot, INNER_FIRE) && CanTryCast(bot, INNER_FIRE))
        return INNER_FIRE;
    if (!HasAuraUp(bot, POWER_WORD_FORT) && CanTryCast(bot, POWER_WORD_FORT))
        return POWER_WORD_FORT;

    if (critical && CanTryCast(bot, GUARDIAN_SPIRIT))
        return GUARDIAN_SPIRIT;
    if (ctx.injuredAllies >= 4 && CanTryCast(bot, DIVINE_HYMN))
        return DIVINE_HYMN;

    if (NeedsMaintenance(ctx) && !HasAuraUp(ally, RENEW) && CanTryCast(bot, RENEW))
        return RENEW;
    if (NeedsMaintenance(ctx) && !HasAuraUp(ally, PRAYER_OF_MENDING) && CanTryCast(bot, PRAYER_OF_MENDING))
        return PRAYER_OF_MENDING;

    if (ctx.injuredAllies >= 3)
    {
        if (CanTryCast(bot, CIRCLE_OF_HEALING))
            return CIRCLE_OF_HEALING;
        if (CanTryCast(bot, PRAYER_OF_HEALING))
            return PRAYER_OF_HEALING;
    }

    if (NeedsMaintenance(ctx) && CanTryCast(bot, HOLY_WORD_SERENITY))
        return HOLY_WORD_SERENITY;
    if (urgent && AllowExpensiveHeal(ctx) && CanTryCast(bot, FLASH_HEAL))
        return FLASH_HEAL;
    if (urgent && CanTryCast(bot, BINDING_HEAL))
        return BINDING_HEAL;
    if (big && AllowExpensiveHeal(ctx) && CanTryCast(bot, GREATER_HEAL))
        return GREATER_HEAL;
    // Greater Heal is level 34 and Heal 28.
    if (big && !bot->HasSpell(GREATER_HEAL) && AllowExpensiveHeal(ctx))
    {
        if (CanTryCast(bot, HEAL))
            return HEAL;
        if (CanTryCast(bot, FLASH_HEAL))
            return FLASH_HEAL;
    }

    return 0;
}

uint32 SelectRestorationShaman(HealContext const& ctx)
{
    Player* bot = ctx.bot;
    Player* ally = ctx.healTarget;
    bool const urgent = IsUrgent(ctx);
    bool const big = NeedsBigHeal(ctx);

    enum RestoShamSpells : uint32
    {
        WATER_SHIELD            = 52127,
        EARTH_SHIELD            = 974,
        RIPTIDE                 = 61295,
        HEALING_SURGE           = 8004,
        HEALING_WAVE            = 331,
        GREATER_HEALING_WAVE    = 77472,
        CHAIN_HEAL              = 1064,
        HEALING_STREAM_TOTEM    = 5394,
        HEALING_RAIN            = 73920,
        ASCENDANCE              = 114049,
    };

    auto hasAliveTotem = [](Player* sham) -> bool
    {
        if (!sham)
            return false;
        for (uint8 slot = SUMMON_SLOT_TOTEM; slot < MAX_TOTEM_SLOT; ++slot)
        {
            uint64 const guid = sham->m_SummonSlot[slot];
            if (!guid)
                continue;
            if (Creature* totem = ObjectAccessor::GetCreature(*sham, guid))
                if (totem->IsAlive())
                    return true;
        }
        return false;
    };

    if (!HasAuraUp(bot, WATER_SHIELD) && !urgent && CanTryCast(bot, WATER_SHIELD))
        return WATER_SHIELD;

    // Real triage heals before totems / long CDs — HST every GCD starved the line.
    if (urgent && AllowExpensiveHeal(ctx) && CanTryCast(bot, HEALING_SURGE))
        return HEALING_SURGE;
    if (big && AllowExpensiveHeal(ctx) && CanTryCast(bot, GREATER_HEALING_WAVE))
        return GREATER_HEALING_WAVE;
    // Greater Healing Wave is level 60 and Healing Wave 20.
    if (big && !bot->HasSpell(GREATER_HEALING_WAVE) && AllowExpensiveHeal(ctx))
    {
        if (CanTryCast(bot, HEALING_WAVE))
            return HEALING_WAVE;
        if (CanTryCast(bot, HEALING_SURGE))
            return HEALING_SURGE;
    }

    if (NeedsMaintenance(ctx)
        && (!HasAuraUp(ally, RIPTIDE) || AuraRemains(ally, RIPTIDE) <= 3.0f)
        && CanTryCast(bot, RIPTIDE))
        return RIPTIDE;

    if (ctx.injuredAllies >= 3)
    {
        if (CanTryCast(bot, HEALING_RAIN))
            return HEALING_RAIN;
        if (CanTryCast(bot, CHAIN_HEAL))
            return CHAIN_HEAL;
    }

    if (NeedsMaintenance(ctx) && ctx.healTargetHealthPct < 85.0f && CanTryCast(bot, HEALING_WAVE))
        return HEALING_WAVE;

    if (!HasAuraUp(bot, WATER_SHIELD) && CanTryCast(bot, WATER_SHIELD))
        return WATER_SHIELD;

    if (!HasAuraUp(ally, EARTH_SHIELD) && CanTryCast(bot, EARTH_SHIELD))
        return EARTH_SHIELD;

    // Ascendance is a short CD save, not a maintenance filler.
    if (urgent && CanTryCast(bot, ASCENDANCE))
        return ASCENDANCE;

    if (NeedsMaintenance(ctx) && !hasAliveTotem(bot) && CanTryCast(bot, HEALING_STREAM_TOTEM))
        return HEALING_STREAM_TOTEM;

    return 0;
}

uint32 SelectRestorationDruid(HealContext const& ctx)
{
    Player* bot = ctx.bot;
    Player* ally = ctx.healTarget;
    bool const urgent = IsUrgent(ctx);
    bool const big = NeedsBigHeal(ctx);

    enum RestoDruidSpells : uint32
    {
        MARK_OF_THE_WILD    = 1126,
        REJUVENATION        = 774,
        LIFEBLOOM           = 33763,
        REGROWTH            = 8936,
        HEALING_TOUCH       = 5185,
        WILD_GROWTH         = 48438,
        SWIFTMEND           = 18562,
        NATURES_SWIFTNESS   = 132158,
        TREE_OF_LIFE        = 33891,
    };

    if (!HasAuraUp(bot, MARK_OF_THE_WILD) && CanTryCast(bot, MARK_OF_THE_WILD))
        return MARK_OF_THE_WILD;
    if (NeedsMaintenance(ctx) && ReadyToShapeshift(bot, TREE_OF_LIFE))
        return TREE_OF_LIFE;

    if (NeedsMaintenance(ctx)
        && (!HasAuraUp(ally, LIFEBLOOM) || AuraStacks(ally, LIFEBLOOM) < 3
            || AuraRemains(ally, LIFEBLOOM) <= 3.0f) && CanTryCast(bot, LIFEBLOOM))
        return LIFEBLOOM;
    if (NeedsMaintenance(ctx) && !HasAuraUp(ally, REJUVENATION) && CanTryCast(bot, REJUVENATION))
        return REJUVENATION;

    if (NeedsMaintenance(ctx)
        && (HasAuraUp(ally, REJUVENATION) || HasAuraUp(ally, REGROWTH))
        && CanTryCast(bot, SWIFTMEND))
        return SWIFTMEND;

    if (ctx.injuredAllies >= 3 && CanTryCast(bot, WILD_GROWTH))
        return WILD_GROWTH;

    if (urgent && CanTryCast(bot, NATURES_SWIFTNESS))
        return NATURES_SWIFTNESS;
    if (urgent && AllowExpensiveHeal(ctx) && CanTryCast(bot, REGROWTH))
        return REGROWTH;
    if (big && AllowExpensiveHeal(ctx) && CanTryCast(bot, HEALING_TOUCH))
        return HEALING_TOUCH;
    // Healing Touch is level 26.
    if (big && !bot->HasSpell(HEALING_TOUCH) && AllowExpensiveHeal(ctx) && CanTryCast(bot, REGROWTH))
        return REGROWTH;
    if (NeedsMaintenance(ctx) && !HasAuraUp(ally, REJUVENATION) && CanTryCast(bot, REJUVENATION))
        return REJUVENATION;

    return 0;
}

uint32 SelectMistweaver(HealContext const& ctx)
{
    Player* bot = ctx.bot;
    Player* ally = ctx.healTarget;
    uint32 const chi = bot->GetPower(POWER_CHI);
    bool const urgent = IsUrgent(ctx);
    bool const big = NeedsBigHeal(ctx);

    enum MistweaverSpells : uint32
    {
        SOOTHING_MIST       = 115175,
        SURGING_MIST        = 116694,
        ENVELOPING_MIST     = 124682,
        RENEWING_MIST       = 115151,
        UPLIFT              = 116670,
        LIFE_COCOON         = 116849,
        REVIVAL             = 115310,
        THUNDER_FOCUS_TEA   = 116680,
        STANCE_WISE_SERPENT = 115070,
        VITAL_MISTS         = 118674,
        EXPEL_HARM          = 115072,
        CHI_WAVE            = 115098,
        ZEN_SPHERE          = 124081,
        CHI_BURST           = 123986,
        MANA_TEA            = 115294,
        MANA_TEA_GLYPHED    = 123761,
        MANA_TEA_STACKS     = 115867,
    };

    if (ReadyToShapeshift(bot, STANCE_WISE_SERPENT))
        return STANCE_WISE_SERPENT;

    if (IsCritical(ctx) && CanTryCast(bot, LIFE_COCOON))
        return LIFE_COCOON;
    if (ctx.injuredAllies >= 4 && CanTryCast(bot, REVIVAL))
        return REVIVAL;

    // Five Vital Mists stacks make Surging Mist instant and free.
    if (AuraStacks(bot, VITAL_MISTS) >= 5 && ctx.healTargetHealthPct < 90.0f && CanTryCast(bot, SURGING_MIST))
        return SURGING_MIST;

    if (NeedsMaintenance(ctx) && CanTryCast(bot, THUNDER_FOCUS_TEA))
        return THUNDER_FOCUS_TEA;
    if (NeedsMaintenance(ctx) && !HasAuraUp(ally, RENEWING_MIST) && CanTryCast(bot, RENEWING_MIST))
        return RENEWING_MIST;
    if (ctx.injuredAllies >= 2 && chi >= 2 && CanTryCast(bot, UPLIFT))
        return UPLIFT;
    if (big && chi >= 3 && !HasAuraUp(ally, ENVELOPING_MIST) && CanTryCast(bot, ENVELOPING_MIST))
        return ENVELOPING_MIST;
    if (urgent && AllowExpensiveHeal(ctx) && CanTryCast(bot, SURGING_MIST))
        return SURGING_MIST;

    if (ctx.injuredAllies >= 2 || urgent)
    {
        if (CanTryCast(bot, CHI_WAVE))
            return CHI_WAVE;
        if (CanTryCast(bot, CHI_BURST))
            return CHI_BURST;
    }
    if (NeedsMaintenance(ctx) && !HasAuraUp(ally, ZEN_SPHERE) && CanTryCast(bot, ZEN_SPHERE))
        return ZEN_SPHERE;

    if (ctx.manaPct < 70.0f && AuraStacks(bot, MANA_TEA_STACKS) >= 2)
    {
        if (CanTryCast(bot, MANA_TEA_GLYPHED))
            return MANA_TEA_GLYPHED;
        if (CanTryCast(bot, MANA_TEA))
            return MANA_TEA;
    }

    // Expel Harm heals the monk and builds Chi for Uplift / Enveloping Mist.
    if (ally == bot && chi < bot->GetMaxPower(POWER_CHI) && CanTryCast(bot, EXPEL_HARM))
        return EXPEL_HARM;

    if (NeedsMaintenance(ctx) && CanTryCast(bot, SOOTHING_MIST))
        return SOOTHING_MIST;

    return 0;
}

} // namespace BotRotation
