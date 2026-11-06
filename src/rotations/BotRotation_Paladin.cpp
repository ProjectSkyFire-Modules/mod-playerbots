/*
 * Retribution / Protection — modules/mod-playerbots/rotation.md
 * Unknown (low-level) spells are skipped via CanTryCast.
 */

#include "BotRotationLists.h"
#include "Player.h"
#include "Unit.h"

namespace BotRotation
{
namespace
{
    enum PalaSpells : uint32
    {
        SEAL_OF_TRUTH           = 31801,
        SEAL_OF_COMMAND         = 105361, // level 3 until Seal of Truth (24)
        INQUISITION             = 84963,
        TEMPLARS_VERDICT        = 85256,
        CRUSADER_STRIKE         = 35395,
        JUDGMENT                = 20271,
        EXORCISM                = 879,
        RIGHTEOUS_FURY          = 25780,
        SEAL_OF_INSIGHT         = 20165,
        AVENGERS_SHIELD         = 31935,
        HAMMER_OF_THE_RIGHTEOUS = 53595,
        SHIELD_OF_THE_RIGHTEOUS = 53600,
        CONSECRATION            = 26573,
        SEAL_OF_RIGHTEOUSNESS   = 20154,
        DIVINE_STORM            = 53385,
        HAMMER_OF_WRATH         = 24275,
        AVENGING_WRATH          = 31884,
        GUARDIAN_OF_KINGS_RET   = 86698,
        HOLY_AVENGER            = 105809,
        EXECUTION_SENTENCE      = 114157,
        HOLY_PRISM              = 114165,
        LIGHTS_HAMMER           = 114158,
        DIVINE_PURPOSE_PROC     = 90174,

        // Protection
        HOLY_WRATH              = 119072,
        SACRED_SHIELD           = 20925,
        SACRED_SHIELD_AURA      = 65148,
        GRAND_CRUSADER          = 85416,
        ETERNAL_FLAME           = 114163,
        WORD_OF_GLORY           = 85673,
        ARDENT_DEFENDER         = 31850,
        GUARDIAN_OF_KINGS_PROT  = 86659,
    };
}

uint32 SelectRetribution(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    uint32 const hp = bot->GetPower(POWER_HOLY_POWER);
    bool const divinePurpose = HasAuraUp(bot, DIVINE_PURPOSE_PROC);
    bool const avengingWrath = HasAuraUp(bot, AVENGING_WRATH);
    bool const cleave = ctx.enemies >= 2;

    // Seal of Righteousness only pays off on big packs; swap back below 3 to avoid flapping.
    if (ctx.enemies >= 4 && !HasAuraUp(bot, SEAL_OF_RIGHTEOUSNESS) && CanTryCast(bot, SEAL_OF_RIGHTEOUSNESS))
        return SEAL_OF_RIGHTEOUSNESS;
    if (ctx.enemies < 3 && !HasAuraUp(bot, SEAL_OF_TRUTH) && CanTryCast(bot, SEAL_OF_TRUTH))
        return SEAL_OF_TRUTH;
    if (!HasAuraUp(bot, SEAL_OF_TRUTH) && !HasAuraUp(bot, SEAL_OF_RIGHTEOUSNESS)
        && CanTryCast(bot, SEAL_OF_TRUTH))
        return SEAL_OF_TRUTH;
    if (!bot->HasSpell(SEAL_OF_TRUTH) && !HasAuraUp(bot, SEAL_OF_COMMAND) && CanTryCast(bot, SEAL_OF_COMMAND))
        return SEAL_OF_COMMAND;

    if (NeedsMyAuraRefresh(bot, bot, INQUISITION, 2.0f) && (hp >= 3 || divinePurpose)
        && CanTryCast(bot, INQUISITION))
        return INQUISITION;

    bool const inquisition = HasAuraUp(bot, INQUISITION);

    // Cooldowns line up inside Inquisition.
    if (inquisition && WantsCooldowns(ctx))
    {
        if (CanTryCast(bot, AVENGING_WRATH))
            return AVENGING_WRATH;
        if (CanTryCast(bot, GUARDIAN_OF_KINGS_RET))
            return GUARDIAN_OF_KINGS_RET;
        if (hp <= 2 && CanTryCast(bot, HOLY_AVENGER))
            return HOLY_AVENGER;
    }

    if (inquisition && CanTryCast(bot, EXECUTION_SENTENCE))
        return EXECUTION_SENTENCE;
    if (inquisition && CanTryCast(bot, LIGHTS_HAMMER))
        return LIGHTS_HAMMER;

    // Never sit on 5 Holy Power or a free spender; Holy Avenger triples generation.
    if (hp >= 5 || divinePurpose || (hp >= 3 && HasAuraUp(bot, HOLY_AVENGER)))
    {
        if (cleave && CanTryCast(bot, DIVINE_STORM))
            return DIVINE_STORM;
        if (CanTryCast(bot, TEMPLARS_VERDICT))
            return TEMPLARS_VERDICT;
    }

    // Only usable in execute range or during Avenging Wrath.
    if ((ctx.targetHealthPct < 20.0f || avengingWrath) && CanTryCast(bot, HAMMER_OF_WRATH))
        return HAMMER_OF_WRATH;

    if (avengingWrath && !cleave && hp >= 3 && CanTryCast(bot, TEMPLARS_VERDICT))
        return TEMPLARS_VERDICT;

    if (ctx.enemies >= 4 && CanTryCast(bot, HAMMER_OF_THE_RIGHTEOUS))
        return HAMMER_OF_THE_RIGHTEOUS;
    if (CanTryCast(bot, CRUSADER_STRIKE))
        return CRUSADER_STRIKE;

    if (CanTryCast(bot, JUDGMENT))
        return JUDGMENT;

    // Art of War resets this cooldown, so it simply shows up ready here.
    if (CanTryCast(bot, EXORCISM))
        return EXORCISM;

    // Holy Power is saved for the Inquisition refresh once it is about to drop.
    if (hp >= 3 && (AuraRemains(bot, INQUISITION) > 4.0f || !bot->HasSpell(INQUISITION)))
    {
        if (cleave && CanTryCast(bot, DIVINE_STORM))
            return DIVINE_STORM;
        if (CanTryCast(bot, TEMPLARS_VERDICT))
            return TEMPLARS_VERDICT;
    }

    if (CanTryCast(bot, HOLY_PRISM))
        return HOLY_PRISM;

    return 0;
}

uint32 SelectProtectionPaladin(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    uint32 const hp = bot->GetPower(POWER_HOLY_POWER);
    bool const divinePurpose = HasAuraUp(bot, DIVINE_PURPOSE_PROC);
    float const hpPct = bot->GetMaxHealth()
        ? (100.0f * float(bot->GetHealth()) / float(bot->GetMaxHealth())) : 100.0f;

    // Buffs: Righteous Fury, Seal of Insight
    if (!HasAuraUp(bot, RIGHTEOUS_FURY) && CanTryCast(bot, RIGHTEOUS_FURY))
        return RIGHTEOUS_FURY;
    if (!HasAuraUp(bot, SEAL_OF_INSIGHT) && CanTryCast(bot, SEAL_OF_INSIGHT))
        return SEAL_OF_INSIGHT;
    // Seal of Insight is level 32: Truth (24) or Command (3) until then.
    if (!bot->HasSpell(SEAL_OF_INSIGHT) && !HasAuraUp(bot, SEAL_OF_TRUTH) && !HasAuraUp(bot, SEAL_OF_COMMAND))
    {
        if (CanTryCast(bot, SEAL_OF_TRUTH))
            return SEAL_OF_TRUTH;
        if (!bot->HasSpell(SEAL_OF_TRUTH) && CanTryCast(bot, SEAL_OF_COMMAND))
            return SEAL_OF_COMMAND;
    }

    // Mitigation (Shield of the Righteous is off the GCD).
    if ((hp >= 3 || divinePurpose) && CanTryCast(bot, SHIELD_OF_THE_RIGHTEOUS))
        return SHIELD_OF_THE_RIGHTEOUS;
    if (!HasAuraUp(bot, SACRED_SHIELD_AURA) && CanTryCast(bot, SACRED_SHIELD))
        return SACRED_SHIELD;
    if (hpPct < 70.0f && (hp >= 3 || divinePurpose))
    {
        if (CanTryCast(bot, ETERNAL_FLAME))
            return ETERNAL_FLAME;
        if (CanTryCast(bot, WORD_OF_GLORY))
            return WORD_OF_GLORY;
    }

    if (hpPct < 35.0f && !HasAuraUp(bot, GUARDIAN_OF_KINGS_PROT) && CanTryCast(bot, ARDENT_DEFENDER))
        return ARDENT_DEFENDER;
    if (hpPct < 50.0f && !HasAuraUp(bot, ARDENT_DEFENDER) && CanTryCast(bot, GUARDIAN_OF_KINGS_PROT))
        return GUARDIAN_OF_KINGS_PROT;

    if (WantsCooldowns(ctx))
    {
        if (CanTryCast(bot, AVENGING_WRATH))
            return AVENGING_WRATH;
        if (CanTryCast(bot, HOLY_AVENGER))
            return HOLY_AVENGER;
    }

    if (HasAuraUp(bot, GRAND_CRUSADER) && CanTryCast(bot, AVENGERS_SHIELD))
        return AVENGERS_SHIELD;

    if (ctx.targetHealthPct < 20.0f && CanTryCast(bot, HAMMER_OF_WRATH))
        return HAMMER_OF_WRATH;

    if (ctx.enemies >= 3 && CanTryCast(bot, HAMMER_OF_THE_RIGHTEOUS))
        return HAMMER_OF_THE_RIGHTEOUS;
    if (CanTryCast(bot, CRUSADER_STRIKE))
        return CRUSADER_STRIKE;
    // Low level: Hammer of the Righteous before Crusader Strike is learned.
    if (CanTryCast(bot, HAMMER_OF_THE_RIGHTEOUS) && !bot->HasSpell(CRUSADER_STRIKE))
        return HAMMER_OF_THE_RIGHTEOUS;

    if (CanTryCast(bot, JUDGMENT))
        return JUDGMENT;

    if (CanTryCast(bot, AVENGERS_SHIELD))
        return AVENGERS_SHIELD;

    if (ctx.enemies >= 3 && CanTryCast(bot, CONSECRATION))
        return CONSECRATION;

    if (CanTryCast(bot, EXECUTION_SENTENCE))
        return EXECUTION_SENTENCE;
    if (CanTryCast(bot, HOLY_PRISM))
        return HOLY_PRISM;
    if (CanTryCast(bot, LIGHTS_HAMMER))
        return LIGHTS_HAMMER;

    if (CanTryCast(bot, HOLY_WRATH))
        return HOLY_WRATH;

    if (CanTryCast(bot, CONSECRATION))
        return CONSECRATION;

    return 0;
}

} // namespace BotRotation
