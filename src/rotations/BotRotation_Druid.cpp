/*
 * Guardian / Feral / Balance — modules/mod-playerbots/rotation.md
 * Unknown (low-level) spells are skipped via CanTryCast.
 */

#include "BotRotationLists.h"
#include "Player.h"
#include "Unit.h"

namespace BotRotation
{
namespace
{
    enum GuardianSpells : uint32
    {
        BEAR_FORM       = 5487,
        MANGLE_BEAR     = 33878,
        THRASH_BEAR     = 77758,
        LACERATE        = 33745,
        SWIPE           = 779,
        SAVAGE_DEFENSE  = 62606,
        SAVAGE_DEFENSE_BUFF = 132402,
        FRENZIED_REGEN  = 22842,
        MAUL            = 6807,
        BERSERK_BEAR    = 50334,
        ENRAGE_BEAR     = 5229,
    };

    enum FeralSpells : uint32
    {
        CAT_FORM        = 768,
        SAVAGE_ROAR     = 52610,
        RAKE            = 1822,
        MANGLE_CAT      = 33876,
        SHRED           = 5221,
        RIP             = 1079,
        FEROCIOUS_BITE  = 22568,
        TIGERS_FURY     = 5217,
        BERSERK_CAT     = 106951,
        INCARNATION_CAT = 102543,
        THRASH_CAT      = 106830,
        SWIPE_CAT       = 62078,
        CLEARCASTING    = 135700,
        FAERIE_FIRE     = 770,
        WEAKENED_ARMOR  = 113746,
    };

    enum BalanceSpells : uint32
    {
        MOONKIN_FORM    = 24858,
        MOONFIRE        = 8921,
        SUNFIRE         = 93402,
        STARSURGE       = 78674,
        WRATH           = 5176,
        STARFIRE        = 2912,
        STARFALL        = 48505,
        HURRICANE       = 16914,
        ECLIPSE_SOLAR   = 48517,
        ECLIPSE_LUNAR   = 48518,
        SOLAR_MARKER    = 67483,  // bar moving toward Solar: Starfire advances it
        LUNAR_MARKER    = 67484,  // bar moving toward Lunar: Wrath advances it
        CELESTIAL_ALIGNMENT = 112071,
        INCARNATION_ELUNE = 102560,
        NATURES_VIGIL   = 124974,
    };

    bool IsBehind(Player* bot, Unit* target)
    {
        return !target->HasInArc(static_cast<float>(M_PI), bot);
    }
}

uint32 SelectGuardian(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    if (ReadyToShapeshift(bot, BEAR_FORM))
        return BEAR_FORM;

    uint32 const rage = bot->GetPower(POWER_RAGE) / 10;
    float const hpPct = bot->GetMaxHealth()
        ? (100.0f * float(bot->GetHealth()) / float(bot->GetMaxHealth())) : 100.0f;

    // Savage Defense / Frenzied Regeneration are off the GCD and the main rage sinks.
    if (rage >= 60 && !HasAuraUp(bot, SAVAGE_DEFENSE_BUFF) && CanTryCast(bot, SAVAGE_DEFENSE))
        return SAVAGE_DEFENSE;
    if (hpPct < 60.0f && rage >= 30 && CanTryCast(bot, FRENZIED_REGEN))
        return FRENZIED_REGEN;
    if (rage < 30 && CanTryCast(bot, ENRAGE_BEAR))
        return ENRAGE_BEAR;
    if (WantsCooldowns(ctx) && CanTryCast(bot, BERSERK_BEAR))
        return BERSERK_BEAR;

    if (CanTryCast(bot, MANGLE_BEAR))
        return MANGLE_BEAR;

    if ((ctx.enemies >= 3 || NeedsMyAuraRefresh(bot, target, THRASH_BEAR, 3.0f)) && CanTryCast(bot, THRASH_BEAR))
        return THRASH_BEAR;

    if (AuraStacks(target, WEAKENED_ARMOR) < 3 && CanTryCast(bot, FAERIE_FIRE))
        return FAERIE_FIRE;

    uint32 const lacStacks = AuraStacks(target, LACERATE);
    if (ctx.enemies < 3 && (lacStacks < 3 || NeedsMyAuraRefresh(bot, target, LACERATE, 3.0f))
        && CanTryCast(bot, LACERATE))
        return LACERATE;

    if (CanTryCast(bot, SWIPE))
        return SWIPE;

    if (rage >= 90 && CanTryCast(bot, MAUL))
        return MAUL;

    if (CanTryCast(bot, LACERATE))
        return LACERATE;
    if (CanTryCast(bot, FAERIE_FIRE))
        return FAERIE_FIRE;

    return 0;
}

uint32 SelectFeral(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    int8 const cp = ctx.comboPoints;
    uint32 const energy = bot->GetPower(POWER_ENERGY);

    if (ReadyToShapeshift(bot, CAT_FORM))
        return CAT_FORM;

    if (cp > 0 && NeedsMyAuraRefresh(bot, bot, SAVAGE_ROAR, 1.0f) && CanTryCast(bot, SAVAGE_ROAR))
        return SAVAGE_ROAR;

    bool const berserk = HasAuraUp(bot, BERSERK_CAT);
    if (energy <= 35 && !berserk && CanTryCast(bot, TIGERS_FURY))
        return TIGERS_FURY;
    if (HasAuraUp(bot, TIGERS_FURY) && WantsCooldowns(ctx))
    {
        if (CanTryCast(bot, BERSERK_CAT))
            return BERSERK_CAT;
        if (CanTryCast(bot, INCARNATION_CAT))
            return INCARNATION_CAT;
    }

    if (AuraStacks(target, WEAKENED_ARMOR) < 3 && WantsCooldowns(ctx) && CanTryCast(bot, FAERIE_FIRE))
        return FAERIE_FIRE;

    bool const clearcasting = HasAuraUp(bot, CLEARCASTING);
    bool const behind = IsBehind(bot, target);

    if (ctx.enemies >= 3)
    {
        if (MyAuraRemains(bot, target, THRASH_CAT) < 4.5f && CanTryCast(bot, THRASH_CAT))
            return THRASH_CAT;
        if (cp >= 5 && NeedsMyAuraRefresh(bot, bot, SAVAGE_ROAR, 4.0f) && CanTryCast(bot, SAVAGE_ROAR))
            return SAVAGE_ROAR;
        if (CanTryCast(bot, SWIPE_CAT))
            return SWIPE_CAT;
    }

    if (cp >= 5)
    {
        if (NeedsMyAuraRefresh(bot, target, RIP, 2.0f) && CanTryCast(bot, RIP))
            return RIP;
        // Bite refreshes Rip in execute range, otherwise only spend when Rip and Roar are safe.
        if ((ctx.targetHealthPct < 25.0f
                || (MyAuraRemains(bot, target, RIP) > 6.0f && MyAuraRemains(bot, bot, SAVAGE_ROAR) > 4.0f))
            && CanTryCast(bot, FEROCIOUS_BITE))
            return FEROCIOUS_BITE;
    }

    if (NeedsMyAuraRefresh(bot, target, RAKE, 3.0f) && CanTryCast(bot, RAKE))
        return RAKE;
    if (clearcasting && NeedsMyAuraRefresh(bot, target, THRASH_CAT, 3.0f) && CanTryCast(bot, THRASH_CAT))
        return THRASH_CAT;

    if (cp < 5)
    {
        if (behind && (berserk || clearcasting) && CanTryCast(bot, SHRED))
            return SHRED;
        if (CanTryCast(bot, MANGLE_CAT))
            return MANGLE_CAT;
    }
    if (behind && CanTryCast(bot, SHRED))
        return SHRED;
    if (CanTryCast(bot, MANGLE_CAT))
        return MANGLE_CAT;

    return 0;
}

uint32 SelectBalance(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    if (ReadyToShapeshift(bot, MOONKIN_FORM))
        return MOONKIN_FORM;

    bool const solar = HasAuraUp(bot, ECLIPSE_SOLAR);
    bool const lunar = HasAuraUp(bot, ECLIPSE_LUNAR);

    if ((solar || lunar) && WantsCooldowns(ctx))
    {
        if (CanTryCast(bot, CELESTIAL_ALIGNMENT))
            return CELESTIAL_ALIGNMENT;
        if (CanTryCast(bot, INCARNATION_ELUNE))
            return INCARNATION_ELUNE;
        if (CanTryCast(bot, NATURES_VIGIL))
            return NATURES_VIGIL;
    }

    if ((lunar || ctx.enemies >= 4) && CanTryCast(bot, STARFALL))
        return STARFALL;

    // Each DoT is empowered by its own Eclipse, so refresh it there.
    if (MyAuraRemains(bot, target, SUNFIRE) <= (solar ? 3.0f : 2.0f) && CanTryCast(bot, SUNFIRE))
        return SUNFIRE;
    if (MyAuraRemains(bot, target, MOONFIRE) <= (lunar ? 3.0f : 2.0f) && CanTryCast(bot, MOONFIRE))
        return MOONFIRE;

    if (ctx.enemies >= 4 && !HasGroundEffect(bot, HURRICANE) && CanTryCast(bot, HURRICANE))
        return HURRICANE;

    if (CanTryCast(bot, STARSURGE))
        return STARSURGE;

    bool const towardSolar = lunar || (!solar && HasAuraUp(bot, SOLAR_MARKER));
    if (towardSolar && CanTryCast(bot, STARFIRE))
        return STARFIRE;
    if (CanTryCast(bot, WRATH))
        return WRATH;
    if (CanTryCast(bot, STARFIRE))
        return STARFIRE;

    return 0;
}

uint32 SelectLowLevelDruid(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    bool const cat = bot->GetShapeshiftForm() == FORM_CAT;
    float const dist = bot->GetDistance(target);

    // Open from range; Cat Form (level 6) once the target closes in.
    if (cat && !bot->IsInCombat() && dist > 10.0f)
    {
        bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
        return SelectBalance(ctx);
    }
    if (!cat)
    {
        if ((bot->IsWithinMeleeRange(target) || dist <= 8.0f) && ReadyToShapeshift(bot, CAT_FORM))
            return CAT_FORM;
        return SelectBalance(ctx);
    }

    int8 const cp = ctx.comboPoints;
    if ((cp >= 5 || (cp >= 1 && ctx.targetHealthPct <= 25.0f)) && CanTryCast(bot, FEROCIOUS_BITE))
        return FEROCIOUS_BITE;
    if (NeedsMyAuraRefresh(bot, target, RAKE, 3.0f) && CanTryCast(bot, RAKE))
        return RAKE;
    if (CanTryCast(bot, MANGLE_CAT))
        return MANGLE_CAT;

    return 0;
}

} // namespace BotRotation
