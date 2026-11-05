/*
 * Playerbots - per-spec combat rotation picker.
 *
 * Priorities are hand-ported from MoP Hekili/SimC lists (local Hekili/
 * reference). Unhandled specs return 0 so the AI falls back to fillers.
 */

#ifndef _SF_BOT_ROTATION_H
#define _SF_BOT_ROTATION_H

#include "Define.h"

class Player;
class Unit;

namespace BotRotation
{
    struct Context
    {
        Player* bot = nullptr;
        Unit* target = nullptr;
        uint32 enemies = 1;
        float targetHealthPct = 100.0f;
        int8 comboPoints = 0;
    };

    // Healing priority context (ally-targeted spells).
    struct HealContext
    {
        Player* bot = nullptr;
        Player* healTarget = nullptr;
        float healTargetHealthPct = 100.0f;
        uint32 injuredAllies = 0;
        float lowestAllyHealthPct = 100.0f;
        float manaPct = 100.0f;
        uint32 enemies = 1;
        bool saveMana = false;
        float saveManaThreshold = 60.0f;
    };

    // First ready spell for the bot's active specialization, or 0.
    uint32 SelectNextSpell(Player* bot, Unit* target);

    // First ready heal for healer specs, or 0 (caller may fall back).
    // saveMana: skip expensive flashes/surges below saveManaThreshold unless critical.
    uint32 SelectNextHeal(Player* bot, Player* ally, bool saveMana = false,
        float saveManaThreshold = 60.0f);

    // Cast helper: routes self-buffs to the bot, damage to the enemy.
    bool CastSpell(Player* bot, Unit* enemy, uint32 spellId);

    // Hunter shot cast: clears move flags, finishes GCD-instants, verifies focus spend.
    bool CastHunterShot(Player* bot, Unit* target, uint32 spellId);

    // Cast a heal/buff: self spells on the bot, otherwise on the ally.
    bool CastHealSpell(Player* bot, Player* ally, uint32 spellId);

    // Kick -> cc -> party support (cleanse/defensive/HoP) -> racial -> on-use trinket.
    bool TryCombatUtilities(Player* bot, Unit* enemy);
    bool TryPartySupport(Player* bot);
    bool TryCrowdControl(Player* bot, Unit* enemy);
    bool TryInterrupt(Player* bot, Unit* target);
    bool TryRacial(Player* bot, Unit* targetOrSelf);
    bool TryTrinkets(Player* bot);
    bool TryMaintainBuffs(Player* bot);
    // OOC class rez only (no combat battle-rez). 0 if none / in combat.
    uint32 SelectResurrectSpell(Player* bot);
    // Nearest dead party member still as a corpse (not released) awaiting rez.
    Player* FindPartyMemberToResurrect(Player* bot);
    bool IsBursting(Player* bot);
    bool HasGlyphSpell(Player* bot, uint32 glyphSpellId);

    // Apply a fixed recommended talent spell loadout for Wave-1 DPS specs.
    void ApplyRecommendedTalents(Player* bot);
    // Apply recommended major/minor glyphs for the bot's active specialization.
    void ApplyRecommendedGlyphs(Player* bot);

    // Helpers used by per-spec lists.
    uint32 CountNearbyEnemies(Player* bot, float range);
    // Hostiles around `around` that `bot` can attack. 0 when there are none
    // (unlike CountNearbyEnemies, which reports 1 when the search is empty).
    uint32 CountEnemiesNear(Player* bot, Unit* around, float range);
    // Major (multi-minute) cooldowns: +boost and a target worth it (boss, elite,
    // player) or a pack of 3+, so open-world trash doesn't eat them.
    bool WantsCooldowns(Context const& ctx);
    // Our ground effect (Rain of Fire, Blizzard...) is still on the floor.
    bool HasGroundEffect(Player* bot, uint32 spellId);
    // Nearest valid hostile other than `exclude` (for Havoc / multi-DoT).
    Unit* FindSecondaryEnemy(Player* bot, Unit* exclude, float range);
    // MoP often applies a different aura ID than the cast spell (e.g. Corruption
    // 172 → 146739). Pass either cast or aura ID; returns the aura to inspect.
    uint32 AuraIdForSpell(uint32 castOrAuraId);
    float AuraRemains(Unit* unit, uint32 spellId);
    // Remaining time of OUR aura on the unit (caster-GUID filtered).
    float MyAuraRemains(Player* bot, Unit* unit, uint32 spellId);
    bool HasAuraUp(Unit* unit, uint32 spellId);
    bool HasMyAura(Player* bot, Unit* unit, uint32 spellId);
    // True when our DoT is missing or remaining duration <= refreshAt seconds.
    bool NeedsMyAuraRefresh(Player* bot, Unit* unit, uint32 spellId, float refreshAt);
    uint32 AuraStacks(Unit* unit, uint32 spellId);
    bool SpellReady(Player* bot, uint32 spellId);
    // Known, off GCD/CD, and affordable (SpellPower.dbc cost).
    bool CanTryCast(Player* bot, uint32 spellId);
    // True when `formSpellId` should be cast this tick. Drops a conflicting
    // shapeshift first — form spells have SPELL_ATTR0_NOT_SHAPESHIFT, so Bear /
    // Cat / Moonkin fail while another form is already up.
    bool ReadyToShapeshift(Player* bot, uint32 formSpellId);
    // Drop Cat / Moonkin / etc. when `spellId` cannot be cast in the current form.
    // Returns true if a form was removed.
    bool BreakFormForSpell(Player* bot, uint32 spellId);
    bool IsSelfCastSpell(uint32 spellId);
    // Off-hand is an actual shield (not a held weapon / tome).
    bool HasShieldEquipped(Player* bot);
}

#endif // _SF_BOT_ROTATION_H
