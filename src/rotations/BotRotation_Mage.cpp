// Arcane / Fire / Frost — modules/mod-playerbots/rotation.md
// Unknown (low-level) spells are skipped via CanTryCast.

#include "BotRotationLists.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

namespace BotRotation
{
namespace
{
    enum MageSpells : uint32
    {
        MAGE_ARMOR          = 6117,
        MOLTEN_ARMOR        = 30482,
        FROST_ARMOR         = 7302,
        MIRROR_IMAGE        = 55342,
        EVOCATION           = 12051,
        ARCANE_EXPLOSION    = 1449,

        // Talents (one of the three bombs)
        LIVING_BOMB         = 44457,
        NETHER_TEMPEST      = 114923,
        FROST_BOMB          = 112948,

        // Arcane
        ARCANE_BLAST        = 30451,
        ARCANE_MISSILES     = 5143,
        ARCANE_BARRAGE      = 44425,
        ARCANE_CHARGE       = 36032,
        ARCANE_MISSILES_PROC = 79683,
        ARCANE_POWER        = 12042,

        // Fire
        INFERNO_BLAST       = 108853,
        PYROBLAST           = 11366,
        HEATING_UP          = 48107,
        HOT_STREAK          = 48108,
        FIREBALL            = 133,
        COMBUSTION          = 11129,
        IGNITE              = 12654,
        DRAGONS_BREATH      = 31661,
        FLAMESTRIKE         = 2120,

        // Frost
        FROZEN_ORB          = 84714,
        ICE_LANCE           = 30455,
        FINGERS_OF_FROST    = 44544,
        FROSTFIRE_BOLT      = 44614,
        BRAIN_FREEZE        = 57761,
        FROSTBOLT           = 116,
        ICY_VEINS           = 12472,
        SUMMON_WATER_ELEMENTAL = 31687,
        CONE_OF_COLD        = 120,
        BLIZZARD            = 10,
        FIRE_BLAST          = 2136,
        FROST_NOVA          = 122,
    };

    // Best mage armor the bot actually knows. Frost Armor is 54, so a level 46
    // frost mage is still on Molten Armor; cast that instead of skipping armor.
    uint32 SelectKnownArmor(Player* bot, uint32 preferred)
    {
        uint32 const order[] = { preferred, FROST_ARMOR, MOLTEN_ARMOR, MAGE_ARMOR };
        uint32 best = 0;
        for (uint32 id : order)
        {
            if (!bot->HasSpell(id))
                continue;
            best = id;
            break;
        }
        if (!best || HasAuraUp(bot, best))
            return 0;
        return CanTryCast(bot, best) ? best : 0;
    }

    bool InSpellRange(Player* bot, Unit* target, uint32 spellId, float fallback)
    {
        if (!bot || !target)
            return false;
        float range = fallback;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
        {
            // false: hostile range. Friend range is 0 on damage spells.
            float const maxRange = info->GetMaxRange(false, bot);
            if (maxRange > 1.0f)
                range = maxRange;
        }
        return bot->IsWithinDistInMap(target, range);
    }

    float ManaPct(Player* bot)
    {
        return bot->GetMaxPower(POWER_MANA)
            ? (100.0f * float(bot->GetPower(POWER_MANA)) / float(bot->GetMaxPower(POWER_MANA))) : 100.0f;
    }

    uint32 SelectBomb(Player* bot, Unit* target)
    {
        if (!HasMyAura(bot, target, LIVING_BOMB) && CanTryCast(bot, LIVING_BOMB))
            return LIVING_BOMB;
        if (!HasMyAura(bot, target, NETHER_TEMPEST) && CanTryCast(bot, NETHER_TEMPEST))
            return NETHER_TEMPEST;
        if (!HasMyAura(bot, target, FROST_BOMB) && CanTryCast(bot, FROST_BOMB))
            return FROST_BOMB;
        return 0;
    }

    uint32 SelectMageUtility(Context const& ctx)
    {
        Player* bot = ctx.bot;
        if (WantsCooldowns(ctx) && CanTryCast(bot, MIRROR_IMAGE))
            return MIRROR_IMAGE;
        if (ManaPct(bot) < 15.0f && bot->IsStopped() && CanTryCast(bot, EVOCATION))
            return EVOCATION;
        return 0;
    }
}

uint32 SelectArcane(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    if (!HasAuraUp(bot, MAGE_ARMOR) && CanTryCast(bot, MAGE_ARMOR))
        return MAGE_ARMOR;

    if (uint32 const bomb = SelectBomb(bot, target))
        return bomb;
    if (uint32 const util = SelectMageUtility(ctx))
        return util;

    uint32 const charges = AuraStacks(bot, ARCANE_CHARGE);
    uint32 const missiles = AuraStacks(bot, ARCANE_MISSILES_PROC);
    float const mana = ManaPct(bot);

    if (charges >= 4 && missiles && WantsCooldowns(ctx) && CanTryCast(bot, ARCANE_POWER))
        return ARCANE_POWER;

    if (ctx.enemies >= 4 && bot->GetDistance(target) <= 8.0f && CanTryCast(bot, ARCANE_EXPLOSION))
        return ARCANE_EXPLOSION;

    if (charges >= 4)
    {
        if ((missiles >= 2 || (missiles == 1 && mana < 95.0f)) && CanTryCast(bot, ARCANE_MISSILES))
            return ARCANE_MISSILES;
        if (!missiles && mana < 90.0f && CanTryCast(bot, ARCANE_BARRAGE))
            return ARCANE_BARRAGE;
    }
    else if (missiles >= 2 && CanTryCast(bot, ARCANE_MISSILES))
        return ARCANE_MISSILES;

    if (CanTryCast(bot, ARCANE_BLAST))
        return ARCANE_BLAST;
    // Low level: Missiles procs before Arcane Blast is learned.
    if (missiles && CanTryCast(bot, ARCANE_MISSILES))
        return ARCANE_MISSILES;

    return 0;
}

uint32 SelectFire(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    if (!HasAuraUp(bot, MOLTEN_ARMOR) && CanTryCast(bot, MOLTEN_ARMOR))
        return MOLTEN_ARMOR;

    bool const hotStreak = HasAuraUp(bot, HOT_STREAK);
    bool const heatingUp = HasAuraUp(bot, HEATING_UP);

    if (uint32 const util = SelectMageUtility(ctx))
        return util;

    // Combustion copies Ignite and the Pyroblast DoT, so only fire it with both rolling.
    if (WantsCooldowns(ctx) && HasMyAura(bot, target, IGNITE) && HasMyAura(bot, target, PYROBLAST)
        && CanTryCast(bot, COMBUSTION))
        return COMBUSTION;

    if (hotStreak && CanTryCast(bot, PYROBLAST))
        return PYROBLAST;
    if (heatingUp && CanTryCast(bot, INFERNO_BLAST))
        return INFERNO_BLAST;

    if (uint32 const bomb = SelectBomb(bot, target))
        return bomb;

    if (ctx.enemies >= 3 && CanTryCast(bot, FLAMESTRIKE))
        return FLAMESTRIKE;
    if (ctx.enemies >= 3 && bot->GetDistance(target) <= 10.0f && CanTryCast(bot, DRAGONS_BREATH))
        return DRAGONS_BREATH;

    if (CanTryCast(bot, FIREBALL))
        return FIREBALL;

    return 0;
}

uint32 SelectFrostMage(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    if (uint32 const armor = SelectKnownArmor(bot, FROST_ARMOR))
        return armor;
    if (!bot->GetPet() && CanTryCast(bot, SUMMON_WATER_ELEMENTAL))
        return SUMMON_WATER_ELEMENTAL;

    if (WantsCooldowns(ctx) && CanTryCast(bot, ICY_VEINS))
        return ICY_VEINS;
    if (uint32 const util = SelectMageUtility(ctx))
        return util;

    uint32 const fingers = AuraStacks(bot, FINGERS_OF_FROST);
    bool const brainFreeze = HasAuraUp(bot, BRAIN_FREEZE);

    // Spend Fingers of Frost before another Frostbolt. Brain Freeze (level 77)
    // is what makes Frostfire Bolt free; until then it is not the filler.
    if (fingers >= 1 && CanTryCast(bot, ICE_LANCE))
        return ICE_LANCE;
    if (brainFreeze && CanTryCast(bot, FROSTFIRE_BOLT))
        return FROSTFIRE_BOLT;

    if (uint32 const bomb = SelectBomb(bot, target))
        return bomb;

    if (CanTryCast(bot, FROZEN_ORB))
        return FROZEN_ORB;

    // Cone (12 yd) and Frost Nova (12 yd) only reach something already in melee.
    if (InSpellRange(bot, target, CONE_OF_COLD, 12.0f) && CanTryCast(bot, CONE_OF_COLD))
        return CONE_OF_COLD;
    if (CountEnemiesNear(bot, bot, 12.0f) >= 1 && CanTryCast(bot, FROST_NOVA))
        return FROST_NOVA;

    // Blizzard is level 52. Flamestrike (44, 8 yd around the target) covers a
    // pack until then. ctx.enemies is 1 while the aoe strategy is off.
    if (ctx.enemies >= 3)
    {
        if (!HasGroundEffect(bot, BLIZZARD) && CanTryCast(bot, BLIZZARD))
            return BLIZZARD;
        if (!bot->HasSpell(BLIZZARD) && !HasGroundEffect(bot, FLAMESTRIKE)
            && CanTryCast(bot, FLAMESTRIKE))
            return FLAMESTRIKE;
        if (CountEnemiesNear(bot, bot, 10.0f) >= 3 && CanTryCast(bot, ARCANE_EXPLOSION))
            return ARCANE_EXPLOSION;
    }

    // Fire Blast is the instant until Frozen Orb exists. After that it only
    // fills movement, so it does not replace Frostbolt.
    if ((!bot->HasSpell(FROZEN_ORB) || !bot->IsStopped())
        && InSpellRange(bot, target, FIRE_BLAST, 30.0f) && CanTryCast(bot, FIRE_BLAST))
        return FIRE_BLAST;
    if (CanTryCast(bot, FROSTBOLT))
        return FROSTBOLT;

    return 0;
}

} // namespace BotRotation
