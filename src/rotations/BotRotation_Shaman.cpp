/*
 * Elemental / Enhancement — modules/mod-playerbots/rotation.md
 * Unknown (low-level) spells are skipped via CanTryCast.
 * Restoration healers live in BotRotation_Healers.cpp — not modified here.
 */

#include "BotRotationLists.h"
#include "Creature.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Unit.h"

namespace BotRotation
{
namespace
{
    enum ShamanSpells : uint32
    {
        LIGHTNING_SHIELD     = 324,
        FLAME_SHOCK          = 8050,
        EARTH_SHOCK          = 8042,
        LIGHTNING_BOLT       = 403,
        CHAIN_LIGHTNING      = 421,
        EARTHQUAKE           = 61882,
        UNLEASH_ELEMENTS     = 73680,
        UNLEASH_FLAME        = 73683,
        SEARING_TOTEM        = 3599,
        MAGMA_TOTEM          = 8190,
        FIRE_ELEMENTAL_TOTEM = 2894,
        ASCENDANCE           = 114049,
        ASCENDANCE_ELE       = 114050,
        ASCENDANCE_ENH       = 114051,
        ELEMENTAL_MASTERY    = 16166,
        ELEMENTAL_BLAST      = 117014,

        // Elemental
        LAVA_BURST           = 51505,
        LAVA_SURGE           = 77756,

        // Enhancement
        WINDFURY_WEAPON      = 8232,
        FLAMETONGUE_WEAPON   = 8024,
        STORMSTRIKE          = 17364,
        PRIMAL_STRIKE        = 73899,  // level 3 until Stormstrike (26)
        LAVA_LASH            = 60103,
        MAELSTROM_WEAPON     = 53817,
        FERAL_SPIRIT         = 51533,
        FIRE_NOVA            = 1535,
    };

    bool HasMainhandWeaponImbue(Player* bot)
    {
        if (!bot)
            return false;
        if (Item* mh = bot->GetWeaponForAttack(WeaponAttackType::BASE_ATTACK))
            return mh->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) != 0;
        return false;
    }

    bool HasOffhandWeaponImbue(Player* bot)
    {
        if (!bot)
            return false;
        if (Item* oh = bot->GetWeaponForAttack(WeaponAttackType::OFF_ATTACK))
            return oh->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) != 0;
        return false;
    }

    // Searing / Magma / Fire Elemental share the fire slot; a totem left behind out of range counts as down.
    bool FireTotemActive(Player* bot, Unit* target)
    {
        uint64 const guid = bot->m_SummonSlot[SUMMON_SLOT_TOTEM];
        if (!guid)
            return false;
        Creature* totem = ObjectAccessor::GetCreature(*bot, guid);
        return totem && totem->IsAlive() && totem->IsWithinDistInMap(target, 25.0f);
    }

    uint32 SelectShamanCooldowns(Context const& ctx, bool ascendReady)
    {
        Player* bot = ctx.bot;
        if (!WantsCooldowns(ctx))
            return 0;
        if (CanTryCast(bot, FIRE_ELEMENTAL_TOTEM))
            return FIRE_ELEMENTAL_TOTEM;
        if (CanTryCast(bot, ELEMENTAL_MASTERY))
            return ELEMENTAL_MASTERY;
        if (ascendReady && !HasAuraUp(bot, ASCENDANCE_ELE) && !HasAuraUp(bot, ASCENDANCE_ENH)
            && CanTryCast(bot, ASCENDANCE))
            return ASCENDANCE;
        return 0;
    }
}

uint32 SelectElemental(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    if (!HasAuraUp(bot, LIGHTNING_SHIELD) && CanTryCast(bot, LIGHTNING_SHIELD))
        return LIGHTNING_SHIELD;

    float const flameShock = MyAuraRemains(bot, target, FLAME_SHOCK);
    bool const ascended = HasAuraUp(bot, ASCENDANCE_ELE);

    if (uint32 const cd = SelectShamanCooldowns(ctx, flameShock > 15.0f))
        return cd;

    if (flameShock < 3.0f && CanTryCast(bot, FLAME_SHOCK))
        return FLAME_SHOCK;

    if ((ascended || HasAuraUp(bot, LAVA_SURGE)) && CanTryCast(bot, LAVA_BURST))
        return LAVA_BURST;

    if (!FireTotemActive(bot, target) && CanTryCast(bot, SEARING_TOTEM))
        return SEARING_TOTEM;

    if (ctx.enemies >= 4 && CanTryCast(bot, EARTHQUAKE))
        return EARTHQUAKE;

    if (ctx.enemies >= 3)
    {
        if (AuraStacks(bot, LIGHTNING_SHIELD) >= 7 && CanTryCast(bot, EARTH_SHOCK))
            return EARTH_SHOCK;
        if (CanTryCast(bot, CHAIN_LIGHTNING))
            return CHAIN_LIGHTNING;
    }

    if (CanTryCast(bot, ELEMENTAL_BLAST))
        return ELEMENTAL_BLAST;
    if (flameShock > 0.0f && CanTryCast(bot, LAVA_BURST))
        return LAVA_BURST;
    // Fulmination: discharge the Lightning Shield charges at the cap.
    if (AuraStacks(bot, LIGHTNING_SHIELD) >= 7 && !ascended && CanTryCast(bot, EARTH_SHOCK))
        return EARTH_SHOCK;
    // Low level: Earth Shock as a damage spell before Fulmination.
    if (!bot->HasSpell(LAVA_BURST) && CanTryCast(bot, EARTH_SHOCK))
        return EARTH_SHOCK;

    if (CanTryCast(bot, LIGHTNING_BOLT))
        return LIGHTNING_BOLT;

    return 0;
}

uint32 SelectEnhancement(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    uint32 const mw = AuraStacks(bot, MAELSTROM_WEAPON);

    // Buffs: Windfury (main hand), Flametongue (off-hand), Lightning Shield.
    // The off-hand imbue only goes out once the main hand has one (see CastSpell).
    if (!HasMainhandWeaponImbue(bot))
    {
        if (CanTryCast(bot, WINDFURY_WEAPON))
            return WINDFURY_WEAPON;
        if (CanTryCast(bot, FLAMETONGUE_WEAPON))
            return FLAMETONGUE_WEAPON;
    }
    else if (bot->GetWeaponForAttack(WeaponAttackType::OFF_ATTACK) && !HasOffhandWeaponImbue(bot)
        && CanTryCast(bot, FLAMETONGUE_WEAPON))
        return FLAMETONGUE_WEAPON;
    if (!HasAuraUp(bot, LIGHTNING_SHIELD) && CanTryCast(bot, LIGHTNING_SHIELD))
        return LIGHTNING_SHIELD;

    bool const stormstrikeCd = bot->HasSpell(STORMSTRIKE) && bot->GetSpellCooldownDelay(STORMSTRIKE) >= 3;
    if (uint32 const cd = SelectShamanCooldowns(ctx, stormstrikeCd))
        return cd;
    if (WantsCooldowns(ctx) && CanTryCast(bot, FERAL_SPIRIT))
        return FERAL_SPIRIT;

    if (!FireTotemActive(bot, target))
    {
        if (ctx.enemies > 5 && bot->IsWithinDistInMap(target, 8.0f) && CanTryCast(bot, MAGMA_TOTEM))
            return MAGMA_TOTEM;
        if (CanTryCast(bot, SEARING_TOTEM))
            return SEARING_TOTEM;
    }

    bool const flameShockUp = HasMyAura(bot, target, FLAME_SHOCK);

    if (mw >= 1 && (mw >= 5 || ctx.enemies >= 2) && CanTryCast(bot, ELEMENTAL_BLAST))
        return ELEMENTAL_BLAST;

    if (ctx.enemies >= 2)
    {
        if (flameShockUp && ctx.enemies >= 3 && CanTryCast(bot, FIRE_NOVA))
            return FIRE_NOVA;
        if (mw >= 3 && CanTryCast(bot, CHAIN_LIGHTNING))
            return CHAIN_LIGHTNING;
        if (!flameShockUp && CanTryCast(bot, FLAME_SHOCK))
            return FLAME_SHOCK;
        if (flameShockUp && CanTryCast(bot, LAVA_LASH))
            return LAVA_LASH;
    }

    if (mw >= 5 && CanTryCast(bot, LIGHTNING_BOLT))
        return LIGHTNING_BOLT;
    if (CanTryCast(bot, STORMSTRIKE))
        return STORMSTRIKE;
    if (!bot->HasSpell(STORMSTRIKE) && CanTryCast(bot, PRIMAL_STRIKE))
        return PRIMAL_STRIKE;
    if (CanTryCast(bot, LAVA_LASH))
        return LAVA_LASH;
    if ((HasAuraUp(bot, UNLEASH_FLAME) || !flameShockUp) && CanTryCast(bot, FLAME_SHOCK))
        return FLAME_SHOCK;
    if (CanTryCast(bot, UNLEASH_ELEMENTS))
        return UNLEASH_ELEMENTS;
    if (CanTryCast(bot, EARTH_SHOCK))
        return EARTH_SHOCK;
    if (mw >= 2 && CanTryCast(bot, LIGHTNING_BOLT))
        return LIGHTNING_BOLT;

    return 0;
}

} // namespace BotRotation
