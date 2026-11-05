/*
 * Shared rotation helpers and spec dispatch.
 */

#include "BotRotation.h"
#include "BotRotationLists.h"
#include "Creature.h"

#include "DBCStores.h"
#include "Group.h"
#include "GroupReference.h"
#include "Item.h"
#include "ItemPrototype.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellTargeting.h"
#include "Unit.h"
#include "Util.h"

#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace BotRotation
{
namespace
{
    PlayerbotAI* GetAI(Player* bot)
    {
        return sPlayerbotMgr->GetBotAI(bot);
    }

    bool StrategyOn(Player* bot, char const* name)
    {
        if (PlayerbotAI* ai = GetAI(bot))
            return ai->HasStrategy(name, BotState::Combat);
        // No AI attached — allow utilities so self-tests still work.
        return true;
    }

    // Spells that should be cast on the bot (buffs / forms / seals).
    bool IsSelfCast(uint32 spellId)
    {
        switch (spellId)
        {
            // Self-defensives used by TryPartySupport (must route onto the bot).
            case 642:    // Divine Shield
            case 498:    // Divine Protection
            case 871:    // Shield Wall
            case 12975:  // Last Stand
            case 55694:  // Enraged Regeneration
            case 48792:  // Icebound Fortitude
            case 48707:  // Anti-Magic Shell
            case 55233:  // Vampiric Blood
            case 61336:  // Survival Instincts
            case 22812:  // Barkskin
            case 19263:  // Deterrence
            case 45438:  // Ice Block
            case 47585:  // Dispersion
            case 586:    // Fade
            case 31224:  // Cloak of Shadows
            case 5277:   // Evasion
            case 104773: // Unending Resolve
            case 108271: // Astral Shift
            case 115203: // Fortifying Brew
            case 122278: // Dampen Harm
            case 84963:  // Inquisition
            case 31801:  // Seal of Truth
            case 20154:  // Seal of Righteousness
            case 15473:  // Shadowform
            case 15286:  // Vampiric Embrace
            case 21562:  // Power Word: Fortitude
            case 588:    // Inner Fire
            case 324:    // Lightning Shield
            case 109773: // Dark Intent
            case 116740: // Tigereye Brew (consume)
            case 115288: // Energizing Brew
            case 74434:  // Soulburn
            case 1454:   // Life Tap
            case 113860: // Dark Soul: Misery
            case 82692:  // Focus Fire
            case 19574:  // Bestial Wrath
            case 3045:   // Rapid Fire
            case 82726:  // Fervor
            case 883:    // Call Pet
            case 982:    // Revive Pet
            case 136:    // Mend Pet
            case 13165:  // Aspect of the Hawk
            case 109260: // Aspect of the Iron Hawk
            case 20217:  // Blessing of Kings
            case 19740:  // Blessing of Might
            case 115921: // Legacy of the Emperor
            case 116781: // Legacy of the White Tiger
            case 16166:  // Elemental Mastery
            case 114049: // Ascendance (shaman)
            case 79206:  // Spiritwalker's Grace
            case 8024:   // Flametongue Weapon
            case 8232:   // Windfury Weapon
            case 2823:   // Deadly Poison (lethal)
            case 8679:   // Wound Poison (lethal)
            case 3408:   // Crippling Poison (non-lethal)
            case 108211: // Leeching Poison (non-lethal)
            case 108215: // Paralytic Poison (non-lethal)
            case 26573:  // Consecration
            case 768:    // Cat Form
            case 62078:  // Swipe (Cat), area around the caster
            case 52610:  // Savage Roar
            case 5217:   // Tiger's Fury
            case 106951: // Berserk (feral)
            case 6673:   // Battle Shout
            case 2458:   // Berserker Stance
            case 1719:   // Recklessness
            case 12328:  // Sweeping Strikes
            case 5171:   // Slice and Dice
            case 13750:  // Adrenaline Rush
            case 121471: // Shadow Blades
            case 13877:  // Blade Flurry
            case 12472:  // Icy Veins
            case 31687:  // Summon Water Elemental
            case 122:    // Frost Nova (centered on the caster)
            case 103958: // Metamorphosis
            case 49016:  // Unholy Frenzy
            case 63560:  // Dark Transformation
            case 51533:  // Feral Spirit
            case 2894:   // Fire Elemental Totem
            case 3599:   // Searing Totem
            case 8190:   // Magma Totem
            case 1535:   // Fire Nova
            case 1126:   // Mark of the Wild
            case 108683: // Fire and Brimstone
            case 18499:  // Berserker Rage
            case 107574: // Avatar
            case 12292:  // Bloodbath
            case 114207: // Skull Banner
            case 102560: // Incarnation: Chosen of Elune
            case 102543: // Incarnation: King of the Jungle
            case 1856:   // Vanish
            case 1949:   // Hellfire
            case 88751:  // Wild Mushroom: Detonate
            case 57330:  // Horn of Winter
            case 46584:  // Raise Dead
            case 113858: // Dark Soul: Instability
            case 113861: // Dark Soul: Knowledge
            case 24858:  // Moonkin Form
            case 5487:   // Bear Form
            case 71:     // Defensive Stance
            case 48263:  // Blood Presence
            case 48265:  // Unholy Presence
            case 48266:  // Frost Presence
            case 51271:  // Pillar of Frost
            case 45529:  // Blood Tap
            case 47568:  // Empower Rune Weapon
            case 48982:  // Rune Tap
            case 115989: // Unholy Blight
            case 20165:  // Seal of Insight
            case 20164:  // Seal of Justice
            case 6117:   // Mage Armor
            case 7302:   // Frost Armor
            case 55342:  // Mirror Image
            case 12042:  // Arcane Power
            case 12051:  // Evocation
            case 30482:  // Molten Armor
            case 1459:   // Arcane Brilliance
            case 51713:  // Shadow Dance
            case 49222:  // Bone Shield
            case 115069: // Stance of the Sturdy Ox
            case 115070: // Stance of the Wise Serpent
            case 103985: // Stance of the Fierce Tiger
            case 123761: // Mana Tea (glyphed)
            case 115295: // Guard
            case 119582: // Purifying Brew
            case 115308: // Elusive Brew
            case 115399: // Chi Brew
            case 115072: // Expel Harm
            case 116847: // Rushing Jade Wind
            case 101546: // Spinning Crane Kick
            case 2565:   // Shield Block
            case 1160:   // Demoralizing Shout
            case 62606:  // Savage Defense
            case 22842:  // Frenzied Regeneration
            case 5229:   // Enrage (bear)
            case 50334:  // Berserk (bear)
            case 112071: // Celestial Alignment
            case 124974: // Nature's Vigil
            case 48505:  // Starfall
            case 132404: // Shield Block (MoP buff)
            case 54428:  // Divine Plea
            case 31884:  // Avenging Wrath
            case 31842:  // Divine Favor
            case 105809: // Holy Avenger
            case 86698:  // Guardian of Ancient Kings (Retribution)
            case 86659:  // Guardian of Ancient Kings (Protection)
            case 31850:  // Ardent Defender
            case 20572:  // Blood Fury (AP)
            case 33702:  // Blood Fury (SP)
            case 33697:  // Blood Fury (both)
            case 26297:  // Berserking
            case 28730:  // Arcane Torrent (mana)
            case 25046:  // Arcane Torrent (energy)
            case 50613:  // Arcane Torrent (runic)
            case 69179:  // Arcane Torrent (rage)
            case 80483:  // Arcane Torrent (focus)
            case 129597: // Arcane Torrent (chi)
            case 69041:  // Rocket Barrage
            case 20549:  // War Stomp
            case 59752:  // Every Man for Himself
            case 7744:   // Will of the Forsaken
            case 20594:  // Stoneform
            case 20589:  // Escape Artist
            case 5394:   // Healing Stream Totem
            case 109964: // Spirit Shell
            case 10060:  // Power Infusion
            case 64843:  // Divine Hymn
            case 33891:  // Tree of Life
            case 115294: // Mana Tea
            case 116680: // Thunder Focus Tea
            case 132158: // Nature's Swiftness (druid)
            case 52127:  // Water Shield
                return true;
            default:
                return false;
        }
    }

    // Ally-targeted heals a DPS/tank rotation casts on itself. Kept out of
    // IsSelfCast so healer specs can still put them on party members.
    bool IsSelfCastInRotation(uint32 spellId)
    {
        switch (spellId)
        {
            case 20925:  // Sacred Shield
            case 114163: // Eternal Flame
            case 85673:  // Word of Glory
            case 124081: // Zen Sphere
                return true;
            default:
                return IsSelfCast(spellId);
        }
    }

    uint32 InterruptSpellForClass(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return 6552;   // Pummel
            case CLASS_ROGUE:        return 1766;   // Kick
            case CLASS_MAGE:         return 2139;   // Counterspell
            case CLASS_SHAMAN:       return 57994;  // Wind Shear
            case CLASS_DEATH_KNIGHT: return 47528;  // Mind Freeze
            case CLASS_PALADIN:      return 96231;  // Rebuke
            case CLASS_MONK:         return 116705; // Spear Hand Strike
            case CLASS_DRUID:        return 106839; // Skull Bash
            case CLASS_HUNTER:       return 147362; // Counter Shot
            case CLASS_PRIEST:       return 15487;  // Silence
            case CLASS_WARLOCK:      return 119910; // Spell Lock (Command Demon)
            default:                 return 0;
        }
    }

    float UnitHealthPct(Unit const* unit)
    {
        if (!unit || !unit->GetMaxHealth())
            return 100.0f;
        return 100.0f * float(unit->GetHealth()) / float(unit->GetMaxHealth());
    }
}

bool IsSelfCastSpell(uint32 spellId)
{
    return IsSelfCast(spellId);
}

uint32 CountNearbyEnemies(Player* bot, float range)
{
    if (!bot)
        return 0;

    std::list<Unit*> list;
    Skyfire::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, range);
    Skyfire::UnitListSearcher<Skyfire::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, list, check);
    bot->VisitNearbyObject(range, searcher);

    uint32 count = 0;
    for (Unit* u : list)
        if (u && u->IsAlive() && bot->IsValidAttackTarget(u))
            ++count;
    return count ? count : 1;
}

uint32 CountEnemiesNear(Player* bot, Unit* around, float range)
{
    if (!bot || !around || range <= 0.0f)
        return 0;

    std::list<Unit*> list;
    Skyfire::AnyUnfriendlyUnitInObjectRangeCheck check(around, bot, range);
    Skyfire::UnitListSearcher<Skyfire::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, list, check);
    around->VisitNearbyObject(range, searcher);

    uint32 count = 0;
    bool sawCenter = false;
    for (Unit* u : list)
    {
        if (!u || !u->IsAlive() || !bot->IsValidAttackTarget(u))
            continue;
        if (u == around)
            sawCenter = true;
        ++count;
    }
    // The grid search does not always yield the unit it is centered on.
    if (!sawCenter && around->IsAlive() && bot->IsValidAttackTarget(around))
        ++count;
    return count;
}

bool WantsCooldowns(Context const& ctx)
{
    if (!ctx.bot || !ctx.target || !StrategyOn(ctx.bot, "boost"))
        return false;
    if (ctx.target->GetTypeId() == TypeID::TYPEID_PLAYER)
        return true;
    if (Creature const* creature = ctx.target->ToCreature())
        if (creature->isWorldBoss() || creature->IsDungeonBoss() || creature->isElite())
            return true;
    // Ranged bots stand outside the pack, so also count enemies on the target.
    if (CountNearbyEnemies(ctx.bot, 10.0f) >= 3)
        return true;
    return CountEnemiesNear(ctx.bot, ctx.target, 8.0f) >= 3;
}

bool HasGroundEffect(Player* bot, uint32 spellId)
{
    return bot && bot->GetDynObject(spellId) != nullptr;
}

Unit* FindSecondaryEnemy(Player* bot, Unit* exclude, float range)
{
    if (!bot)
        return nullptr;

    std::list<Unit*> list;
    Skyfire::AnyUnfriendlyUnitInObjectRangeCheck check(bot, bot, range);
    Skyfire::UnitListSearcher<Skyfire::AnyUnfriendlyUnitInObjectRangeCheck> searcher(bot, list, check);
    bot->VisitNearbyObject(range, searcher);

    Unit* best = nullptr;
    float bestDist = 0.0f;
    for (Unit* u : list)
    {
        if (!u || u == exclude || !u->IsAlive() || !bot->IsValidAttackTarget(u))
            continue;
        float const dist = bot->GetDistance(u);
        if (!best || dist < bestDist)
        {
            best = u;
            bestDist = dist;
        }
    }
    return best;
}

uint32 AuraIdForSpell(uint32 castOrAuraId)
{
    if (!castOrAuraId)
        return 0;

    // Cast spell → applied aura when they differ (Hekili / Spell.dbc).
    switch (castOrAuraId)
    {
        case 172:    return 146739; // Corruption
        case 1978:   return 118253; // Serpent Sting
        case 2565:   return 132404; // Shield Block
        default:
            break;
    }

    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(castOrAuraId))
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& eff = info->Effects[i];
            if (!eff.Effect)
                continue;
            // Instant-apply auras use the cast ID as the aura ID.
            if (eff.IsAura())
                return castOrAuraId;
            if (eff.Effect == SPELL_EFFECT_TRIGGER_SPELL
                || eff.Effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE)
            {
                if (eff.TriggerSpell)
                    return eff.TriggerSpell;
            }
        }
    }
    return castOrAuraId;
}

float AuraRemains(Unit* unit, uint32 spellId)
{
    if (!unit)
        return 0.0f;
    uint32 const auraId = AuraIdForSpell(spellId);
    Aura* aura = unit->GetAuraOfRankedSpell(auraId);
    if (!aura)
        aura = unit->GetAura(auraId);
    if (!aura && auraId != spellId)
    {
        aura = unit->GetAuraOfRankedSpell(spellId);
        if (!aura)
            aura = unit->GetAura(spellId);
    }
    if (!aura)
        return 0.0f;
    if (aura->IsPermanent() || aura->GetDuration() < 0)
        return 9999.0f;
    return aura->GetDuration() / 1000.0f;
}

float MyAuraRemains(Player* bot, Unit* unit, uint32 spellId)
{
    if (!bot || !unit)
        return 0.0f;
    uint32 const auraId = AuraIdForSpell(spellId);
    Aura* aura = unit->GetAura(auraId, bot->GetGUID());
    if (!aura)
        aura = unit->GetAuraOfRankedSpell(auraId, bot->GetGUID());
    if (!aura && auraId != spellId)
    {
        aura = unit->GetAura(spellId, bot->GetGUID());
        if (!aura)
            aura = unit->GetAuraOfRankedSpell(spellId, bot->GetGUID());
    }
    if (!aura)
        return 0.0f;
    if (aura->IsPermanent() || aura->GetDuration() < 0)
        return 9999.0f;
    return aura->GetDuration() / 1000.0f;
}

bool HasAuraUp(Unit* unit, uint32 spellId)
{
    if (!unit)
        return false;
    uint32 const auraId = AuraIdForSpell(spellId);
    if (unit->HasAura(auraId) || unit->GetAuraOfRankedSpell(auraId))
        return true;
    if (auraId != spellId
        && (unit->HasAura(spellId) || unit->GetAuraOfRankedSpell(spellId)))
        return true;
    return false;
}

bool HasMyAura(Player* bot, Unit* unit, uint32 spellId)
{
    if (!bot || !unit)
        return false;
    uint32 const auraId = AuraIdForSpell(spellId);
    if (unit->HasAura(auraId, bot->GetGUID())
        || unit->GetAuraOfRankedSpell(auraId, bot->GetGUID()))
        return true;
    if (auraId != spellId
        && (unit->HasAura(spellId, bot->GetGUID())
            || unit->GetAuraOfRankedSpell(spellId, bot->GetGUID())))
        return true;
    return false;
}

bool NeedsMyAuraRefresh(Player* bot, Unit* unit, uint32 spellId, float refreshAt)
{
    if (!bot || !unit)
        return true;
    float const remains = MyAuraRemains(bot, unit, spellId);
    if (remains <= 0.0f)
        return true;
    return remains <= refreshAt;
}

bool HasShieldEquipped(Player* bot)
{
    if (!bot)
        return false;
    Item* offhand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (!offhand || !offhand->GetTemplate())
        return false;
    return offhand->GetTemplate()->InventoryType == INVTYPE_SHIELD;
}

uint32 AuraStacks(Unit* unit, uint32 spellId)
{
    if (!unit)
        return 0;
    uint32 const auraId = AuraIdForSpell(spellId);
    if (Aura* aura = unit->GetAura(auraId))
        return aura->GetStackAmount();
    if (auraId != spellId)
        if (Aura* aura = unit->GetAura(spellId))
            return aura->GetStackAmount();
    return 0;
}

bool SpellReady(Player* bot, uint32 spellId)
{
    if (!bot || !spellId)
        return false;
    if (!bot->HasSpell(spellId))
        return false;
    if (bot->HasSpellCooldown(spellId))
        return false;
    if (PlayerbotAI* ai = GetAI(bot))
        if (ai->IsSpellBackedOff(spellId))
            return false;
    return true;
}

// True when the bot can afford the SpellPower.dbc cost for this spell.
bool HasRunesFor(Player* bot, uint32 spellId)
{
    if (bot->getClass() != CLASS_DEATH_KNIGHT)
        return true;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info || !info->RuneCostID)
        return true;
    SpellRuneCostEntry const* src = sSpellRuneCostStore.LookupEntry(info->RuneCostID);
    if (!src || src->NoRuneCost())
        return true;

    int32 runeCost[uint8(RuneType::NUM_RUNE_TYPES)];
    for (uint8 i = 0; i < uint8(RuneType::NUM_RUNE_TYPES); ++i)
        runeCost[i] = src->RuneCost[i];
    runeCost[uint8(RuneType::RUNE_DEATH)] = 0;

    uint32 readyDeath = 0;
    for (uint8 i = 0; i < MAX_RUNES; ++i)
    {
        if (bot->GetRuneCooldown(i) != 0)
            continue;
        uint8 const rune = uint8(bot->GetCurrentRune(i));
        if (rune == uint8(RuneType::RUNE_DEATH))
            ++readyDeath;
        else if (runeCost[rune] > 0)
            --runeCost[rune];
    }

    uint32 missing = 0;
    for (uint8 i = 0; i < uint8(RuneType::RUNE_DEATH); ++i)
        if (runeCost[i] > 0)
            missing += uint32(runeCost[i]);
    return missing <= readyDeath;
}

bool CanAffordSpell(Player* bot, uint32 spellId)
{
    if (!bot || !spellId)
        return false;
    if (!HasRunesFor(bot, spellId))
        return false;

    for (uint32 i = 0; i < sSpellPowerStore.GetNumRows(); ++i)
    {
        SpellPowerEntry const* power = sSpellPowerStore.LookupEntry(i);
        if (!power || power->spellId != spellId)
            continue;
        if (power->ShapeShiftSpellID && !bot->HasAura(power->ShapeShiftSpellID))
            continue;

        int32 cost = int32(power->manaCost);
        // Rage/energy/runic are stored ×10 in DBC; GetPower uses the same scale.
        if (cost <= 0 && power->ManaCostPercentageFloat <= 0.0f)
            return true;

        Powers const pt = Powers(power->powerType);
        if (pt == POWER_HEALTH)
        {
            if (bot->GetHealth() <= uint32(cost))
                return false;
            return true;
        }
        if (pt >= MAX_POWERS)
            return true;

        if (power->ManaCostPercentageFloat > 0.0f
            && (pt == POWER_MANA || pt == POWER_DEMONIC_FURY))
            cost += int32(CalculatePct(bot->GetMaxPower(pt), power->ManaCostPercentageFloat));

        if (cost > 0 && bot->GetPower(pt) < cost)
            return false;
        return true;
    }
    return true; // no SpellPower row → treat as free
}

bool CanTryCast(Player* bot, uint32 spellId)
{
    if (!SpellReady(bot, spellId))
        return false;

    if (bot->HasUnitState(UNIT_STATE_CASTING) && !bot->IsNonMeleeSpellCasted(false, false, true))
        bot->ClearUnitState(UNIT_STATE_CASTING);

    // Block on a real cast-time bar / channel. Pending GCD-instants are finished
    // immediately in CastSpell() — do not treat them as busy here.
    if (Spell* generic = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        if (generic->getState() != SPELL_STATE_FINISHED
            && generic->getState() != SPELL_STATE_DELAYED
            && generic->GetCastTime() > 0)
            return false;
    }
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        return false;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
        return false;
    if (info->StartRecoveryTime > 0 && bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
        return false;
    // Stance-locked spells (monk Jab outside a stance) have no matching SpellPower row and would
    // pass the cost check, then fail in the core every tick.
    if (info->CheckShapeshift(bot->GetShapeshiftForm()) != SpellCastResult::SPELL_CAST_OK)
        return false;
    if (!CanAffordSpell(bot, spellId))
        return false;
    return true;
}

bool ReadyToShapeshift(Player* bot, uint32 formSpellId)
{
    if (!bot || HasAuraUp(bot, formSpellId) || !CanTryCast(bot, formSpellId))
        return false;
    if (bot->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
        bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
    return true;
}

bool BreakFormForSpell(Player* bot, uint32 spellId)
{
    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!bot || !info || !bot->HasAuraType(SPELL_AURA_MOD_SHAPESHIFT))
        return false;
    if (info->CheckShapeshift(bot->GetShapeshiftForm()) == SpellCastResult::SPELL_CAST_OK)
        return false;
    bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
    return true;
}

// Face + stop so SPELL_FAILED_UNIT_NOT_INFRONT / moving-cast failures don't
// silently no-op (bots previously called CastSpell and assumed success).
void PrepareHostileCast(Player* bot, Unit* castTarget)
{
    if (!bot || !castTarget || castTarget == bot)
        return;
    // Auto Shot is background — do not block facing for abilities.
    if (bot->IsNonMeleeSpellCasted(false, false, true))
        return;

    // Plant for cast-time / channeled fillers. Self-bot (player in bot mode)
    // previously skipped StopMoving, so only instant DoTs landed while fillers
    // failed every GCD. Brief plant is intentional assist takeover.
    if (!bot->IsStopped())
        bot->StopMoving();

    bot->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
    bot->SetSelection(castTarget->GetGUID());
    if (!bot->HasInArc(static_cast<float>(M_PI), castTarget))
        bot->SetInFront(castTarget);
}

// True only if THIS spell actually started this call (not leftover GCD/CD).
bool CastStarted(Player* bot, SpellInfo const* info, uint32 spellId,
    bool hadSpellCd, bool hadGcd)
{
    if (!bot || !info)
        return false;
    if (bot->FindCurrentSpellBySpellId(spellId))
        return true;
    // Instant finished this frame: spell CD or GCD newly applied by this cast.
    if (!hadSpellCd && (bot->HasSpellCooldown(spellId) || bot->GetSpellCooldownDelay(spellId) > 0))
        return true;
    if (!hadGcd && info->StartRecoveryTime > 0 && bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
        return true;
    return false;
}

bool CastSpell(Player* bot, Unit* enemy, uint32 spellId)
{
    if (!bot || !spellId || !CanTryCast(bot, spellId))
        return false;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
        return false;

    // Only force self for known self-buffs / ground AoE. Do NOT use
    // !NeedsExplicitUnitTarget() — that redirected many damage spells onto the
    // caster and made every cast fail (no abilities on recount).
    // Cone of Cold has no unit target; it hits whatever is in front of the caster.
    bool const cone = spellId == 120;
    Unit* castTarget = (IsSelfCastInRotation(spellId) || cone) ? static_cast<Unit*>(bot) : enemy;
    if (!castTarget)
        return false;

    // Havoc marks a *secondary* target so Chaos Bolt / Incinerate cleave onto it.
    if (spellId == 80240) // Havoc
    {
        if (Unit* secondary = FindSecondaryEnemy(bot, enemy, 40.0f))
            castTarget = secondary;
        else
            return false;
    }

    // Off-GCD spells (Taunt, etc.) must not StopMoving — that cancels Charge mid-flight
    // when we fire taunt in the same tick after a successful Charge.
    bool const offGcd = info->StartRecoveryTime == 0;
    if (cone && enemy)
        PrepareHostileCast(bot, enemy);
    else if (castTarget != bot)
    {
        if (offGcd)
        {
            bot->SetSelection(castTarget->GetGUID());
            if (!bot->HasInArc(static_cast<float>(M_PI), castTarget))
                bot->SetInFront(castTarget);
        }
        else
            PrepareHostileCast(bot, castTarget);
    }

    bool const hadSpellCd = bot->HasSpellCooldown(spellId) || bot->GetSpellCooldownDelay(spellId) > 0;
    bool const hadGcd = info->StartRecoveryTime > 0 && bot->GetGlobalCooldownMgr().HasGlobalCooldown(info);
    uint32 const focusBefore = bot->GetPower(POWER_FOCUS);

    // Imbues cast without an item target land on the main hand; send Flametongue to an
    // unimbued off-hand once the main hand already carries its imbue.
    Item* offhand = spellId == 8024 ? bot->GetWeaponForAttack(WeaponAttackType::OFF_ATTACK) : nullptr;
    Item* mainhand = offhand ? bot->GetWeaponForAttack(WeaponAttackType::BASE_ATTACK) : nullptr;
    if (offhand && mainhand && mainhand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT)
        && !offhand->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
    {
        SpellCastTargets targets;
        targets.SetItemTarget(offhand);
        bot->CastSpell(targets, info, nullptr);
    }
    // Ground-targeted spells without a destination land on the caster, so aim them at the enemy.
    else if (castTarget != bot && (info->GetExplicitTargetMask() & TARGET_FLAG_DEST_LOCATION))
        bot->CastSpell(castTarget->GetPositionX(), castTarget->GetPositionY(), castTarget->GetPositionZ(),
            spellId, false);
    else
        bot->CastSpell(castTarget, spellId, false);

    // Instants with a GCD stay in PREPARING until a 1ms spell event fires.
    // Hunter "instants" also get a 500ms ammo-delay cast time — finish those too.
    if (Spell* pending = bot->FindCurrentSpellBySpellId(spellId))
    {
        bool const hunterInstant = (spellId == 3044 || spellId == 1978);
        if (pending->getState() == SPELL_STATE_PREPARING
            && (pending->GetCastTime() <= 0 || hunterInstant))
            pending->cast(true);
    }

    bool ok = CastStarted(bot, info, spellId, hadSpellCd, hadGcd);

    // Hunter focus spenders: only reject if the spell already left the cast slot
    // without spending focus. Arcane/Serpent get a 500ms ammo-delay cast time —
    // focus is taken on cast(), so a mid-bar check falsely cancelled GCD/Steady.
    if (ok && (spellId == 3044 || spellId == 1978)
        && !bot->FindCurrentSpellBySpellId(spellId)
        && bot->GetPower(POWER_FOCUS) >= focusBefore
        && info->StartRecoveryTime > 0)
    {
        bot->GetGlobalCooldownMgr().CancelGlobalCooldown(info);
        ok = false;
    }

    if (ok)
    {
        if (PlayerbotAI* ai = GetAI(bot))
        {
            char const* name = info->SpellName ? info->SpellName : "spell";
            ai->DebugCombat(std::string("Casting ") + name);
            ai->NoteSpellCast(spellId, castTarget->GetGUID());
        }
    }
    else if (PlayerbotAI* ai = GetAI(bot))
    {
        if (ai->IsCombatDebug())
        {
            char const* name = info->SpellName ? info->SpellName : "spell";
            ai->DebugCombat(std::string("FAILED ") + name);
        }
    }
    return ok;
}

// Hunter GCD shot. Auto Shot is separate and stays channeling — we never cancel it.
// Rotation: Arcane while focus >= 30, else Steady.
bool CastHunterShot(Player* bot, Unit* target, uint32 spellId)
{
    if (!bot || !target || !spellId)
        return false;

    PlayerbotAI* ai = GetAI(bot);
    auto log = [ai](std::string const& line)
    {
        if (ai)
            ai->HunterDebugLog(line);
    };

    if (!bot->HasSpell(spellId))
    {
        if (spellId == 3044 || spellId == 56641 || spellId == 1978 || spellId == 77767)
            bot->learnSpell(spellId, false);
        if (!bot->HasSpell(spellId))
        {
            log(std::string("Cast FAIL no-spell id=") + std::to_string(spellId));
            return false;
        }
    }

    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
        return false;

    char const* name = info->SpellName ? info->SpellName : "spell";

    if (bot->HasSpellCooldown(spellId))
    {
        log(std::string("Cast SKIP cd ") + name);
        return false;
    }
    if (info->StartRecoveryTime > 0 && bot->GetGlobalCooldownMgr().HasGlobalCooldown(info))
    {
        log(std::string("Cast SKIP gcd ") + name);
        return false;
    }

    // Do not clip Steady. FINISHED leftovers can be interrupted.
    // DELAYED projectiles (Arcane ammo/travel): do NOT InterruptSpell — that
    // cancels the missile. CastSpell/SetCurrentCastedSpell detaches DELAYED
    // without canceling when the next GCD shot prepares.
    if (Spell* generic = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        uint32 const st = generic->getState();
        if (st == SPELL_STATE_PREPARING && generic->GetCastTime() > 0)
        {
            log(std::string("Cast BLOCKED preparing ") + name
                + " " + (ai ? ai->BuildHunterStateSnapshot(target) : ""));
            return false;
        }
        if (st == SPELL_STATE_FINISHED)
        {
            log(std::string("Cast CLEAR leftover GEN FINISHED id=")
                + std::to_string(generic->GetSpellInfo()->Id));
            bot->InterruptSpell(CURRENT_GENERIC_SPELL, false, true);
        }
        else if (st == SPELL_STATE_DELAYED)
        {
            log(std::string("Cast NOTE GEN DELAYED id=")
                + std::to_string(generic->GetSpellInfo()->Id)
                + " (leave missile; next prepare detaches)");
        }
    }

    if (bot->HasUnitState(UNIT_STATE_CASTING) && !bot->IsNonMeleeSpellCasted(false, false, true))
        bot->ClearUnitState(UNIT_STATE_CASTING);

    bot->SetSelection(target->GetGUID());
    if (!bot->HasInArc(static_cast<float>(M_PI), target))
        bot->SetInFront(target);

    {
        Spell* probe = new Spell(bot, info, TRIGGERED_NONE);
        probe->m_targets.SetUnitTarget(target);
        SpellCastResult const pre = probe->CheckCast(true);
        delete probe;
        if (pre != SpellCastResult::SPELL_CAST_OK)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Cast CheckCast FAIL %s check=%d %s",
                name, int(pre), ai ? ai->BuildHunterStateSnapshot(target).c_str() : "");
            log(buf);
            if (ai && ai->IsCombatDebug())
            {
                char chat[96];
                std::snprintf(chat, sizeof(chat), "FAILED %s check=%d", name, int(pre));
                ai->DebugCombat(chat);
            }
            return false;
        }
    }

    uint32 const focusBefore = bot->GetPower(POWER_FOCUS);
    log(std::string("Cast PREPARE ") + name + " focusBefore=" + std::to_string(focusBefore)
        + " " + (ai ? ai->BuildHunterStateSnapshot(target) : ""));
    bot->CastSpell(target, spellId, false);

    // Success: bar started, or Arcane already spent its 30 focus.
    bool const foundCurrent = bot->FindCurrentSpellBySpellId(spellId) != nullptr;
    uint32 const focusAfter = bot->GetPower(POWER_FOCUS);
    bool const focusDropped = (spellId == 3044 || spellId == 1978) && focusAfter < focusBefore;
    bool const ok = foundCurrent || focusDropped;

    {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "Cast %s %s foundCurrent=%d focus %u->%u %s",
            ok ? "OK" : "FAIL",
            name,
            foundCurrent ? 1 : 0,
            focusBefore,
            focusAfter,
            ai ? ai->BuildHunterStateSnapshot(target).c_str() : "");
        log(buf);
    }

    if (ai)
    {
        if (ok)
        {
            ai->DebugCombat(std::string("Casting ") + name);
            ai->NoteSpellCast(spellId, target->GetGUID());
        }
        else if (ai->IsCombatDebug())
            ai->DebugCombat(std::string("FAILED ") + name);
    }
    return ok;
}

bool CastHealSpell(Player* bot, Player* ally, uint32 spellId)
{
    if (!bot || !spellId || !CanTryCast(bot, spellId))
        return false;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
        return false;

    Unit* castTarget = IsSelfCast(spellId) ? static_cast<Unit*>(bot) : static_cast<Unit*>(ally);
    if (!castTarget)
        return false;

    if (castTarget != bot)
    {
        // StopMoving interrupts an in-progress cast — only plant when idle.
        if (!bot->IsNonMeleeSpellCasted(false, false, true) && !bot->HasUnitState(UNIT_STATE_CASTING))
        {
            if (!bot->IsStopped())
                bot->StopMoving();
            bot->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
            bot->SetSelection(castTarget->GetGUID());
            if (!bot->HasInArc(static_cast<float>(M_PI), castTarget))
                bot->SetInFront(castTarget);
        }
    }

    bot->CastSpell(castTarget, spellId, false);
    bool const hadSpellCd = false; // CanTryCast already required !HasSpellCooldown
    bool const hadGcd = false;
    return CastStarted(bot, info, spellId, hadSpellCd, hadGcd);
}

bool TryInterrupt(Player* bot, Unit* target)
{
    if (!bot || !target || !target->IsAlive())
        return false;
    if (!target->IsNonMeleeSpellCasted(false))
        return false;

    uint32 interruptId = InterruptSpellForClass(bot->getClass());
    // Hunter fallback to Silencing Shot when Counter Shot is unknown.
    if (bot->getClass() == CLASS_HUNTER && !SpellReady(bot, interruptId))
        interruptId = 34490;
    // Warlock Optical Blast fallback.
    if (bot->getClass() == CLASS_WARLOCK && !SpellReady(bot, interruptId))
        interruptId = 119911;

    if (!CanTryCast(bot, interruptId))
        return false;

    return CastSpell(bot, target, interruptId);
}

bool TryRacial(Player* bot, Unit* /*targetOrSelf*/)
{
    if (!bot || bot->HasUnitState(UNIT_STATE_CASTING))
        return false;

    float const hpPct = UnitHealthPct(bot);

    auto trySelf = [&](uint32 id) -> bool
    {
        if (!CanTryCast(bot, id))
            return false;
        return CastSpell(bot, bot, id);
    };

    // Defensive / escape racials when low (not gated by boost).
    if (hpPct < 35.0f)
    {
        static uint32 const defensive[] = {
            59752, // Every Man for Himself
            7744,  // Will of the Forsaken
            20594, // Stoneform
            20589, // Escape Artist
        };
        for (uint32 id : defensive)
            if (trySelf(id))
                return true;
    }

    if (!bot->IsInCombat())
        return false;

    // Offensive racials require +boost.
    if (!StrategyOn(bot, "boost"))
        return false;

    static uint32 const offensive[] = {
        33697,  // Blood Fury (both)
        20572,  // Blood Fury (AP)
        33702,  // Blood Fury (SP)
        26297,  // Berserking
        69041,  // Rocket Barrage
        28730,  // Arcane Torrent (mana)
        25046,  // Arcane Torrent (energy)
        50613,  // Arcane Torrent (runic)
        69179,  // Arcane Torrent (rage)
        80483,  // Arcane Torrent (focus)
        129597, // Arcane Torrent (chi)
    };
    for (uint32 id : offensive)
        if (trySelf(id))
            return true;

    if (CountNearbyEnemies(bot, 8.0f) >= 2 && trySelf(20549)) // War Stomp
        return true;

    return false;
}

bool IsBursting(Player* bot)
{
    if (!bot)
        return false;

    // Major DPS cooldown buffs — trinkets should land in these windows.
    static uint32 const kBurstAuras[] =
    {
        13750,  // Adrenaline Rush
        121471, // Shadow Blades
        79140,  // Vendetta (debuff on target — also checked below)
        51713,  // Shadow Dance
        114050, // Ascendance (Elemental)
        114051, // Ascendance (Enhancement)
        114052, // Ascendance (Resto)
        16166,  // Elemental Mastery
        12472,  // Icy Veins
        12042,  // Arcane Power
        48108,  // Hot Streak (not a CD but burst)
        3045,   // Rapid Fire
        19574,  // Bestial Wrath
        121818, // Stampede
        1719,   // Recklessness
        107574, // Avatar
        46924,  // Bladestorm
        31884,  // Avenging Wrath
        105809, // Holy Avenger
        51271,  // Pillar of Frost
        49206,  // Summon Gargoyle / Dark Transformation window proxy
        49016,  // Unholy Frenzy
        103958, // Metamorphosis
        113860, // Dark Soul: Misery
        113861, // Dark Soul: Knowledge
        113858, // Dark Soul: Instability
        106951, // Berserk (Feral)
        102543, // Incarnation: King of the Jungle
        102560, // Incarnation: Chosen of Elune
        112071, // Celestial Alignment
        116740, // Tigereye Brew
        115288, // Energizing Brew
    };

    for (uint32 id : kBurstAuras)
        if (HasAuraUp(bot, id))
            return true;

    if (Unit* victim = bot->GetVictim())
        if (HasAuraUp(victim, 79140)) // Vendetta
            return true;

    return false;
}

bool HasGlyphSpell(Player* bot, uint32 glyphSpellId)
{
    if (!bot || !glyphSpellId)
        return false;
    if (bot->HasSpell(glyphSpellId) || bot->HasAura(glyphSpellId))
        return true;

    for (uint8 slot = 0; slot < MAX_GLYPH_SLOT_INDEX; ++slot)
    {
        uint32 const glyph = bot->GetGlyph(bot->GetActiveSpec(), slot);
        if (!glyph)
            continue;
        if (GlyphPropertiesEntry const* gp = sGlyphPropertiesStore.LookupEntry(glyph))
            if (gp->SpellId == glyphSpellId)
                return true;
    }
    return false;
}

bool TryTrinkets(Player* bot)
{
    if (!bot || bot->HasUnitState(UNIT_STATE_CASTING))
        return false;
    if (!StrategyOn(bot, "boost"))
        return false;

    for (uint8 slot = EQUIPMENT_SLOT_TRINKET1; slot <= EQUIPMENT_SLOT_TRINKET2; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            continue;

        for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            _Spell const& data = proto->Spells[i];
            if (data.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE || !data.SpellId)
                continue;

            if (bot->HasSpellCooldown(data.SpellId) || bot->GetSpellCooldownDelay(data.SpellId) > 0)
                continue;

            if (bot->CanUseItem(item) != InventoryResult::EQUIP_ERR_OK)
                continue;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(data.SpellId);
            if (!info)
                continue;

            SpellCastTargets targets;
            Unit* victim = bot->GetVictim();
            if (info->NeedsExplicitUnitTarget() && victim && bot->IsValidAttackTarget(victim))
                targets.SetUnitTarget(victim);
            else
                targets.SetUnitTarget(bot);

            bot->CastItemUseSpell(item, targets, 0, 0);
            if (bot->HasSpellCooldown(data.SpellId) || bot->GetSpellCooldownDelay(data.SpellId) > 0)
                return true;
        }
    }
    return false;
}

namespace
{
    bool HasAnyAura(Unit* unit, std::initializer_list<uint32> ids)
    {
        if (!unit)
            return false;
        for (uint32 id : ids)
            if (unit->HasAura(id))
                return true;
        return false;
    }

    bool TryCastBuff(Player* bot, uint32 spellId)
    {
        if (!CanTryCast(bot, spellId))
            return false;
        return CastSpell(bot, bot, spellId);
    }

    // Growl pulls threat off the tank and gets the pet killed; only keep it for solo hunters.
    void SetPetGrowl(Pet* pet, bool enabled)
    {
        enum : uint32 { GROWL = 2649 };

        SpellInfo const* growl = sSpellMgr->GetSpellInfo(GROWL);
        if (!growl)
            return;

        bool on = false;
        for (uint8 i = 0; i < pet->GetPetAutoSpellSize() && !on; ++i)
            on = pet->GetPetAutoSpellOnPos(i) == GROWL;
        if (on == enabled)
            return;

        pet->ToggleAutocast(growl, enabled);
        if (CharmInfo* charmInfo = pet->GetCharmInfo())
            charmInfo->SetSpellAutocast(growl, enabled);
    }

    // Call Pet 1 brings out the slot 0 pet; once that pet has died only Revive Pet
    // summons it again. Failed attempts back off so the other one gets its turn.
    bool TryHunterPet(Player* bot)
    {
        enum : uint32 { CALL_PET_1 = 883, REVIVE_PET = 982 };

        if (bot->IsMounted() || bot->GetVehicle())
            return false;
        Pet* pet = bot->GetPet();
        if (pet && pet->IsAlive())
        {
            SetPetGrowl(pet, !bot->GetGroup());
            return false;
        }

        PlayerbotAI* ai = GetAI(bot);
        for (uint32 spellId : { pet ? REVIVE_PET : CALL_PET_1, REVIVE_PET })
        {
            if (!CanTryCast(bot, spellId))
                continue;
            if (CastSpell(bot, bot, spellId))
                return true;
            if (ai)
                ai->BackOffSpell(spellId);
        }
        return false;
    }

    // Grimoire of Sacrifice needs a demon out, and a sacrificed Felhunter grants Spell Lock.
    bool TryWarlockPet(Player* bot)
    {
        enum : uint32
        {
            SUMMON_IMP            = 688,
            SUMMON_FELHUNTER      = 691,
            SUMMON_FELGUARD       = 30146,
            GRIMOIRE_OF_SACRIFICE = 108503
        };

        if (bot->IsMounted() || bot->GetVehicle() || bot->HasAura(GRIMOIRE_OF_SACRIFICE))
            return false;

        bool const sacrifice = bot->HasSpell(GRIMOIRE_OF_SACRIFICE);
        if (Pet* pet = bot->GetPet())
        {
            if (pet->IsAlive())
                return sacrifice && CanTryCast(bot, GRIMOIRE_OF_SACRIFICE)
                    && CastSpell(bot, bot, GRIMOIRE_OF_SACRIFICE);
        }

        uint32 const preferred = !sacrifice
            && bot->GetTalentSpecialization(bot->GetActiveSpec()) == SPEC_WARLOCK_DEMONOLOGY
            ? SUMMON_FELGUARD : SUMMON_FELHUNTER;

        PlayerbotAI* ai = GetAI(bot);
        uint32 const summons[] = { preferred, SUMMON_FELHUNTER, SUMMON_IMP };
        for (uint32 spellId : summons)
        {
            if (!CanTryCast(bot, spellId))
                continue;
            if (CastSpell(bot, bot, spellId))
                return true;
            if (ai)
                ai->BackOffSpell(spellId);
        }
        return false;
    }

    // Caster GUID → buff spell → member GUIDs who still lacked the aura after a
    // successful raid-wide cast. Cleared when they die so a rez gets another try.
    std::map<uint64, std::map<uint32, std::set<uint64>> > raidBuffRejected;

    bool HasExclusiveRaidBuff(Unit* unit, uint32 buffSpellId)
    {
        SpellInfo const* buffInfo = sSpellMgr->GetSpellInfo(buffSpellId);
        if (!unit || !buffInfo)
            return false;
        if (unit->IsImmunedToSpell(buffInfo))
            return true;
        if (buffInfo->CheckShapeshift(unit->GetShapeshiftForm()) != SpellCastResult::SPELL_CAST_OK)
            return true;

        Unit::AuraApplicationMap const& auras = unit->GetAppliedAuras();
        for (Unit::AuraApplicationMap::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
        {
            AuraApplication const* app = itr->second;
            if (!app)
                continue;
            Aura const* existing = app->GetBase();
            if (!existing)
                continue;
            SpellInfo const* existingInfo = existing->GetSpellInfo();
            if (!existingInfo || existingInfo->Id == buffSpellId)
                continue;
            if (buffInfo->IsAuraExclusiveBySpecificWith(existingInfo))
                return true;
            SpellGroupStackRule const rule = sSpellMgr->CheckSpellGroupStackRules(buffInfo, existingInfo);
            if (rule == SPELL_GROUP_STACK_RULE_EXCLUSIVE
                || rule == SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT)
                return true;
        }
        return false;
    }

    void PruneRaidBuffRejects(Player* bot, uint32 buffSpellId)
    {
        auto botIt = raidBuffRejected.find(bot->GetGUID());
        if (botIt == raidBuffRejected.end())
            return;
        auto spellIt = botIt->second.find(buffSpellId);
        if (spellIt == botIt->second.end())
            return;

        std::set<uint64> living;
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (member && member->IsAlive())
                    living.insert(member->GetGUID());
            }
        }
        for (std::set<uint64>::iterator it = spellIt->second.begin(); it != spellIt->second.end();)
        {
            if (living.find(*it) == living.end())
                it = spellIt->second.erase(it);
            else
                ++it;
        }
    }

    void RememberUncoveredAfterCast(Player* bot, std::initializer_list<uint32> auraIds, uint32 buffSpellId)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return;

        std::set<uint64>& skip = raidBuffRejected[bot->GetGUID()][buffSpellId];
        if (!HasAnyAura(bot, auraIds) && !HasExclusiveRaidBuff(bot, buffSpellId))
            skip.insert(bot->GetGUID());
        else
            skip.erase(bot->GetGUID());

        float const range = 40.0f;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == bot || !member->IsInWorld() || !member->IsAlive())
                continue;
            if (member->GetMap() != bot->GetMap())
                continue;
            if (!bot->IsWithinDistInMap(member, range, false))
                continue;
            if (HasAnyAura(member, auraIds) || HasExclusiveRaidBuff(member, buffSpellId))
                continue;
            skip.insert(member->GetGUID());
        }
    }

    bool WasRejectedForBuff(Player* bot, uint32 buffSpellId, uint64 memberGuid)
    {
        auto botIt = raidBuffRejected.find(bot->GetGUID());
        if (botIt == raidBuffRejected.end())
            return false;
        auto spellIt = botIt->second.find(buffSpellId);
        if (spellIt == botIt->second.end())
            return false;
        return spellIt->second.find(memberGuid) != spellIt->second.end();
    }

    // Raid-wide buffs hit the party in one cast. Dead members are ignored; a
    // rez'd ally is not (reject list is pruned on death). Allies who still lack
    // the aura after a successful cast are skipped so we do not burn every GCD.
    bool GroupCoveredByBuff(Player* bot, std::initializer_list<uint32> auraIds, uint32 buffSpellId)
    {
        if (!bot)
            return true;

        PruneRaidBuffRejects(bot, buffSpellId);

        if (HasAnyAura(bot, auraIds))
            raidBuffRejected[bot->GetGUID()][buffSpellId].erase(bot->GetGUID());
        else if (!HasExclusiveRaidBuff(bot, buffSpellId)
            && !WasRejectedForBuff(bot, buffSpellId, bot->GetGUID()))
            return false;

        Group* group = bot->GetGroup();
        if (!group)
            return true;

        float const range = 40.0f;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsInWorld() || !member->IsAlive() || member == bot)
                continue;
            if (member->GetMap() != bot->GetMap())
                continue;
            if (!bot->IsWithinDistInMap(member, range, false))
                continue;
            if (HasAnyAura(member, auraIds))
            {
                raidBuffRejected[bot->GetGUID()][buffSpellId].erase(member->GetGUID());
                continue;
            }
            if (HasExclusiveRaidBuff(member, buffSpellId))
                continue;
            if (WasRejectedForBuff(bot, buffSpellId, member->GetGUID()))
                continue;
            return false;
        }
        return true;
    }

    bool TryRaidBuff(Player* bot, std::initializer_list<uint32> auraIds, uint32 spellId)
    {
        if (GroupCoveredByBuff(bot, auraIds, spellId))
            return false;
        if (!TryCastBuff(bot, spellId))
            return false;
        RememberUncoveredAfterCast(bot, auraIds, spellId);
        return true;
    }
}

Player* FindPartyMemberToResurrect(Player* bot)
{
    if (!bot)
        return nullptr;

    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    Player* best = nullptr;
    float bestDist = 0.0f;
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || member == bot || !member->IsInWorld())
            continue;
        if (member->IsAlive() || member->IsRessurectRequested())
            continue;
        // Released ghosts sit at the graveyard — only rez unreleased corpses.
        if (member->getDeathState() != DeathState::CORPSE)
            continue;
        if (member->GetMap() != bot->GetMap())
            continue;

        float const dist = bot->GetDistance(member);
        if (!best || dist < bestDist)
        {
            best = member;
            bestDist = dist;
        }
    }
    return best;
}

uint32 SelectResurrectSpell(Player* bot)
{
    if (!bot)
        return 0;

    // Out of combat only — no combat battle-rez (Rebirth / Raise Ally).
    if (bot->IsInCombat())
        return 0;

    uint32 spellId = 0;
    switch (bot->getClass())
    {
        case CLASS_PRIEST:  spellId = 2006;   break; // Resurrection
        case CLASS_PALADIN: spellId = 7328;   break; // Redemption
        case CLASS_SHAMAN:  spellId = 2008;   break; // Ancestral Spirit
        case CLASS_DRUID:   spellId = 50769;  break; // Revive
        case CLASS_MONK:    spellId = 115178; break; // Resuscitate
        default:            return 0;
    }
    return CanTryCast(bot, spellId) ? spellId : 0;
}

bool TryMaintainBuffs(Player* bot)
{
    if (!bot || bot->HasUnitState(UNIT_STATE_CASTING)
        || bot->IsNonMeleeSpellCasted(false, false, true))
        return false;

    // Raid buff cast IDs vs applied aura IDs can differ (e.g. Legacy of the
    // Emperor 115921 applies aura 117666). Always check the auras that stick.
    std::initializer_list<uint32> const kStats = {
        1126, 20217, 115921, 117666, 117667, 72586, 90363, 79833, 102046
    };
    std::initializer_list<uint32> const kStamina = { 21562, 469, 72590 };
    std::initializer_list<uint32> const kAttackPower = { 6673, 57330 };
    std::initializer_list<uint32> const kSpellPower = { 1459, 109773 };
    std::initializer_list<uint32> const kMastery = { 19740 };
    std::initializer_list<uint32> const kMeleeCrit = { 116781 };

    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
        {
            bool const preferStamina =
                bot->GetTalentSpecialization(bot->GetActiveSpec()) == SPEC_WARRIOR_PROTECTION;
            if (preferStamina)
            {
                if (TryRaidBuff(bot, kStamina, 469))
                    return true;
                if (TryRaidBuff(bot, kAttackPower, 6673))
                    return true;
            }
            else
            {
                if (TryRaidBuff(bot, kAttackPower, 6673))
                    return true;
                if (TryRaidBuff(bot, kStamina, 469))
                    return true;
            }
            break;
        }
        case CLASS_PALADIN:
            // Only Blessing of Kings for the stats raid buff. Do not auto-cast
            // Might — with MotW/Emperor up it would re-cast every tick (one
            // blessing per pala + incomplete mastery coverage checks).
            if (TryRaidBuff(bot, kStats, 20217))
                return true;
            break;
        case CLASS_PRIEST:
            if (TryRaidBuff(bot, kStamina, 21562))
                return true;
            if (!HasAuraUp(bot, 588) && !HasAuraUp(bot, 73413) && TryCastBuff(bot, 588))
                return true;
            break;
        case CLASS_MAGE:
            if (TryRaidBuff(bot, kSpellPower, 1459))
                return true;
            break;
        case CLASS_HUNTER:
            if (TryHunterPet(bot))
                return true;
            break;
        case CLASS_WARLOCK:
            if (TryRaidBuff(bot, kSpellPower, 109773))
                return true;
            if (TryWarlockPet(bot))
                return true;
            break;
        case CLASS_DRUID:
            if (TryRaidBuff(bot, kStats, 1126))
                return true;
            break;
        case CLASS_DEATH_KNIGHT:
            if (TryRaidBuff(bot, kAttackPower, 57330))
                return true;
            break;
        case CLASS_MONK:
            if (TryRaidBuff(bot, kStats, 115921))
                return true;
            if (TryRaidBuff(bot, kMeleeCrit, 116781))
                return true;
            break;
        default:
            break;
    }

    return false;
}

bool TryPartySupport(Player* bot)
{
    if (!bot || bot->HasUnitState(UNIT_STATE_CASTING))
        return false;

    auto allyNeedsDispel = [](Player* ally, SpellInfo const* cleanseInfo) -> bool
    {
        if (!ally || !cleanseInfo)
            return false;

        uint32 const mask = cleanseInfo->GetDispelMask();
        if (!mask)
            return false;

        for (auto const& pair : ally->GetAppliedAuras())
        {
            AuraApplication const* app = pair.second;
            if (!app || app->IsPositive())
                continue;

            Aura const* aura = app->GetBase();
            if (!aura)
                continue;

            SpellInfo const* auraInfo = aura->GetSpellInfo();
            if (!auraInfo || !auraInfo->Dispel)
                continue;
            if (!(mask & SpellInfo::GetDispelMask(DispelType(auraInfo->Dispel))))
                continue;
            if (!cleanseInfo->CanDispelAura(auraInfo))
                continue;
            return true;
        }
        return false;
    };

    auto isTankSpec = [](Player* p) -> bool
    {
        if (!p)
            return false;
        uint32 const specId = p->GetTalentSpecialization(p->GetActiveSpec());
        uint8 const cls = p->getClass();
        uint32 const* specs = GetClassSpecializations(cls);
        if (!specs)
            return false;
        switch (cls)
        {
            case CLASS_WARRIOR:      return specs[2] == specId;
            case CLASS_PALADIN:      return specs[1] == specId;
            case CLASS_DEATH_KNIGHT: return specs[0] == specId;
            case CLASS_MONK:         return specs[0] == specId;
            case CLASS_DRUID:        return specs[2] == specId;
            default: return false;
        }
    };

    // --- 1) Paladin emergency externals ---
    if (bot->getClass() == CLASS_PALADIN)
    {
        Group* group = bot->GetGroup();
        if (group)
        {
            Player* lohTarget = nullptr;
            Player* hopTarget = nullptr;
            float lohPct = 100.0f;
            float hopPct = 100.0f;

            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || !member->IsInWorld() || !member->IsAlive())
                    continue;
                if (member->GetMap() != bot->GetMap())
                    continue;
                if (!bot->IsWithinDistInMap(member, 40.0f, false))
                    continue;
                if (member->HasAura(25771)) // Forbearance
                    continue;

                float const pct = UnitHealthPct(member);
                if (pct < 25.0f && pct < lohPct)
                {
                    lohTarget = member;
                    lohPct = pct;
                }
                // HoP: critically low non-tanks (physical immunity stalls tank threat).
                if (pct < 35.0f && !isTankSpec(member) && !member->HasAura(1022) && pct < hopPct)
                {
                    hopTarget = member;
                    hopPct = pct;
                }
            }

            if (lohTarget && CanTryCast(bot, 633))
            {
                if (CastHealSpell(bot, lohTarget, 633))
                    return true;
            }
            if (hopTarget && CanTryCast(bot, 1022))
            {
                if (CastHealSpell(bot, hopTarget, 1022))
                    return true;
            }
        }
    }

    // --- 2) Self-defensives ---
    float const selfHp = UnitHealthPct(bot);
    if (selfHp < 40.0f)
    {
        uint32 defensiveId = 0;
        switch (bot->getClass())
        {
            case CLASS_PALADIN:
                if (CanTryCast(bot, 642)) defensiveId = 642;       // Divine Shield
                else if (CanTryCast(bot, 498)) defensiveId = 498;  // Divine Protection
                break;
            case CLASS_WARRIOR:
                if (CanTryCast(bot, 871)) defensiveId = 871;         // Shield Wall
                else if (CanTryCast(bot, 12975)) defensiveId = 12975; // Last Stand
                else if (CanTryCast(bot, 55694)) defensiveId = 55694; // Enraged Regeneration
                break;
            case CLASS_DEATH_KNIGHT:
                if (CanTryCast(bot, 48792)) defensiveId = 48792;     // Icebound Fortitude
                else if (CanTryCast(bot, 48707)) defensiveId = 48707; // Anti-Magic Shell
                else if (CanTryCast(bot, 55233)) defensiveId = 55233; // Vampiric Blood
                break;
            case CLASS_DRUID:
                if (CanTryCast(bot, 61336)) defensiveId = 61336;     // Survival Instincts
                else if (CanTryCast(bot, 22812)) defensiveId = 22812; // Barkskin
                break;
            case CLASS_HUNTER:
                if (CanTryCast(bot, 19263)) defensiveId = 19263;     // Deterrence
                break;
            case CLASS_MAGE:
                if (CanTryCast(bot, 45438)) defensiveId = 45438;     // Ice Block
                break;
            case CLASS_PRIEST:
                if (CanTryCast(bot, 47585)) defensiveId = 47585;     // Dispersion
                else if (CanTryCast(bot, 586)) defensiveId = 586;    // Fade
                break;
            case CLASS_ROGUE:
                if (CanTryCast(bot, 31224)) defensiveId = 31224;     // Cloak of Shadows
                else if (CanTryCast(bot, 5277)) defensiveId = 5277;  // Evasion
                break;
            case CLASS_WARLOCK:
                if (CanTryCast(bot, 104773)) defensiveId = 104773;   // Unending Resolve
                break;
            case CLASS_SHAMAN:
                if (CanTryCast(bot, 108271)) defensiveId = 108271;   // Astral Shift
                break;
            case CLASS_MONK:
                if (CanTryCast(bot, 115203)) defensiveId = 115203;   // Fortifying Brew
                else if (CanTryCast(bot, 122278)) defensiveId = 122278; // Dampen Harm
                break;
            default:
                break;
        }

        if (defensiveId && CastSpell(bot, bot, defensiveId))
            return true;
    }

    // --- 3) Party cleanse ---
    uint32 cleanseId = 0;
    switch (bot->getClass())
    {
        case CLASS_PALADIN: cleanseId = 4987; break;   // Cleanse
        case CLASS_PRIEST:  cleanseId = 527; break;    // Purify
        case CLASS_SHAMAN:  cleanseId = 51886; break;  // Cleanse Spirit
        case CLASS_MAGE:    cleanseId = 475; break;    // Remove Curse
        case CLASS_DRUID:   cleanseId = 88423; break;  // Nature's Cure
        case CLASS_MONK:    cleanseId = 115450; break; // Detox
        default: break;
    }

    if (cleanseId && CanTryCast(bot, cleanseId))
    {
        SpellInfo const* cleanseInfo = sSpellMgr->GetSpellInfo(cleanseId);
        Group* group = bot->GetGroup();
        if (cleanseInfo && group)
        {
            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || !member->IsInWorld() || !member->IsAlive())
                    continue;
                if (member->GetMap() != bot->GetMap())
                    continue;
                if (!bot->IsWithinDistInMap(member, 40.0f, false))
                    continue;
                if (!allyNeedsDispel(member, cleanseInfo))
                    continue;
                if (CastHealSpell(bot, member, cleanseId))
                    return true;
            }
        }
        // Solo: cleanse self.
        if ((!group || group->GetMembersCount() <= 1) && allyNeedsDispel(bot, cleanseInfo))
        {
            if (CastHealSpell(bot, bot, cleanseId))
                return true;
        }
    }

    return false;
}

bool TryCrowdControl(Player* bot, Unit* enemy)
{
    if (!bot || !enemy || !enemy->IsAlive() || bot->HasUnitState(UNIT_STATE_CASTING))
        return false;
    if (!StrategyOn(bot, "cc"))
        return false;
    if (!bot->IsValidAttackTarget(enemy))
        return false;

    // Prefer an add that is not the tank's current target when possible.
    Unit* ccTarget = enemy;
    if (Group* group = bot->GetGroup())
    {
        Unit* tankVictim = nullptr;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsAlive())
                continue;
            if (PlayerbotAI* mai = GetAI(member))
            {
                if (mai->GetCombatRolePublic() != 0)
                    continue;
            }
            else
                continue;
            if (Unit* v = member->GetVictim())
            {
                tankVictim = v;
                break;
            }
        }
        if (tankVictim && tankVictim == enemy)
        {
            if (Unit* alt = FindSecondaryEnemy(bot, enemy, 30.0f))
                ccTarget = alt;
            else
                return false; // Don't CC the tank's focus.
        }
    }

    if (ccTarget->HasAuraType(SPELL_AURA_MOD_STUN)
        || ccTarget->HasAuraType(SPELL_AURA_MOD_CONFUSE)
        || ccTarget->HasAuraType(SPELL_AURA_MOD_FEAR)
        || ccTarget->HasAuraType(SPELL_AURA_MOD_ROOT))
        return false;

    auto tryCc = [&](uint32 spellId) -> bool
    {
        if (!CanTryCast(bot, spellId))
            return false;
        return CastSpell(bot, ccTarget, spellId);
    };

    switch (bot->getClass())
    {
        case CLASS_MAGE:
            if (tryCc(118)) // Polymorph
                return true;
            break;
        case CLASS_WARLOCK:
            if (ccTarget->GetCreatureType() == CREATURE_TYPE_DEMON
                || ccTarget->GetCreatureType() == CREATURE_TYPE_ELEMENTAL)
            {
                if (tryCc(710)) // Banish
                    return true;
            }
            if (tryCc(5782)) // Fear
                return true;
            break;
        case CLASS_HUNTER:
            if (tryCc(1499)) // Freezing Trap
                return true;
            break;
        case CLASS_SHAMAN:
            if (tryCc(51514)) // Hex
                return true;
            break;
        case CLASS_PRIEST:
            if (ccTarget->GetCreatureType() == CREATURE_TYPE_UNDEAD && tryCc(9484)) // Shackle Undead
                return true;
            break;
        case CLASS_DRUID:
            if (tryCc(33786)) // Cyclone
                return true;
            break;
        case CLASS_ROGUE:
            if (bot->HasAuraType(SPELL_AURA_MOD_STEALTH) && tryCc(6770)) // Sap
                return true;
            break;
        default:
            break;
    }
    return false;
}

bool TryCombatUtilities(Player* bot, Unit* enemy)
{
    if (!bot)
        return false;
    if (bot->HasUnitState(UNIT_STATE_CASTING))
        return false;

    if (enemy && TryInterrupt(bot, enemy))
        return true;
    if (enemy && TryCrowdControl(bot, enemy))
        return true;
    if (TryPartySupport(bot))
        return true;
    if (TryRacial(bot, enemy ? enemy : bot))
        return true;
    if (TryTrinkets(bot))
        return true;
    return false;
}

uint32 SelectNextSpell(Player* bot, Unit* target)
{
    if (!bot || !target || !target->IsAlive())
        return 0;

    Context ctx;
    ctx.bot = bot;
    ctx.target = target;
    ctx.enemies = CountNearbyEnemies(bot, 10.0f);
    if (StrategyOn(bot, "aoe"))
    {
        // Ranged bots stand outside the pack, so count enemies on the target too.
        uint32 const packed = CountEnemiesNear(bot, target, 8.0f);
        if (packed > ctx.enemies)
            ctx.enemies = packed;
    }
    else
        ctx.enemies = 1;
    ctx.targetHealthPct = UnitHealthPct(target);
    ctx.comboPoints = bot->GetComboPoints();

    uint32 const spec = bot->GetTalentSpecialization(bot->GetActiveSpec());
    switch (spec)
    {
        case SPEC_PALADIN_RETRIBUTION:   return SelectRetribution(ctx);
        case SPEC_PALADIN_PROTECTION:    return SelectProtectionPaladin(ctx);
        case SPEC_MONK_WINDWALKER:       return SelectWindwalker(ctx);
        case SPEC_MONK_BREWMASTER:       return SelectBrewmaster(ctx);
        case SPEC_HUNTER_BEAST_MASTERY:  return SelectBeastMastery(ctx);
        case SPEC_HUNTER_MARKSMANSHIP:   return SelectMarksmanship(ctx);
        case SPEC_HUNTER_SURVIVAL:       return SelectSurvival(ctx);
        case SPEC_PRIEST_SHADOW:         return SelectShadow(ctx);
        case SPEC_WARLOCK_AFFLICTION:    return SelectAffliction(ctx);
        case SPEC_WARLOCK_DESTRUCTION:   return SelectDestruction(ctx);
        case SPEC_WARLOCK_DEMONOLOGY:    return SelectDemonology(ctx);
        case SPEC_SHAMAN_ELEMENTAL:      return SelectElemental(ctx);
        case SPEC_SHAMAN_ENHANCEMENT:    return SelectEnhancement(ctx);
        case SPEC_DRUID_BALANCE:         return SelectBalance(ctx);
        case SPEC_DRUID_FERAL:           return SelectFeral(ctx);
        case SPEC_DRUID_GUARDIAN:        return SelectGuardian(ctx);
        case SPEC_WARRIOR_ARMS:          return SelectArms(ctx);
        case SPEC_WARRIOR_FURY:          return SelectFury(ctx);
        case SPEC_WARRIOR_PROTECTION:    return SelectProtectionWarrior(ctx);
        case SPEC_ROGUE_ASSASSINATION:   return SelectAssassination(ctx);
        case SPEC_ROGUE_COMBAT:          return SelectCombat(ctx);
        case SPEC_ROGUE_SUBTLETY:        return SelectSubtlety(ctx);
        case SPEC_MAGE_ARCANE:           return SelectArcane(ctx);
        case SPEC_MAGE_FIRE:             return SelectFire(ctx);
        case SPEC_MAGE_FROST:            return SelectFrostMage(ctx);
        case SPEC_DEATH_KNIGHT_BLOOD:    return SelectBlood(ctx);
        case SPEC_DEATH_KNIGHT_FROST:    return SelectFrostDK(ctx);
        case SPEC_DEATH_KNIGHT_UNHOLY:   return SelectUnholy(ctx);
        // Healer specs: damage line used when co +healer dps (nobody needs heals).
        case SPEC_PALADIN_HOLY:          return SelectHolyPaladinDps(ctx);
        case SPEC_PRIEST_DISCIPLINE:     return SelectDisciplineDps(ctx);
        case SPEC_PRIEST_HOLY:           return SelectHolyPriestDps(ctx);
        case SPEC_SHAMAN_RESTORATION:    return SelectRestorationShamanDps(ctx);
        case SPEC_DRUID_RESTORATION:     return SelectRestorationDruidDps(ctx);
        case SPEC_MONK_MISTWEAVER:       return SelectMistweaverDps(ctx);
        default:
            // Low-level / unset spec: still run the class rotation.
            if (bot->getClass() == CLASS_HUNTER)
                return SelectBeastMastery(ctx);
            return 0;
    }
}

uint32 SelectNextHeal(Player* bot, Player* ally, bool saveMana, float saveManaThreshold)
{
    if (!bot || !ally || !ally->IsAlive())
        return 0;

    HealContext ctx;
    ctx.bot = bot;
    ctx.healTarget = ally;
    ctx.healTargetHealthPct = UnitHealthPct(ally);
    ctx.lowestAllyHealthPct = ctx.healTargetHealthPct;
    ctx.injuredAllies = 0;
    ctx.enemies = CountNearbyEnemies(bot, 10.0f);
    if (!StrategyOn(bot, "aoe"))
        ctx.enemies = 1;
    ctx.manaPct = bot->GetMaxPower(POWER_MANA)
        ? (100.0f * float(bot->GetPower(POWER_MANA)) / float(bot->GetMaxPower(POWER_MANA)))
        : 100.0f;
    ctx.saveMana = saveMana;
    ctx.saveManaThreshold = saveManaThreshold;

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsAlive() || !bot->IsInMap(member))
                continue;
            if (!bot->IsWithinDistInMap(member, 40.0f))
                continue;
            float const pct = UnitHealthPct(member);
            if (pct < ctx.lowestAllyHealthPct)
                ctx.lowestAllyHealthPct = pct;
            if (pct < 90.0f)
                ++ctx.injuredAllies;
        }
    }
    else if (ctx.healTargetHealthPct < 90.0f)
        ctx.injuredAllies = 1;

    uint32 const spec = bot->GetTalentSpecialization(bot->GetActiveSpec());
    switch (spec)
    {
        case SPEC_PALADIN_HOLY:        return SelectHolyPaladin(ctx);
        case SPEC_PRIEST_DISCIPLINE:   return SelectDiscipline(ctx);
        case SPEC_PRIEST_HOLY:         return SelectHolyPriest(ctx);
        case SPEC_SHAMAN_RESTORATION:  return SelectRestorationShaman(ctx);
        case SPEC_DRUID_RESTORATION:   return SelectRestorationDruid(ctx);
        case SPEC_MONK_MISTWEAVER:     return SelectMistweaver(ctx);
        default:                       return 0;
    }
}

} // namespace BotRotation
