/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "BotGearSets.h"
#include "BotPreferredMounts.h"
#include "BotTeleportMaps.h"
#include "BotVendorHubs.h"
#include "engine/BotAiEngine.h"
#include "engine/BotFleeManager.h"
#include "engine/BotFormation.h"
#include "engine/BotMovement.h"
#include "rotations/BotRotation.h"
#include "Bag.h"
#include "CellImpl.h"
#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "DynamicObject.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "GroupMgr.h"
#include "GroupReference.h"
#include "Item.h"
#include "ItemPrototype.h"
#include "LFGMgr.h"
#include "LootMgr.h"
#include "Map.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PathGenerator.h"
#include <unordered_set>
#include "PetDefines.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "ThreatManager.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "ByteBuffer.h"
#include "GossipDef.h"
#include "Config.h"
#include "Log.h"
#include "Spell.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <limits>
#include <list>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // How often (ms) the bot re-evaluates its behaviour. Kept coarse to keep the
    // per-tick cost of large bot populations low.
    constexpr uint32 BOT_AI_UPDATE_INTERVAL = 500;
    constexpr uint32 BOT_STRANDED_CHECK_INTERVAL = 30000;

    // Auto Shot / wand stay on CURRENT_AUTOREPEAT forever. Callers that mean
    // "am I mid cast-bar?" must skip autorepeat or hunters never act again.
    bool IsBusyCasting(Player const* bot)
    {
        if (!bot)
            return false;
        return bot->IsNonMeleeSpellCasted(false, false, true)
            || bot->HasUnitState(UNIT_STATE_CASTING);
    }

    // Conjured Mana Pudding wrapper (item cast). Actual seated regen auras:
    constexpr uint32 BOT_REFRESHMENT_SPELL = 128701;

    // Level-based rest auras: the item's own Food (OBS_MOD_HEALTH) / Drink spell,
    // applied directly. Drink spells must be in the core's periodic-dummy →
    // MOD_POWER_REGEN list (AuraEffect::CalculatePeriodic).
    struct BotRestTier
    {
        uint8 minLevel;
        uint32 spellId;
    };

    BotRestTier const BotFoodTiers[] =
    {
        { 85, 104934 },  // Frybread (82451)
        { 80, 80169 },   // Massive Turkey Leg / Highland Pomegranate (58269 / 58265)
        { 70, 43180 },   // Salted Venison (33454)
        { 65, 35270 },   // Clefthoof Ribs (29451)
        { 55, 27094 },   // Mag'har Grainbread (27855)
        { 45, 1131 },    // Roasted Quail (8952)
        { 35, 1129 },    // Cured Ham Steak (4599)
        { 25, 1127 },    // Wild Hog Shank (3771)
        { 15, 435 },     // Mutton Chop (3770)
        { 5, 434 },      // Haunch of Meat (2287)
        { 1, 433 },      // Tough Hunk of Bread (4540)
    };

    BotRestTier const BotDrinkTiers[] =
    {
        { 85, 104262 },  // Roasted Barley Tea (81406) — drink part of its Refreshment
        { 80, 80167 },   // Highland Spring Water (58257)
        { 70, 43182 },   // Pungent Seal Whey (33444)
        { 60, 27089 },   // Purified Draenic Water (27860)
        { 45, 1137 },    // Morning Glory Dew (8766)
        { 35, 1135 },    // Moonberry Juice (1645)
        { 25, 1133 },    // Sweet Nectar (1708)
        { 15, 432 },     // Melon Juice (1205)
        { 5, 431 },      // Ice Cold Milk (1179)
        { 1, 430 },      // Refreshing Spring Water (159)
    };

    template <size_t N>
    uint32 RestSpellForLevel(BotRestTier const (&tiers)[N], uint8 level)
    {
        for (BotRestTier const& tier : tiers)
            if (level >= tier.minLevel)
                return tier.spellId;
        return tiers[N - 1].spellId;
    }

    template <size_t N>
    bool HasAnyRestAura(Player const* player, BotRestTier const (&tiers)[N])
    {
        for (BotRestTier const& tier : tiers)
            if (player->HasAura(tier.spellId))
                return true;
        return false;
    }

    template <size_t N>
    void RemoveRestAuras(Player* player, BotRestTier const (&tiers)[N])
    {
        for (BotRestTier const& tier : tiers)
            player->RemoveAurasDueToSpell(tier.spellId);
    }

    // Follow a little further out than pets so a party of bots doesn't stack on
    // the leader.
    constexpr float BOT_FOLLOW_DIST = 2.0f;
    constexpr float TWO_PI = 6.2831853f;

    // Beyond this distance (same map) the follow generator can't realistically
    // catch up, so the bot teleports to the leader instead.
    constexpr float BOT_TELEPORT_DIST = 100.0f;

    // Distance ranged/caster bots try to hold from their target.
    constexpr float BOT_CAST_DIST = 25.0f;

    // Healer OOC top-up / combat triage band (SelectHealTarget default).
    constexpr float BOT_HEAL_BELOW_PCT = 90.0f;
    // Hybrid DPS emergency off-heal (co +offheal).
    constexpr float BOT_OFFHEAL_BELOW_PCT = 30.0f;

    // How far a bot will walk to loot a corpse or reach a repairer.
    // Keep loot near the group — long seeks cause follow↔loot thrash.
    constexpr float BOT_LOOT_SEEK_DIST = 25.0f;
    // Long enough to fall through to the next ability, short enough to retry once the cause clears.
    constexpr uint32 BOT_SPELL_BACKOFF_MS = 3000;
    constexpr float BOT_LOOT_LEADER_RADIUS = 40.0f;
    constexpr float BOT_REPAIR_SEEK_DIST = 20.0f;
    constexpr float BOT_VENDOR_SEEK_DIST = 20.0f;
    constexpr float BOT_QUEST_SEEK_DIST = 20.0f;
    // Skip quest NPCs with no takeable/turn-in work (bugged flags, wrong phase,
    // already done). Re-check after this so level-ups can unlock them later.
    constexpr uint32 BOT_QUEST_NPC_IGNORE_MS = 10 * MINUTE * IN_MILLISECONDS;
    // Once within this of a quest spawn, hunt/wander locally instead of re-pathing.
    constexpr float BOT_QUEST_HUNT_RADIUS = 40.0f;

    // Solo idle wander: radius around the bot and pause between picks.
    constexpr float BOT_WANDER_RADIUS = 18.0f;
    constexpr uint32 BOT_WANDER_PAUSE_MIN = 4000;
    constexpr uint32 BOT_WANDER_PAUSE_MAX = 10000;

    // Repair when any equipped item is below this fraction of max durability.
    constexpr float BOT_REPAIR_THRESHOLD = 0.50f;

    // Rest / save-mana defaults (override via Playerbots.Rest.* / SaveMana.Threshold).
    // Strategy enable/disable is runtime co/nc only — not config.
    constexpr float BOT_REST_REGEN_PCT = 0.15f;

    bool CanFitLootItem(Player* looter, uint32 itemId, uint32 count)
    {
        if (!looter || !itemId || !count)
            return false;
        ItemPosCountVec dest;
        return looter->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) == EQUIP_ERR_OK;
    }

    // Free loot the bot can actually put in bags (or gold). Does not include
    // roll-blocked threshold items — those are opened once to start rolls.
    bool HasStorableFreeLoot(Player* looter, Loot* loot)
    {
        if (!looter || !loot || loot->isLooted())
            return false;
        if (loot->gold)
            return true;
        for (LootItem const& item : loot->items)
        {
            if (item.is_looted || item.is_blocked)
                continue;
            if (CanFitLootItem(looter, item.itemid, item.count))
                return true;
        }
        return false;
    }

    bool HasPendingLootRolls(Loot* loot)
    {
        return loot && loot->hasOverThresholdItem();
    }

    // Skip corpses that only have roll-blocked threshold items (LFG NBG thrash),
    // or free loot the bot cannot store (full bags) after already opening for rolls.
    bool HasTakeableLoot(Player* looter, Creature* creature,
        std::unordered_set<uint64> const* bagFullSkip = nullptr,
        std::unordered_set<uint64> const* rollOpened = nullptr)
    {
        if (!looter || !creature || creature->IsAlive() || !looter->isAllowedToLoot(creature))
            return false;
        Loot* loot = &creature->loot;
        if (!loot || loot->isLooted())
            return false;

        uint64 const guid = creature->GetGUID();
        bool const pendingRolls = HasPendingLootRolls(loot);
        bool const needOpenForRolls = pendingRolls && (!rollOpened || !rollOpened->count(guid));
        if (needOpenForRolls)
            return true;

        if (bagFullSkip && bagFullSkip->count(guid) && !HasStorableFreeLoot(looter, loot))
            return false;

        return HasStorableFreeLoot(looter, loot);
    }

    struct BotLootCreatureCheck
    {
        BotLootCreatureCheck(Player* looter, float range,
            std::unordered_set<uint64> const* bagFullSkip,
            std::unordered_set<uint64> const* rollOpened)
            : _looter(looter), _range(range), _bagFullSkip(bagFullSkip), _rollOpened(rollOpened) { }

        bool operator()(Creature* creature) const
        {
            if (!creature || creature->IsAlive())
                return false;
            if (!_looter->IsWithinDist(creature, _range))
                return false;
            return HasTakeableLoot(_looter, creature, _bagFullSkip, _rollOpened);
        }

        Player* _looter;
        float _range;
        std::unordered_set<uint64> const* _bagFullSkip;
        std::unordered_set<uint64> const* _rollOpened;
    };

    struct BotRepairerCheck
    {
        BotRepairerCheck(Player* bot, float range) : _bot(bot), _range(range) { }

        bool operator()(Creature* creature) const
        {
            if (!creature || !creature->IsAlive() || creature->IsInCombat())
                return false;
            if (!_bot->IsWithinDist(creature, _range))
                return false;
            return creature->HasFlag(UNIT_FIELD_NPC_FLAGS, UNIT_NPC_FLAG_REPAIR);
        }

        Player* _bot;
        float _range;
    };

    struct BotVendorCheck
    {
        BotVendorCheck(Player* bot, float range) : _bot(bot), _range(range) { }

        bool operator()(Creature* creature) const
        {
            if (!creature || !creature->IsAlive() || creature->IsInCombat())
                return false;
            if (!_bot->IsWithinDist(creature, _range))
                return false;
            return creature->HasFlag(UNIT_FIELD_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR);
        }

        Player* _bot;
        float _range;
    };

    struct BotQuestgiverCheck
    {
        BotQuestgiverCheck(Player* bot, float range, PlayerbotAI* ai)
            : _bot(bot), _range(range), _ai(ai) { }

        bool operator()(Creature* creature) const
        {
            if (!creature || !creature->IsAlive() || creature->IsInCombat())
                return false;
            if (!_bot->IsWithinDist(creature, _range))
                return false;
            // Class/profession trainers often also carry QUESTGIVER. Spells come from
            // init — never walk up and open trainer gossip (spams DB/gossip errors).
            if (creature->HasFlag(UNIT_FIELD_NPC_FLAGS, UNIT_NPC_FLAG_TRAINER))
                return false;
            if (!creature->HasFlag(UNIT_FIELD_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
                return false;
            if (_ai && (_ai->IsQuestNpcIgnored(creature->GetEntry())
                || !_ai->QuestgiverHasUsefulWork(creature)))
                return false;
            return true;
        }

        Player* _bot;
        float _range;
        PlayerbotAI* _ai;
    };
}

PlayerbotAI::PlayerbotAI(Player* bot, bool clientControlled)
    : _bot(bot), _clientControlled(clientControlled), _updateTimer(0), _chaseGuid(0),
      _autoShotGuid(0), _debugLastMs(0), _followGuid(0), _lootGuid(0), _wanderTimer(0),
      _stay(false), _food(true), _loot(true), _quests(false),
      _passive(false), _grind(false),
      _tankMode(false), _tankAssist(false), _dpsMode(false), _dpsAssist(false),
      _threat(false), _healerDps(false), _saveMana(false), _offHeal(false), _ncHeal(false),
      _waitForAttack(false),
      _aoe(false), _boost(true), _cc(false), _avoidAoe(true),
      _debugCombat(false),
      _forceRest(false), _resting(false), _holdAssist(false),
      _forcedTargetGuid(0), _lfgRoleResponded(false), _lfgProposalResponded(false),
      _wasGrouped(false)
{
    ResetStrategiesToRoleDefaults();
    _aiEngine = std::make_unique<BotAiEngine>(this);
    _wasGrouped = _bot && _bot->GetGroup();
}

PlayerbotAI::~PlayerbotAI() = default;

void PlayerbotAI::ResetStrategiesToRoleDefaults()
{
    CombatRole const role = GetCombatRole();
    _strategies.ResetToRoleDefaults(role == CombatRole::Tank, role == CombatRole::Healer,
        _bot ? _bot->getClass() : 0);
    SyncFlagsFromStrategies();
    _forceRest = false;
    _holdAssist = false;
    ClearForcedTarget();
}

void PlayerbotAI::AfterInitRelocate(bool didTeleport)
{
    if (!_bot)
        return;

    ClearTravelDestination();
    _forceQuest = false;
    _wanderTimer = 0;
    _followGuid = 0;
    _chaseGuid = 0;
    StopResting();

    if (!_clientControlled)
    {
        if (_bot->GetVictim())
            _bot->AttackStop();
        BotMovement::ClearMotion(_bot);
        BotMovement::StopAndIdle(_bot);
        if (_bot->getStandState() != UNIT_STAND_STATE_STAND)
            _bot->SetStandState(UNIT_STAND_STATE_STAND);
    }

    // Init resets to follow/food/loot — ungrouped random bots must resume
    // open-world questing/grind or they stand still after a scatter teleport.
    if (!_clientControlled && !_bot->GetGroup() && sPlayerbotMgr->IsRandomBot(_bot->GetGUID()))
    {
        _strategies.ChangeStrategy("+quests,+grind,-follow", BotState::NonCombat);
        _strategies.Add("grind", BotState::Combat);
        SyncFlagsFromStrategies();
        _wasGrouped = false;
    }

    if (didTeleport)
        EnsureVisiblePhases();
}

void PlayerbotAI::SyncFlagsFromStrategies()
{
    _food = _strategies.Has("food", BotState::NonCombat);
    _loot = _strategies.Has("loot", BotState::NonCombat);
    _quests = _strategies.Has("quests", BotState::NonCombat);
    _stay = _strategies.Has("stay", BotState::NonCombat)
        || _strategies.Has("stay", BotState::Combat);
    if (_strategies.Has("follow", BotState::NonCombat))
        _stay = false;

    _passive = _strategies.Has("passive", BotState::Combat)
        || _strategies.Has("passive", BotState::NonCombat);
    _grind = _strategies.Has("grind", BotState::Combat)
        || _strategies.Has("grind", BotState::NonCombat);

    _tankMode = _strategies.Has("tank", BotState::Combat);
    _tankAssist = _strategies.Has("tank assist", BotState::Combat);
    _dpsMode = _strategies.Has("dps", BotState::Combat);
    _dpsAssist = _strategies.Has("dps assist", BotState::Combat);
    _threat = _strategies.Has("threat", BotState::Combat);
    _healerDps = _strategies.Has("healer dps", BotState::Combat);
    _saveMana = _strategies.Has("save mana", BotState::Combat);
    _offHeal = _strategies.Has("offheal", BotState::Combat);
    _ncHeal = _strategies.Has("heal", BotState::NonCombat);
    _waitForAttack = _strategies.Has("wait for attack", BotState::Combat);
    _aoe = _strategies.Has("aoe", BotState::Combat);
    _boost = _strategies.Has("boost", BotState::Combat);
    _cc = _strategies.Has("cc", BotState::Combat);
    _avoidAoe = _strategies.Has("avoid aoe", BotState::Combat);
    _debugCombat = _strategies.Has("debug", BotState::Combat)
        || _strategies.Has("debug", BotState::NonCombat);
    if (_strategies.Has("heal", BotState::Combat))
        _healerDps = false;

    // Tank without explicit +dps stays in tank mode.
    if (GetCombatRole() == CombatRole::Tank && !_strategies.Has("dps", BotState::Combat))
        _tankMode = true;
    if (GetCombatRole() == CombatRole::Damage && !_dpsMode)
        _dpsMode = true; // damage bots always "dps" unless somehow cleared

    RebuildAiEngine();
}

void PlayerbotAI::RebuildAiEngine()
{
    if (_aiEngine)
        _aiEngine->Rebuild();
}

bool PlayerbotAI::RunCombat() { return HandleCombat(); }
bool PlayerbotAI::RunCombatCastOnly() { return HandleCombatCastOnly(); }
bool PlayerbotAI::RunRest() { return HandleRest(); }
bool PlayerbotAI::RunHeal()
{
    if (!_bot || !_ncHeal)
        return false;
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty())
        return false;
    return HandleHealing(BOT_HEAL_BELOW_PCT);
}
void PlayerbotAI::RunFollow() { HandleFollow(); }
void PlayerbotAI::RunStay() { HandleStay(); }
bool PlayerbotAI::RunLoot() { return HandleLoot(); }
void PlayerbotAI::RunWander() { HandleWander(); }
void PlayerbotAI::RunVendor() { HandleVendor(); }

bool PlayerbotAI::SetTravelDestination(uint32 mapId, float x, float y, float z)
{
    if (!_bot || _clientControlled)
        return false;
    if (_bot->GetMapId() != mapId)
        return false;

    _bot->UpdateAllowedPositionZ(x, y, z);
    _travel.mapId = mapId;
    _travel.x = x;
    _travel.y = y;
    _travel.z = z;
    _travel.active = true;
    _travelFailCount = 0;
    _followGuid = 0;
    _lootGuid = 0;
    _chaseGuid = 0;

    // Leave stay so TravelAction can run; follow strategy stays for after arrival.
    if (_stay)
    {
        _strategies.Remove("stay", BotState::NonCombat);
        _strategies.Remove("stay", BotState::Combat);
        if (!_strategies.Has("follow", BotState::NonCombat))
            _strategies.Add("follow", BotState::NonCombat);
        SyncFlagsFromStrategies();
    }
    return true;
}

void PlayerbotAI::ClearTravelDestination()
{
    _travel.active = false;
    _travelFailCount = 0;
}

bool PlayerbotAI::IsTravelArrived(float tol) const
{
    if (!_bot || !_travel.active)
        return true;
    if (_bot->GetMapId() != _travel.mapId)
        return false;
    float const dx = _bot->GetPositionX() - _travel.x;
    float const dy = _bot->GetPositionY() - _travel.y;
    return (dx * dx + dy * dy) <= (tol * tol);
}

bool PlayerbotAI::RunTravel()
{
    if (!_bot || _clientControlled || !_travel.active)
        return false;

    if (_bot->GetMapId() != _travel.mapId)
    {
        ClearTravelDestination();
        return false;
    }

    // Finish mount cast before pathing — movement cancels mount spells.
    if (BotMovement::IsCasting(_bot))
        return true;

    if (_bot->IsMounted() || _bot->HasAuraType(SPELL_AURA_MOUNTED))
        EnsureGroundMountCapability();

    if (IsTravelArrived())
    {
        ClearTravelDestination();
        // Keep mount if master is still mounted; otherwise Sync will dismount.
        SyncMountWithMaster();
        BotMovement::StopAndIdle(_bot);
        return true;
    }

    // Mount with a real cast while standing, then resume the path.
    if (!_bot->IsMounted() && !_bot->HasAuraType(SPELL_AURA_MOUNTED))
    {
        if (TryMount(BotMountKind::SwiftGround))
            return true;
    }

    MovementGeneratorType const moveType = _bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if (moveType == POINT_MOTION_TYPE)
        return true; // still walking the last MoveTo

    if (BotMovement::MoveTo(_bot, _travel.x, _travel.y, _travel.z))
    {
        _travelFailCount = 0;
        return true;
    }

    if (++_travelFailCount >= 5)
        ClearTravelDestination();
    return true;
}

bool PlayerbotAI::BeginTravelTo(float x, float y, float z, Player* from, bool acknowledge)
{
    if (!_bot)
        return false;
    if (_clientControlled)
    {
        if (acknowledge)
            ReplyTo(from, "Self-bot: you control movement.");
        return true;
    }

    if (!SetTravelDestination(_bot->GetMapId(), x, y, z))
    {
        if (acknowledge)
            ReplyTo(from, "Cannot set destination (wrong map).");
        return true;
    }

    // Probe path once so we can reject unreachable points immediately.
    if (!BotMovement::MoveTo(_bot, _travel.x, _travel.y, _travel.z))
    {
        ClearTravelDestination();
        if (acknowledge)
            ReplyTo(from, "Destination unreachable.");
        return true;
    }

    if (acknowledge)
    {
        std::ostringstream ss;
        ss << "Going to " << int32(_travel.x) << ", " << int32(_travel.y) << ", " << int32(_travel.z) << ".";
        ReplyTo(from, ss.str());
    }
    return true;
}

bool PlayerbotAI::HandleGoCommand(Player* from, std::string const& args, bool acknowledge)
{
    if (!_bot)
        return false;

    std::string body = args;
    while (!body.empty() && body.front() == ' ')
        body.erase(body.begin());
    while (!body.empty() && body.back() == ' ')
        body.pop_back();

    // go / go here → master position or master's selected unit.
    if (body.empty() || body == "here")
    {
        float x = from->GetPositionX();
        float y = from->GetPositionY();
        float z = from->GetPositionZ();
        if (Unit* selected = from->GetSelectedUnit())
        {
            if (selected->GetMap() == _bot->GetMap())
            {
                x = selected->GetPositionX();
                y = selected->GetPositionY();
                z = selected->GetPositionZ();
            }
        }
        else if (from->GetMap() != _bot->GetMap())
        {
            if (acknowledge)
                ReplyTo(from, "You are on another map.");
            return true;
        }
        return BeginTravelTo(x, y, z, from, acknowledge);
    }

    // go x;y;z or go x y z
    {
        std::string normalized = body;
        for (char& ch : normalized)
            if (ch == ';' || ch == ',')
                ch = ' ';

        std::istringstream iss(normalized);
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if ((iss >> x >> y >> z) && iss.eof())
            return BeginTravelTo(x, y, z, from, acknowledge);
    }

    // go <saved name>
    auto it = _savedPositions.find(body);
    if (it != _savedPositions.end())
    {
        if (it->second.mapId != _bot->GetMapId())
        {
            if (acknowledge)
                ReplyTo(from, "Saved position is on another map.");
            return true;
        }
        return BeginTravelTo(it->second.x, it->second.y, it->second.z, from, acknowledge);
    }

    if (acknowledge)
        ReplyTo(from, "Unknown go target. Use: go | go here | go x y z | go <name>.");
    return true;
}

namespace
{
    // Quality words autogear accepts (AC ChatHelper::parseItemQuality).
    bool ParseGearQuality(std::string const& token, int& qualityOut)
    {
        if (token == "poor" || token == "grey" || token == "gray")
            qualityOut = ITEM_QUALITY_POOR;
        else if (token == "common" || token == "white" || token == "normal")
            qualityOut = ITEM_QUALITY_NORMAL;
        else if (token == "uncommon" || token == "green")
            qualityOut = ITEM_QUALITY_UNCOMMON;
        else if (token == "rare" || token == "blue")
            qualityOut = ITEM_QUALITY_RARE;
        else if (token == "epic" || token == "purple")
            qualityOut = ITEM_QUALITY_EPIC;
        else if (token == "legendary" || token == "orange")
            qualityOut = ITEM_QUALITY_LEGENDARY;
        else
            return false;
        return true;
    }

    char const* GearQualityName(int quality)
    {
        switch (quality)
        {
            case ITEM_QUALITY_POOR:      return "poor";
            case ITEM_QUALITY_NORMAL:    return "common";
            case ITEM_QUALITY_UNCOMMON:  return "uncommon";
            case ITEM_QUALITY_RARE:      return "rare";
            case ITEM_QUALITY_EPIC:      return "epic";
            case ITEM_QUALITY_LEGENDARY: return "legendary";
            default:                     return "custom";
        }
    }

    // Average item level over the slots the player has filled, so empty
    // slots do not drag `autogear match` down.
    uint32 WornItemLevelAverage(Player* player)
    {
        uint32 sum = 0;
        uint32 count = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
                continue;
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                sum += item->GetTemplate()->ItemLevel;
                ++count;
            }
        }
        return count ? sum / count : 0;
    }
}

bool PlayerbotAI::HandleAutoGearCommand(Player* from, std::string const& args, bool acknowledge)
{
    auto reply = [&](std::string const& msg)
    {
        if (acknowledge)
            ReplyTo(from, msg);
    };

    if (!sPlayerbotMgr->IsAutoGearCommandEnabled())
    {
        reply("autogear command is not allowed, please check the configuration.");
        return true;
    }
    if (!sPlayerbotMgr->IsAutoGearAltBotsEnabled() && sPlayerbotMgr->IsAltBot(_bot))
    {
        reply("You cannot use autogear on alt bots.");
        return true;
    }
    if (_bot->IsInCombat())
    {
        reply("In combat — cannot change gear.");
        return true;
    }

    int const qualityCap = sPlayerbotMgr->GetAutoGearQualityLimit();
    uint32 const ilvlCap = sPlayerbotMgr->GetAutoGearItemLevelLimit(); // 0 = no limit
    int quality = qualityCap;
    uint32 ilvl = ilvlCap;

    // `reset` wipes the worn gear and regears every slot; it combines with
    // the other options: `autogear reset green`, `autogear reset 450`.
    std::string param = args;
    bool reset = false;
    if (param == "reset" || param.rfind("reset ", 0) == 0)
    {
        reset = true;
        param = param.size() > 6 ? param.substr(6) : std::string();
    }

    auto capItemLevel = [&](uint32 requested)
    {
        ilvl = ilvlCap ? std::min(requested, ilvlCap) : requested;
        if (ilvlCap && requested > ilvlCap)
            reply("Config caps item level at " + std::to_string(ilvlCap) + ", gearing to that instead of "
                + std::to_string(requested) + ".");
    };

    int requestedQuality = 0;
    if (param.empty())
    {
    }
    else if (param == "bis")
    {
        reply("autogear bis is not ported yet; use autogear or autogear reset.");
        return true;
    }
    else if (param == "match")
    {
        uint32 const requested = WornItemLevelAverage(from);
        if (!requested)
        {
            reply("You have no gear equipped for me to match.");
            return true;
        }
        capItemLevel(requested);
    }
    else if (ParseGearQuality(param, requestedQuality))
    {
        requestedQuality = std::max(requestedQuality, int(ITEM_QUALITY_NORMAL));
        quality = std::min(requestedQuality, qualityCap);
        if (requestedQuality > qualityCap)
            reply(std::string("Config limits me to ") + GearQualityName(qualityCap) + " quality, gearing to that.");
    }
    else
    {
        // Rejects negative numbers, garbage and numbers too large for an item level.
        bool const digitsOnly = param.size() <= 5 && param.find_first_not_of("0123456789") == std::string::npos;
        uint32 const requested = digitsOnly ? uint32(std::stoul(param)) : 0;
        if (!requested)
        {
            reply("Unknown autogear option '" + param + "'. Use reset, match, a quality "
                "(white, green, blue, purple, orange) or a positive item level number.");
            return true;
        }
        if (requested <= uint32(ITEM_QUALITY_LEGENDARY))
        {
            reply("'" + param + "' looks like a quality. Numbers are item levels (e.g. 450); use a colour word "
                "for quality: white, green, blue, purple, orange.");
            return true;
        }
        capItemLevel(requested);
    }

    uint32 const slots = sPlayerbotMgr->AutoGearBot(_bot, quality, ilvl, reset);

    std::string summary = std::string(reset ? "Regeared from scratch (" : "Auto geared (")
        + GearQualityName(quality) + (ilvl ? ", up to item level " + std::to_string(ilvl) : std::string()) + "): ";
    if (reset)
        summary += std::to_string(slots) + " slot(s) equipped.";
    else if (slots)
        summary += std::to_string(slots) + " slot(s) upgraded.";
    else
        summary += "no upgrades found.";
    reply(summary);
    return true;
}

bool PlayerbotAI::HandlePositionCommand(Player* from, std::string const& args, bool acknowledge)
{
    if (!_bot)
        return false;

    std::string body = args;
    while (!body.empty() && body.front() == ' ')
        body.erase(body.begin());
    while (!body.empty() && body.back() == ' ')
        body.pop_back();

    if (body.empty() || body == "?")
    {
        if (!acknowledge)
            return true;
        if (_savedPositions.empty())
        {
            ReplyTo(from, "No saved positions.");
            return true;
        }
        std::ostringstream ss;
        ss << "Positions:";
        for (auto const& pair : _savedPositions)
            ss << " " << pair.first;
        ReplyTo(from, ss.str());
        return true;
    }

    if (body.rfind("save ", 0) == 0)
    {
        std::string name = body.substr(5);
        while (!name.empty() && name.front() == ' ')
            name.erase(name.begin());
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        if (name.empty())
        {
            if (acknowledge)
                ReplyTo(from, "Usage: position save <name>");
            return true;
        }
        TravelPoint pt;
        pt.mapId = _bot->GetMapId();
        pt.x = _bot->GetPositionX();
        pt.y = _bot->GetPositionY();
        pt.z = _bot->GetPositionZ();
        _savedPositions[name] = pt;
        if (acknowledge)
            ReplyTo(from, std::string("Saved position '") + name + "'.");
        return true;
    }

    if (body.rfind("go ", 0) == 0)
    {
        std::string name = body.substr(3);
        while (!name.empty() && name.front() == ' ')
            name.erase(name.begin());
        return HandleGoCommand(from, name, acknowledge);
    }

    // Bare name → go to that saved position.
    return HandleGoCommand(from, body, acknowledge);
}
bool PlayerbotAI::IsGroupInCombatPublic() const { return GroupInCombat(); }

Unit* PlayerbotAI::SelectLowestHpGroupEnemyPublic() { return SelectLowestHpGroupEnemy(); }
Unit* PlayerbotAI::SelectAssistTankTargetPublic() { return SelectAssistTankTarget(); }
Unit* PlayerbotAI::SelectTankTargetPublic() { return SelectTankTarget(); }

int PlayerbotAI::GetCombatRolePublic() const
{
    switch (GetCombatRole())
    {
        case CombatRole::Tank:   return 0;
        case CombatRole::Healer: return 1;
        default:                 return 2;
    }
}

bool PlayerbotAI::IsRangedClassPublic() const { return IsRangedClass(); }

bool PlayerbotAI::HasEngageTarget() const
{
    if (_pullPhase != PullPhase::None)
        return true;
    if (_forcedTargetGuid && GetForcedTarget())
        return true;
    if (_targets.GetPullGuid())
        return _targets.GetPullTarget(const_cast<PlayerbotAI*>(this)) != nullptr;
    return false;
}

bool PlayerbotAI::IsSafeAttackTarget(Unit const* target) const
{
    if (!_bot || !target || !target->IsAlive())
        return false;
    // GetMap() ASSERTs when m_currMap is null — never call visibility checks then.
    if (!_bot->IsInWorld() || !target->IsInWorld())
        return false;
    if (!_bot->FindMap() || !target->FindMap() || _bot->FindMap() != target->FindMap())
        return false;
    return _bot->IsValidAttackTarget(target);
}

bool PlayerbotAI::ShouldWaitForAttack() const
{
    if (!_waitForAttack || !_bot)
        return false;
    // Explicit attack/pull orders never wait — engage immediately.
    if (HasEngageTarget())
        return false;
    // Tanks never wait — they are the pull.
    if (GetCombatRole() == CombatRole::Tank && _tankMode)
        return false;
    // Always fight back if something is hitting us.
    if (!_bot->getAttackers().empty())
        return false;
    if (!GroupInCombat() && !_bot->IsInCombat())
        return false;
    if (!_combatStartTime)
        return false;
    uint32 const waitSec = sPlayerbotMgr->GetWaitForAttackSeconds();
    return (time(nullptr) - _combatStartTime) < time_t(waitSec);
}

bool PlayerbotAI::ShouldFollowPublic() const
{
    if (!_bot || _clientControlled || _stay)
        return false;
    if (HasTravelDestination())
        return false;
    if (HasEngageTarget())
        return false;
    // Never freeze the whole formation for one drinker — thirsty bots park via
    // RestAction alone; everyone else keeps following the master.
    Group* group = _bot->GetGroup();
    if (!group)
        return false;
    uint64 const leaderGuid = group->GetLeaderGUID();
    return leaderGuid && leaderGuid != _bot->GetGUID();
}

bool PlayerbotAI::NeedsRestPublic() const
{
    if (!_bot)
        return false;
    // Never report rest need while fighting — self-bot was sitting mid-pull when
    // mana dipped and IsInCombat alone flickered false between casts.
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty())
        return false;
    if (HasEngageTarget())
        return false;
    if (_forceRest)
        return true;

    // Finish an in-progress drink even if the master just started moving — the
    // next HandleRest tick will stand us up if they keep running. Stand once
    // topped up even if the aura still has time left.
    if (HasFoodOrDrinkAura() || _resting)
    {
        float const hpPct = HealthPct();
        float const manaPct = ManaPct();
        if (hpPct >= 98.0f && (!UsesMana() || manaPct >= 98.0f))
            return true; // HandleRest clears aura + stands
        return true;
    }

    // Mid-chase: do not park to drink — stay on the leader.
    if (!IsMasterWaitingForRest())
        return false;

    float const hpPct = HealthPct();
    float const manaPct = ManaPct();
    bool const needHp = hpPct < sPlayerbotMgr->GetRestHealthPct();
    bool const needMana = UsesMana() && manaPct < sPlayerbotMgr->GetRestManaPct();
    // Only THIS bot's resources — never park because an ally is drinking.
    return needHp || needMana;
}

bool PlayerbotAI::NeedsOocHealPublic() const
{
    if (!_bot || !_ncHeal)
        return false;
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty())
        return false;
    if (HasEngageTarget())
        return false;
    if (IsBusyCasting(_bot))
        return true; // finish the cast
    // const_cast: SelectHealTarget is logically read-only for this check.
    return const_cast<PlayerbotAI*>(this)->SelectHealTarget(BOT_HEAL_BELOW_PCT) != nullptr;
}

void PlayerbotAI::HandleSlowActivity()
{
    HandlePendingInvites();
    if (!_bot->IsAlive())
    {
        TryAcceptResurrect();
        return;
    }

    if (_resting || HasFoodOrDrinkAura() || _bot->IsSitState())
        HandleRest();

    if (_clientControlled)
        return;

    Group* group = _bot->GetGroup();
    uint64 const leaderGuid = group ? group->GetLeaderGUID() : 0;
    if (!leaderGuid || leaderGuid == _bot->GetGUID())
        return;

    Player* leader = ObjectAccessor::FindPlayer(leaderGuid);
    if (!leader || !leader->IsInWorld() || !leader->IsAlive())
        return;
    if (!leader->GetSession() || leader->GetSession()->IsBot())
        return;

    if (leader->GetMap() != _bot->GetMap() || _bot->GetDistance(leader) > BOT_TELEPORT_DIST)
        TeleportToLeader(leader);
}

void PlayerbotAI::UpdateAI(uint32 diff)
{
    if (!_bot || _bot->GetTypeId() != TypeID::TYPEID_PLAYER || !_bot->IsInWorld())
        return;

    if (WorldSession* session = _bot->GetSession())
    {
        if (session->IsBot())
        {
            if (session->GetBotActivity() == WorldSession::BotActivity::Dormant)
                return;
            if (session->GetBotActivity() == WorldSession::BotActivity::Slow)
            {
                HandleSlowActivity();
                return;
            }
        }
    }

    // Invites / loot rolls / rez accepts must react immediately — they time out
    // (or stall forever for bots) if only handled on the coarse AI interval.
    HandlePendingInvites();
    SyncPartyStrategies();
    HandleLootRolls();
    if (!_bot->IsAlive())
    {
        TryAcceptResurrect();
        return;
    }

    _updateTimer += diff;
    if (_updateTimer < BOT_AI_UPDATE_INTERVAL)
        return;

    // Wander pauses are tracked in real time so they don't depend on the AI
    // throttle; clamp so a long hitch doesn't skip forever.
    if (_wanderTimer > BOT_AI_UPDATE_INTERVAL)
        _wanderTimer -= BOT_AI_UPDATE_INTERVAL;
    else
        _wanderTimer = 0;

    _updateTimer = 0;

    // A bot on a map tile/area that does not exist triggers "discovered unknown
    // area" / "Wrong area flag" spam and can never path anywhere.
    _strandedCheckTimer += BOT_AI_UPDATE_INTERVAL;
    if (_strandedCheckTimer >= BOT_STRANDED_CHECK_INTERVAL)
    {
        _strandedCheckTimer = 0;
        if (!_clientControlled && !BotTeleportMaps::IsOnKnownGround(_bot)
            && BotTeleportMaps::RescueStrandedBot(_bot))
            return;
    }

    // Ungrouped bots need the same phase sync grouped ones get, plus an explicit
    // viewer create-packet refresh after UpdateAreaPhase desyncs them.
    EnsureVisiblePhases();

    HandleInteractions();

    // Armor can only be changed out of combat.
    if (!_clientControlled && !_bot->IsInCombat())
    {
        int8 const wantPvp = _bot->GetMap()->IsBattlegroundOrArena() ? 1 : 0;
        if (wantPvp != _gearSetPvp)
        {
            BotGearSets::WearSet(_bot, wantPvp != 0);
            _gearSetPvp = wantPvp;
        }
    }

    // Keep raid/self buffs topped up out of combat when not mid-cast.
    if (!_bot->IsInCombat() && _bot->getAttackers().empty()
        && !IsBusyCasting(_bot)
        && BotRotation::TryMaintainBuffs(_bot))
        return;

    // Nearby / open-world questing (one step per tick). Skip while drinking —
    // MovePoint would stand the bot and cancel regen.
    if (!_bot->IsInCombat() && _bot->getAttackers().empty()
        && !NeedsRestPublic() && !_resting && !HasFoodOrDrinkAura() && !_bot->IsSitState()
        && (_quests || _forceQuest) && HandleAutoQuesting())
        return;

    // AC-style Trigger → Action → Queue. MoP rotations run inside the combat action.
    if (_aiEngine && _aiEngine->DoNextAction())
        return;

    // Fallback if the engine queued nothing useful (should be rare).
    if (_clientControlled)
    {
        if (HandleCombatCastOnly())
            return;
        // Self-bot: never auto-drink while combat is active (engine RestAction
        // is also gated; keep the fallback aligned).
        if (!_bot->IsInCombat() && !GroupInCombat() && _bot->getAttackers().empty())
        {
            if (NeedsOocHealPublic())
                RunHeal();
            else
                HandleRest();
        }
        return;
    }

    if (HandleCombat())
        return;
    if (NeedsOocHealPublic() && RunHeal())
        return;
    // Resurrect only when the fight is fully over — never steal GCDs from heals
    // or open with a combat battle-rez while the group still needs healing.
    if (!_bot->IsInCombat() && !GroupInCombat() && _bot->getAttackers().empty()
        && HandleResurrect())
        return;
    if (HandleRest())
        return;
    if (_stay)
    {
        HandleStay();
        HandleVendor();
        return;
    }
    if (ShouldFollowPublic())
        HandleFollow();
    else
    {
        if (HandleLoot())
            return;
        HandleWander();
        HandleVendor();
    }
}

// Auto-accept party/raid invitations so bots can be pulled into groups (and thus
// LFG/LFR/RBG queues). Mirrors the accept branch of HandleGroupAcceptOpcode.
void PlayerbotAI::HandlePendingInvites()
{
    Group* group = _bot->GetGroupInvite();
    if (!group)
        return;

    group->RemoveInvite(_bot);

    if (group->GetLeaderGUID() == _bot->GetGUID())
        return;

    if (group->IsFull())
        return;

    Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());

    if (!group->IsCreated())
    {
        if (!leader)
        {
            group->RemoveAllInvites();
            return;
        }

        group->RemoveInvite(leader);
        group->Create(leader);
        sGroupMgr->AddGroup(group);
    }

    if (!group->AddMember(_bot))
        return;

    group->BroadcastGroupUpdate();
    // Immediate follow pack so the next AI tick does not keep quest-traveling.
    SyncPartyStrategies();
}

void PlayerbotAI::SyncPartyStrategies()
{
    if (!_bot || _clientControlled)
        return;

    bool const inGroup = _bot->GetGroup() != nullptr;
    if (inGroup == _wasGrouped)
        return;
    _wasGrouped = inGroup;

    if (inGroup)
    {
        // Party master: stop open-world questing and follow the leader.
        _strategies.ApplyFollowPack();
        SyncFlagsFromStrategies();
        _forceQuest = false;
        ClearTravelDestination();
        ClearForcedTarget();
        _wanderTimer = 0;
        _followGuid = 0;
        _chaseGuid = 0;
        if (_bot->GetVictim())
            _bot->AttackStop();
        return;
    }

    // Random pool bots return to solo auto-quest + grind when the party dissolves.
    if (sPlayerbotMgr->IsRandomBot(_bot->GetGUID()))
    {
        _strategies.ChangeStrategy("+quests,+grind,-follow", BotState::NonCombat);
        _strategies.Add("grind", BotState::Combat);
        SyncFlagsFromStrategies();
        _followGuid = 0;
    }
}

// Auto-respond to the dungeon finder so a master can queue a party of bots: the
// bot answers the group role check and accepts the join proposal. Called from
// PlayerbotMgr::Update on the world thread (not from map AI ticks) because
// UpdateProposal mutates shared LFG/group state and teleports players.
void PlayerbotAI::HandleLfg()
{
    uint64 const guid = _bot->GetGUID();
    Group* grp = _bot->GetGroup();
    uint8 const roles = ComputeLfgRole();

    // Solo LFG fill: bots join without a party. They still must accept proposals.
    // Grouped bots also set party roles so MoP JoinLfg can pass RoleCheckAllResponded.
    if (grp)
    {
        uint64 const gguid = grp->GetGUID();

        // MoP: HandleLfgJoinOpcode refuses to call JoinLfg until every member has a
        // party role (Group::RoleCheckAllResponded). That happens *before* any LFG
        // ROLECHECK state exists, so bots must set GetMemberRole from their spec
        // while simply grouped — not only during an active role check.
        {
            uint32 const memberRole = grp->GetMemberRole(guid);
            uint32 const combatBits = memberRole & (lfg::PLAYER_ROLE_TANK | lfg::PLAYER_ROLE_HEALER | lfg::PLAYER_ROLE_DAMAGE);
            if (combatBits == 0 || (combatBits & roles) != roles)
            {
                uint32 newRole = roles;
                if (memberRole & lfg::PLAYER_ROLE_LEADER)
                    newRole |= lfg::PLAYER_ROLE_LEADER;
                grp->SetMemberRole(guid, newRole);
                grp->SendUpdate();
            }
        }

        // Prefer group state: player PlayersStore entries can default-construct to
        // NONE if touched before JoinLfg sets ROLECHECK on every member.
        lfg::LfgState const gstate = sLFGMgr->GetState(gguid);
        lfg::LfgState const pstate = sLFGMgr->GetState(guid);
        bool const inRoleCheck = (gstate == lfg::LFG_STATE_ROLECHECK) || (pstate == lfg::LFG_STATE_ROLECHECK);

        if (inRoleCheck)
        {
            uint8 const current = sLFGMgr->GetRoleCheckRoles(gguid, guid);
            // Only submit when unset or mismatched (avoid packet spam every tick).
            if ((current & (lfg::PLAYER_ROLE_TANK | lfg::PLAYER_ROLE_HEALER | lfg::PLAYER_ROLE_DAMAGE)) == 0
                || (current & roles) != roles)
            {
                SF_LOG_INFO("modules", "[mod-playerbots] Bot '%s' answering LFG role check as %u.",
                    _bot->GetName().c_str(), uint32(roles));
                sLFGMgr->UpdateRoleCheck(gguid, guid, roles);
            }
            _lfgRoleResponded = true;
        }
        else if (_lfgRoleResponded)
            _lfgRoleResponded = false;

        // Kick votes are started by the real players in the party; bots back them.
        // UpdateBoot ignores repeat answers and the victim's own (pre-set) vote.
        if (gstate == lfg::LFG_STATE_BOOT)
            sLFGMgr->UpdateBoot(guid, true);
    }
    else
        _lfgRoleResponded = false;

    // Proposals apply to solo queues and party queues alike — do not require a group.
    lfg::LfgState const pstate = sLFGMgr->GetState(guid);
    lfg::LfgState const gstate = grp ? sLFGMgr->GetState(grp->GetGUID()) : lfg::LFG_STATE_NONE;
    bool const inProposal = (pstate == lfg::LFG_STATE_PROPOSAL) || (gstate == lfg::LFG_STATE_PROPOSAL);
    if (inProposal || sLFGMgr->GetActiveProposalIdForPlayer(guid))
    {
        // Only accept once per proposal; GetActiveProposalIdForPlayer already
        // skips AGREE, but guard against re-entry while state is still PROPOSAL.
        if (!_lfgProposalResponded)
        {
            if (uint32 proposalId = sLFGMgr->GetActiveProposalIdForPlayer(guid))
            {
                SF_LOG_INFO("modules", "[mod-playerbots] Bot '%s' accepting LFG proposal %u (role %u).",
                    _bot->GetName().c_str(), proposalId, uint32(roles));
                sLFGMgr->UpdateProposal(proposalId, guid, true);
                _lfgProposalResponded = true;
            }
        }
    }
    else
        _lfgProposalResponded = false;
}

// Pick a role for the role check from the bot's current specialization.
// Hybrids must be init'd to tank/healer/dps (or an explicit spec) so the party
// can pass CheckGroupRoles.
uint8 PlayerbotAI::ComputeLfgRole()
{
    switch (GetCombatRole())
    {
        case CombatRole::Tank:   return lfg::PLAYER_ROLE_TANK;
        case CombatRole::Healer: return lfg::PLAYER_ROLE_HEALER;
        default:                 return lfg::PLAYER_ROLE_DAMAGE;
    }
}

// Combat: acquire a target from group threats (not the master's mouseover),
// position for the class, and run the rotation.
bool PlayerbotAI::HandleCombat()
{
    // Players cannot Attack() or cast combat spells while mounted (core rejects
    // both). Dismount as soon as we have a fight — waiting until IsInCombat
    // never fires because Attack() fails while mounted.
    bool const needFight = HasEngageTarget()
        || _bot->IsInCombat()
        || GroupInCombat()
        || !_bot->getAttackers().empty();
    if (needFight)
    {
        if (_resting || _bot->IsSitState() || HasFoodOrDrinkAura())
            StopResting();
        if (_travel.active)
            ClearTravelDestination();
        TryDismount();
    }

    // Track group/self combat start for wait-for-attack (AC WaitForAttackStrategy).
    if (_bot->IsInCombat() || GroupInCombat())
    {
        if (!_combatStartTime)
            _combatStartTime = time(nullptr);
    }
    else if (_combatStartTime)
    {
        _combatStartTime = 0;
        _targets.OnCombatEnded();
        if (_pullPhase != PullPhase::None)
            EndPullSequence(false);
    }

    // Non-tanks with +wait for attack hold DPS until the tank has threat time.
    if (ShouldWaitForAttack())
    {
        if (!_clientControlled && !_stay)
        {
            // Healers / ranged: hold cast range off the fight — do not trail the master.
            if (GetCombatRole() == CombatRole::Healer)
            {
                if (Unit* anchor = SelectHealerCombatAnchor())
                    HoldRangedCombatPosition(anchor, BOT_CAST_DIST);
            }
            else if (IsRangedClass())
            {
                if (Unit* t = SelectTarget())
                    HoldRangedCombatPosition(t, BOT_CAST_DIST);
            }
            else
                HandleFollow();
        }
        return true;
    }

    // Step out of ground effects, traps, and trigger creatures when +avoid aoe.
    if ((_bot->IsInCombat() || GroupInCombat()) && TryAvoidAoe())
        return true;

    // Between pulls: sit/drink with the party instead of chasing the next pack.
    // Explicit attack/pull orders always engage — don't stall for drinks.
    if (!HasEngageTarget() && !_bot->IsInCombat() && !GroupInCombat() && _food
        && (PartyNeedsRest() || PartyNotAlmostReady()))
        return false;

    // Sequenced pull: reach opener range, cast pull spell, then normal combat.
    if (HandlePullSequence())
        return true;

    // Tank marks the preferred RTI icon on an unmarked pack mob.
    TryAutoMarkRti();

    if (GetCombatRole() == CombatRole::Healer)
    {
        if (HandleHealing(BOT_HEAL_BELOW_PCT))
        {
            StopResting();
            return true;
        }
        // Strict heal: stay at cast range from the tank/fight — never formation-follow
        // the master mid-combat (that dragged healers through melee).
        if (!_healerDps)
        {
            if (GroupInCombat() || !_bot->getAttackers().empty())
            {
                StopResting();
                if (!_clientControlled && !_stay)
                {
                    if (Unit* anchor = SelectHealerCombatAnchor())
                        HoldRangedCombatPosition(anchor, BOT_CAST_DIST);
                }
                return true;
            }
            return false;
        }
    }
    else if (_offHeal && CanOffHealClass() && HandleHealing(BOT_OFFHEAL_BELOW_PCT))
    {
        StopResting();
        return true;
    }

    Unit* target = SelectTarget();
    if (!target)
    {
        // Forced target died or became invalid - drop the order.
        if (_forcedTargetGuid)
            ClearForcedTarget();

        // Nothing to fight: drop lingering attack/chase/selection so we don't
        // keep facing a corpse like a stuck loot attempt.
        if (_bot->GetVictim())
            _bot->AttackStop();
        if (Unit* selected = _bot->GetSelectedUnit())
            if (!selected->IsAlive())
                _bot->SetSelection(0);
        _chaseGuid = 0;
        StopHunterAutoShot();
        return false;
    }

    // Only abort eat/drink once we actually have something to fight.
    StopResting();
    // Guarantee dismount even when needFight was false (e.g. grind pull OOC).
    TryDismount();
    if (_travel.active)
        ClearTravelDestination();

    // Spec decides stance (e.g. Elemental ranged, Enhancement melee). Tanks always melee.
    // Healers in healer-dps mode plant like ranged — never chase into melee formation.
    bool const ranged = GetCombatRole() != CombatRole::Tank && !IsMeleeHealerDps()
        && (IsRangedClass() || GetCombatRole() == CombatRole::Healer);
    bool const inMelee = _bot->IsWithinMeleeRange(target);

    _followGuid = 0;
    _lootGuid = 0;

    // Threat throttle: stop both spells and auto-attack (not just DoRotation).
    if (ShouldThrottleThreat(target))
    {
        if (_bot->GetVictim())
            _bot->AttackStop();
        if (_bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        {
            _bot->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
            _bot->SendMeleeAttackStop(target);
        }
        // Stay near the fight without dealing damage so threat can decay.
        if (!_clientControlled && !ranged && !inMelee)
        {
            MovementGeneratorType moveType = _bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
            bool reissue = _chaseGuid != target->GetGUID()
                || moveType != CHASE_MOTION_TYPE
                || _bot->IsStopped();
            if (reissue)
            {
                if (BotMovement::MoveChase(_bot, target, 0.0f))
                    _chaseGuid = target->GetGUID();
            }
        }
        return true;
    }

    if (!ranged)
    {
        // Always open with chase when out of melee — taunt spam must not replace
        // closing the gap (casts stop movement; MoveChase is blocked while casting).
        if (!inMelee && !_clientControlled)
        {
            MovementGeneratorType moveType = _bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
            bool const casting = IsBusyCasting(_bot);
            bool reissue = _chaseGuid != target->GetGUID()
                || moveType != CHASE_MOTION_TYPE
                || _bot->IsStopped();
            if (reissue && !casting)
            {
                if (BotMovement::MoveChase(_bot, target, 0.0f))
                    _chaseGuid = target->GetGUID();
            }
        }
        else
            _chaseGuid = target->GetGUID();

        _bot->Attack(target, true);
        DebugCombat("Melee auto-attack");

        if (inMelee)
        {
            if (!_bot->HasInArc(static_cast<float>(M_PI), target))
                _bot->SetInFront(target);
            if (GetCombatRole() == CombatRole::Tank && _tankMode)
            {
                if (DoTankExtras(target))
                    return true; // pull Thunder Clap (etc.) spent the GCD
            }
            DoRotation(target);
        }
        else
        {
            // Closing: Charge first (Throw only if Charge unavailable). Tanks may
            // also fire off-GCD Taunt after Charge — never Charge+Throw same tick.
            if (!_bot->HasInArc(static_cast<float>(M_PI), target))
                _bot->SetInFront(target);
            if (GetCombatRole() == CombatRole::Tank && _tankMode)
                DoTankExtras(target, /*closing=*/true);
            else if (_bot->getClass() == CLASS_WARRIOR)
                TryWarriorGapClose(target);
        }

        return true;
    }

    // Ranged / caster: never chase to contact. Walk to a cast-range point on the
    // bot's side of the target, then plant and cast when in range with LoS.
    float const dist = _bot->GetDistance(target);
    // A sweeping hazard (rotating fire wall) moves slowly near the boss it turns around, so a short sidestep
    // there buys time to cast; at full range it outruns the bot and nobody casts at all.
    float const castDist = (AvoidedMovingHazardRecently() && IsUntankedBoss(target)) ? 10.0f : BOT_CAST_DIST;
    bool const inRange = dist <= castDist;
    bool const hasLos = _bot->IsWithinLOSInMap(target);
    // Treat Auto Shot / wand as background fire — still plant/face between GCDs.
    bool const casting = _bot->IsNonMeleeSpellCasted(false, false, true)
        || _bot->HasUnitState(UNIT_STATE_CASTING);

    if (_debugCombat && IsHunterRanged())
    {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "CombatGate dist=%.1f inRange=%d los=%d casting=%d stopped=%d",
            dist, inRange ? 1 : 0, hasLos ? 1 : 0, casting ? 1 : 0, _bot->IsStopped() ? 1 : 0);
        HunterDebugLog(buf);
    }

    // Ensure we are not in a melee-attack state (chase _reachTarget can force it).
    if (_bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
    {
        _bot->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
        _bot->SendMeleeAttackStop(target);
    }

    // Hunters: Auto Shot only (never Attack — that stutter-cancels autorepeat).
    // Pure casters: never weapon-auto; set target and cast.
    // Others on this path shouldn't happen.
    if (IsHunterRanged())
    {
        _bot->SetSelection(target->GetGUID());
    }
    else if (IsPureCaster())
    {
        // Target for spells only — never Attack() (weapon swing / wand).
        _bot->SetSelection(target->GetGUID());
    }
    else
        _bot->Attack(target, false);

    // Too close for casting: hunters always step out — ranged shots fail in melee.
    // Other casters only kite when a tank holds the mob in a dungeon.
    if (!casting && (_bot->IsWithinMeleeRange(target) || dist < castDist * 0.40f))
    {
        if (IsHunterRanged() || WantsRangedKiting())
        {
            if (TryCombatFlee(target))
            {
                if (_debugCombat && IsHunterRanged())
                    HunterDebugLog("CombatGate SKIP DoRotation (flee/kite)");
                return true;
            }
        }
    }

    if (inRange && hasLos)
    {
        // Never StopMoving / Clear while casting — that interrupts the spell.
        // Also skip if already planted: StopAndIdle every tick pauses Auto Shot.
        if (!casting && !_bot->IsStopped())
        {
            BotMovement::StopAndIdle(_bot);
            _bot->SetSelection(target->GetGUID());
            BotMovement::FaceUnit(_bot, target);
        }
        else if (!casting)
        {
            _bot->SetSelection(target->GetGUID());
            if (!_bot->HasInArc(static_cast<float>(M_PI), target))
                BotMovement::FaceUnit(_bot, target);
        }

        _chaseGuid = target->GetGUID();
        StartHunterAutoShot(target);
        DoRotation(target);
        return true;
    }

    if (_debugCombat && IsHunterRanged())
        HunterDebugLog("CombatGate SKIP DoRotation (move to cast range)");

    // Too far, or in range but LoS blocked: keep closing until LoS opens.
    // Parking at cast-range on the wrong side of a corner never clears LoS —
    // step toward the focus instead of reissuing the same stand-off point.
    {
        float destX, destY, destZ;
        float const standDist = hasLos ? (castDist * 0.85f)
                                       : std::max(5.0f, dist * 0.55f);
        float const absAngle = target->GetAngle(_bot);
        target->GetNearPoint(_bot, destX, destY, destZ, _bot->GetObjectSize(), standDist, absAngle);
        _bot->UpdateAllowedPositionZ(destX, destY, destZ);

        MovementGeneratorType moveType = _bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
        bool reissue = _chaseGuid != target->GetGUID()
            || moveType != POINT_MOTION_TYPE
            || _bot->IsStopped()
            || !hasLos;
        if (reissue)
        {
            MoveToPosition(destX, destY, destZ);
            _chaseGuid = target->GetGUID();
        }
    }

    return true;
}

// Self-bot combat: never touch MotionMaster. Cast when the player is already in
// range with LoS; they steer into position themselves.
bool PlayerbotAI::HandleCombatCastOnly()
{
    // Drop refreshment as soon as combat is relevant.
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty())
    {
        if (_resting || _bot->IsSitState() || HasFoodOrDrinkAura())
            StopResting();
    }

    // Seated / mid-cast OOC: never clear rest or start a rotation — that cancelled
    // clicked food/drink and wiped eat/drink state every AI tick.
    if (!_bot->IsInCombat() && _bot->getAttackers().empty() && !GroupInCombat())
    {
        if (_bot->IsSitState() || IsBusyCasting(_bot) || HasFoodOrDrinkAura())
            return false;
    }

    // Heal only when someone is actually injured; otherwise healer-dps may attack.
    if (GetCombatRole() == CombatRole::Healer)
    {
        if (HandleHealing(BOT_HEAL_BELOW_PCT))
        {
            StopResting();
            return true;
        }
        if (!_healerDps)
            return GroupInCombat() || !_bot->getAttackers().empty();
    }
    else if (_offHeal && CanOffHealClass() && HandleHealing(BOT_OFFHEAL_BELOW_PCT))
    {
        StopResting();
        return true;
    }

    Unit* target = SelectTarget();
    if (!target)
    {
        if (_forcedTargetGuid)
            ClearForcedTarget();
        StopHunterAutoShot();
        return false;
    }

    StopResting();

    // Threat throttle: no auto-attack and no rotation.
    if (ShouldThrottleThreat(target))
    {
        if (_bot->GetVictim())
            _bot->AttackStop();
        if (_bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        {
            _bot->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
            _bot->SendMeleeAttackStop(target);
        }
        return true;
    }

    bool const ranged = (GetCombatRole() == CombatRole::Healer && !IsMeleeHealerDps())
        || (IsRangedClass() && GetCombatRole() != CombatRole::Tank);
    if (ranged)
    {
        if (_bot->GetDistance(target) > BOT_CAST_DIST || !_bot->IsWithinLOSInMap(target))
            return true;
        if (_bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
        {
            _bot->ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
            _bot->SendMeleeAttackStop(target);
        }
        _bot->SetSelection(target->GetGUID());
        StartHunterAutoShot(target);
        DoRotation(target);
    }
    else
    {
        // Self-bot assist: one engage path only (Charge → optional Taunt → Throw
        // if Charge unavailable). Do NOT also call GetPullOpenerSpell — that cast
        // Heroic Throw in the same tick and cancelled Charge via StopMoving.
        if (!_bot->IsWithinMeleeRange(target))
        {
            if (_bot->getClass() == CLASS_WARRIOR
                || (GetCombatRole() == CombatRole::Tank && _tankMode))
            {
                if (!_bot->HasInArc(static_cast<float>(M_PI), target))
                    _bot->SetInFront(target);
                DoTankExtras(target, /*closing=*/true);
            }
            return true;
        }
        _bot->Attack(target, true);
        if (GetCombatRole() == CombatRole::Tank && _tankMode)
        {
            if (DoTankExtras(target))
                return true; // pull pack AoE spent the GCD
        }
        DoRotation(target);
    }
    return true;
}

// Target priority (AC Values: pull / tank / rti / dps / current):
//   1) Tanks: urgent peels (mob on healer/DPS) — beats pull/forced/RTI
//   2) Pull / forced command target
//   3) Tanks: hold pack / remaining peels
//   4) DPS assist: main tank's victim (before own-attacker peels / least-HP)
//   5) Own attackers
//   6) Group combat: RTI mark → dps assist → tank victim fallback
//   7) Grind (explicit) / self-bot selected unit
Unit* PlayerbotAI::SelectTarget()
{
    // Healer/DPS under attack always outranks skull / pull focus.
    if (GetCombatRole() == CombatRole::Tank && _tankMode)
        if (Unit* peel = SelectTankTarget())
            if (IsUrgentTankPeel(peel))
            {
                _targets.SetCurrentTarget(peel);
                return peel;
            }

    if (Unit* pull = _targets.GetPullTarget(this))
        return pull;
    if (Unit* forced = GetForcedTarget())
    {
        _targets.SetCurrentTarget(forced);
        return forced;
    }

    // Tanks in tank-mode own peel / pack selection.
    if (GetCombatRole() == CombatRole::Tank && _tankMode)
        if (Unit* tankTarget = _targets.GetTankTarget(this))
        {
            _targets.SetCurrentTarget(tankTarget);
            return tankTarget;
        }

    // Passive bots only fight back; they do not assist or pull.
    if (_passive)
    {
        for (Unit* attacker : _bot->getAttackers())
            if (attacker && attacker->IsAlive() && _bot->IsValidAttackTarget(attacker))
            {
                _targets.SetCurrentTarget(attacker);
                return attacker;
            }
        return nullptr;
    }

    // The tank holds the adds while everyone else brings the untanked boss down.
    if (GetCombatRole() != CombatRole::Tank)
        if (Unit* boss = SelectUntankedBoss())
        {
            _targets.SetCurrentTarget(boss);
            return boss;
        }

    // With dps assist, lock onto the tank's focus before peeling onto whatever
    // just hit us (fresh chain-pull / ranged add). Own attackers still win if
    // the tank has no victim yet.
    bool const preferTankFocus = _dpsAssist && GetCombatRole() != CombatRole::Tank;
    if (preferTankFocus)
    {
        if (Unit* tankAssist = _targets.GetAssistTankTarget(this))
        {
            _targets.SetCurrentTarget(tankAssist);
            return tankAssist;
        }
    }

    for (Unit* attacker : _bot->getAttackers())
        if (attacker && attacker->IsAlive() && _bot->IsValidAttackTarget(attacker))
        {
            _holdAssist = false; // got aggro — fight back
            _targets.SetCurrentTarget(attacker);
            return attacker;
        }

    // @tank attack: non-tanks hold until a mob is actually swinging on the party.
    if (_holdAssist)
    {
        if (SelectGroupThreatTarget())
            _holdAssist = false;
        else
            return nullptr;
    }

    // AC dps assist: damage bots without +dps assist only fight forced/own aggro.
    // Tanks/healers may still fall through to RTI when no peel target was found.
    bool const canAssist = (GetCombatRole() != CombatRole::Damage) || _dpsAssist || _grind;

    if (canAssist)
    {
        // Prefer the configured raid-target icon (default skull) for focus fire.
        if (Unit* rti = _targets.GetRtiTarget(this))
        {
            _targets.SetCurrentTarget(rti);
            return rti;
        }
        if (Unit* dps = _targets.GetDpsTarget(this))
        {
            _targets.SetCurrentTarget(dps);
            return dps;
        }
        if (Unit* tankAssist = _targets.GetAssistTankTarget(this))
        {
            _targets.SetCurrentTarget(tankAssist);
            return tankAssist;
        }
    }

    // Auto-quest: kill incomplete NPC objectives in range before generic grind.
    if (_quests)
    {
        Map* map = _bot->GetMap();
        bool const inInstance = map && map->IsInstance();
        if (!inInstance && !PartyNeedsRest() && !PartyNotAlmostReady())
            if (Unit* questTarget = SelectQuestObjectiveTarget())
            {
                _targets.SetCurrentTarget(questTarget);
                return questTarget;
            }
    }

    if (_grind)
    {
        Map* map = _bot->GetMap();
        bool const inInstance = map && map->IsInstance();
        if (!inInstance && !PartyNeedsRest() && !PartyNotAlmostReady())
            if (Unit* grind = SelectGrindTarget())
            {
                _targets.SetCurrentTarget(grind);
                return grind;
            }
    }

    // Self-bot: use the player's current hostile selection (open or in combat).
    if (_clientControlled)
    {
        if (Unit* selected = _bot->GetSelectedUnit())
            if (selected->IsAlive() && _bot->IsValidAttackTarget(selected))
            {
                _targets.SetCurrentTarget(selected);
                return selected;
            }
    }

    return nullptr;
}

Unit* PlayerbotAI::SelectGrindTarget()
{
    Unit* target = nullptr;
    Skyfire::NearestAttackableUnitInObjectRangeCheck check(_bot, _bot, BOT_LOOT_SEEK_DIST);
    Skyfire::UnitLastSearcher<Skyfire::NearestAttackableUnitInObjectRangeCheck> searcher(_bot, target, check);
    _bot->VisitNearbyObject(BOT_LOOT_SEEK_DIST, searcher);
    if (target && _bot->IsValidAttackTarget(target))
        return target;
    return nullptr;
}

Unit* PlayerbotAI::SelectQuestObjectiveTarget()
{
    if (!_bot)
        return nullptr;

    std::unordered_set<uint32> needEntries;
    for (auto const& qs : _bot->getQuestStatusMap())
    {
        if (qs.second.Status != QUEST_STATUS_INCOMPLETE)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(qs.first);
        if (!quest)
            continue;

        uint16 const slot = _bot->FindQuestSlot(qs.first);
        if (slot >= MAX_QUEST_LOG_SIZE)
            continue;

        for (QuestObjective const* obj : quest->m_questObjectives)
        {
            if (!obj || obj->Type != QUEST_OBJECTIVE_TYPE_NPC)
                continue;
            uint16 const have = _bot->GetQuestSlotCounter(slot, obj->Index);
            uint16 const needAmt = uint16(obj->Amount > 0 ? obj->Amount : 1);
            if (have >= needAmt)
                continue;
            needEntries.insert(obj->ObjectId);
        }
    }
    if (needEntries.empty())
        return nullptr;

    // Prefer objective mobs out to hunt radius before generic grind (25y).
    Unit* best = nullptr;
    float bestDist = BOT_QUEST_HUNT_RADIUS;
    std::list<Unit*> units;
    Skyfire::AnyUnfriendlyUnitInObjectRangeCheck check(_bot, _bot, BOT_QUEST_HUNT_RADIUS);
    Skyfire::UnitListSearcher<Skyfire::AnyUnfriendlyUnitInObjectRangeCheck> searcher(_bot, units, check);
    _bot->VisitNearbyObject(BOT_QUEST_HUNT_RADIUS, searcher);

    for (Unit* unit : units)
    {
        if (!unit || !unit->IsAlive() || !needEntries.count(unit->GetEntry()))
            continue;
        if (!_bot->IsValidAttackTarget(unit))
            continue;
        float const dist = _bot->GetDistance(unit);
        if (dist > bestDist)
            continue;
        bestDist = dist;
        best = unit;
    }
    return best;
}

Unit* PlayerbotAI::GetForcedTarget() const
{
    if (!_forcedTargetGuid)
        return nullptr;

    Unit* target = ObjectAccessor::GetUnit(*_bot, _forcedTargetGuid);
    if (!IsSafeAttackTarget(target))
        return nullptr;
    return target;
}

void PlayerbotAI::SetForcedTarget(Unit* target)
{
    _forcedTargetGuid = target ? target->GetGUID() : 0;
    _chaseGuid = 0;
    _targets.SetPullTarget(target);
}

void PlayerbotAI::ClearForcedTarget()
{
    _forcedTargetGuid = 0;
    _targets.SetPullTarget(nullptr);
    _pullPhase = PullPhase::None;
    _pullStartTime = 0;
}

PlayerbotAI::CombatRole PlayerbotAI::GetCombatRole() const
{
    uint32 specId = _bot->GetTalentSpecialization(_bot->GetActiveSpec());
    uint8 cls = _bot->getClass();

    // Match the same role-to-spec mapping used at character create / init.
    auto isSpec = [&](uint8 tab) -> bool
    {
        uint32 const* specs = GetClassSpecializations(cls);
        return specs && specs[tab] == specId;
    };

    switch (cls)
    {
        case CLASS_WARRIOR:      return isSpec(2) ? CombatRole::Tank : CombatRole::Damage;
        case CLASS_PALADIN:      return isSpec(1) ? CombatRole::Tank : (isSpec(0) ? CombatRole::Healer : CombatRole::Damage);
        case CLASS_DEATH_KNIGHT: return isSpec(0) ? CombatRole::Tank : CombatRole::Damage;
        case CLASS_PRIEST:       return isSpec(2) ? CombatRole::Damage : CombatRole::Healer;
        case CLASS_SHAMAN:       return isSpec(2) ? CombatRole::Healer : CombatRole::Damage;
        case CLASS_MONK:         return isSpec(0) ? CombatRole::Tank : (isSpec(1) ? CombatRole::Healer : CombatRole::Damage);
        case CLASS_DRUID:        return isSpec(2) ? CombatRole::Tank : (isSpec(3) ? CombatRole::Healer : CombatRole::Damage);
        default:                 return CombatRole::Damage;
    }
}

bool PlayerbotAI::MatchesRoleFilter(std::string const& filter) const
{
    if (filter == "tank")
        return GetCombatRole() == CombatRole::Tank;
    if (filter == "heal" || filter == "healer")
        return GetCombatRole() == CombatRole::Healer;
    if (filter == "dps" || filter == "damage")
        return GetCombatRole() == CombatRole::Damage;
    if (filter == "ranged")
        return IsRangedClass() && GetCombatRole() != CombatRole::Tank;
    return false;
}

bool PlayerbotAI::IsRangedClass() const
{
    uint32 specId = _bot->GetTalentSpecialization(_bot->GetActiveSpec());
    uint8 cls = _bot->getClass();

    auto isSpec = [&](uint8 tab) -> bool
    {
        uint32 const* specs = GetClassSpecializations(cls);
        return specs && specs[tab] == specId;
    };

    switch (cls)
    {
        case CLASS_HUNTER:
        case CLASS_PRIEST:
        case CLASS_MAGE:
        case CLASS_WARLOCK:
            return true;
        case CLASS_SHAMAN:
            // Elemental = tab 0; Enhancement/Resto melee for now.
            return isSpec(0);
        case CLASS_DRUID:
            // Balance = tab 0.
            return isSpec(0);
        default:
            return false;
    }
}

bool PlayerbotAI::IsHunterRanged() const
{
    return _bot && _bot->getClass() == CLASS_HUNTER;
}

bool PlayerbotAI::IsPureCaster() const
{
    // Weapon swings / Attack() are useless — only cast spells.
    if (!_bot || IsHunterRanged() || IsMeleeHealerDps())
        return false;
    return IsRangedClass() || GetCombatRole() == CombatRole::Healer;
}

bool PlayerbotAI::IsMeleeHealerDps() const
{
    return _bot && _healerDps && _bot->getClass() == CLASS_MONK
        && GetCombatRole() == CombatRole::Healer;
}

void PlayerbotAI::DebugCombat(std::string const& action)
{
    if (!_debugCombat || !_bot || action.empty())
        return;
    // Always mirror chat lines into the hunter file log (no dedup there).
    if (IsHunterRanged())
        HunterDebugLog(std::string("CHAT ") + action);
    uint32 const now = getMSTime();
    if (action == _debugLastAction && getMSTimeDiff(_debugLastMs, now) < 800)
        return;
    _debugLastAction = action;
    _debugLastMs = now;
    _bot->Say(std::string("Combat: ") + action, Language::LANG_UNIVERSAL);
}

namespace
{
    char const* HunterSpellStateName(uint32 state)
    {
        switch (state)
        {
            case SPELL_STATE_NULL: return "NULL";
            case SPELL_STATE_PREPARING: return "PREPARING";
            case SPELL_STATE_CASTING: return "CASTING";
            case SPELL_STATE_FINISHED: return "FINISHED";
            case SPELL_STATE_IDLE: return "IDLE";
            case SPELL_STATE_DELAYED: return "DELAYED";
            default: return "?";
        }
    }

    void AppendHunterSpellSlot(std::ostringstream& os, Player* bot, CurrentSpellTypes slot, char const* label)
    {
        Spell* spell = bot->GetCurrentSpell(slot);
        if (!spell)
        {
            os << label << "=none ";
            return;
        }
        SpellInfo const* info = spell->GetSpellInfo();
        os << label << '=' << (info ? info->Id : 0)
           << ':' << HunterSpellStateName(spell->getState())
           << ":ct=" << spell->GetCastTime() << ' ';
    }

    std::mutex s_hunterDebugLogMutex;
}

std::string PlayerbotAI::BuildHunterStateSnapshot(Unit* target) const
{
    std::ostringstream os;
    if (!_bot)
        return "no-bot";

    uint32 const focus = _bot->GetPower(POWER_FOCUS);
    uint32 const focusMax = _bot->GetMaxPower(POWER_FOCUS);
    SpellInfo const* arcane = sSpellMgr->GetSpellInfo(3044);
    bool const gcd = arcane && _bot->GetGlobalCooldownMgr().HasGlobalCooldown(arcane);

    os << "focus=" << focus << '/' << focusMax
       << " gcd=" << (gcd ? 1 : 0)
       << " cdArcane=" << (_bot->HasSpellCooldown(3044) ? 1 : 0)
       << " cdSteady=" << (_bot->HasSpellCooldown(56641) ? 1 : 0)
       << " stopped=" << (_bot->IsStopped() ? 1 : 0)
       << " castingState=" << (_bot->HasUnitState(UNIT_STATE_CASTING) ? 1 : 0)
       << " nonMelee=" << (_bot->IsNonMeleeSpellCasted(false, false, true) ? 1 : 0)
       << " nonMeleeAutoIgnore=" << (_bot->IsNonMeleeSpellCasted(true, false, true) ? 1 : 0)
       << " autoGuid=" << _autoShotGuid << ' ';

    AppendHunterSpellSlot(os, _bot, CURRENT_GENERIC_SPELL, "GEN");
    AppendHunterSpellSlot(os, _bot, CURRENT_CHANNELED_SPELL, "CHAN");
    AppendHunterSpellSlot(os, _bot, CURRENT_AUTOREPEAT_SPELL, "AUTO");
    AppendHunterSpellSlot(os, _bot, CURRENT_MELEE_SPELL, "MELEE");

    if (target)
    {
        os << "tgt=" << target->GetEntry()
           << " dist=" << _bot->GetDistance(target)
           << " alive=" << (target->IsAlive() ? 1 : 0)
           << " inArc=" << (_bot->HasInArc(static_cast<float>(M_PI), target) ? 1 : 0);
    }
    else
        os << "tgt=none";

    return os.str();
}

void PlayerbotAI::HunterDebugLog(std::string const& line)
{
    if (!_debugCombat || !_bot || line.empty())
        return;

    if (_hunterDebugLogPath.empty())
    {
        std::string logsDir = sConfigMgr->GetStringDefault("LogsDir", "");
        if (!logsDir.empty() && logsDir.back() != '/' && logsDir.back() != '\\')
            logsDir.push_back('/');
        _hunterDebugLogPath = logsDir + "playerbot_hunter_" + _bot->GetName() + ".log";
    }

    uint32 const seq = ++_hunterDebugSeq;
    uint32 const ms = getMSTime();
    std::ostringstream out;
    out << '[' << ms << " #" << seq << "] " << _bot->GetName() << ": " << line;

    std::string const text = out.str();
    {
        std::lock_guard<std::mutex> lock(s_hunterDebugLogMutex);
        std::ofstream file(_hunterDebugLogPath.c_str(), std::ios::app);
        if (file)
        {
            file << text << '\n';
            file.flush();
        }
    }

    // Also mirror into Server.log when misc logging is enabled.
    sLog->outMessage("misc", LogLevel::LOG_LEVEL_INFO, "%s", text.c_str());
}

// One iconic, low-level "filler" attack per class. Used when no Wave-1
// per-spec priority list returns a spell.
uint32 PlayerbotAI::GetFillerSpell() const
{
    switch (_bot->getClass())
    {
        case CLASS_PALADIN: return 35395; // Crusader Strike
        case CLASS_HUNTER:
            // Prefer Steady when Arcane is unaffordable — otherwise filler idles.
            if (_bot->GetPower(POWER_FOCUS) < 30 && _bot->HasSpell(56641))
                return 56641; // Steady Shot
            if (_bot->HasSpell(77767))
                return 77767; // Cobra Shot
            if (_bot->HasSpell(56641))
                return 56641;
            return 3044;      // Arcane Shot
        case CLASS_ROGUE:
        {
            // Only prefer Mutilate when both daggers are equipped.
            Item* mh = _bot->GetWeaponForAttack(WeaponAttackType::BASE_ATTACK, true);
            Item* oh = _bot->GetWeaponForAttack(WeaponAttackType::OFF_ATTACK, true);
            bool const daggers = mh && oh
                && mh->GetTemplate() && oh->GetTemplate()
                && mh->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER
                && oh->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER;
            if (daggers && _bot->HasSpell(1329))
                return 1329;   // Mutilate
            if (mh && mh->GetTemplate() && mh->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER
                && _bot->HasSpell(111240))
                return 111240; // Dispatch
            if (_bot->HasSpell(16511))
                return 16511;  // Hemorrhage
            return 1752;       // Sinister Strike
        }
        case CLASS_PRIEST:  return 585;   // Smite
        case CLASS_MAGE:    return 116;   // Frostbolt
        case CLASS_WARLOCK: return 686;   // Shadow Bolt
        case CLASS_SHAMAN:  return 403;   // Lightning Bolt (ranged) / melee still auto-attacks
        case CLASS_MONK:    return 100780; // Jab
        case CLASS_WARRIOR:
            if (_bot->HasSpell(23922) && BotRotation::HasShieldEquipped(_bot))
                return 23922; // Shield Slam
            if (_bot->HasSpell(20243))
                return 20243; // Devastate
            if (_bot->HasSpell(7386))
                return 7386;  // Sunder Armor
            if (_bot->HasSpell(78))
                return 78;    // Heroic Strike
            return 0;
        default:            return 0;
    }
}

void PlayerbotAI::StopHunterAutoShot()
{
    _autoShotGuid = 0;
    if (!_bot)
        return;
    Spell* autoRepeat = _bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL);
    if (!autoRepeat || autoRepeat->GetSpellInfo()->Id != 75)
        return;
    _bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL, false, true);
}

// Arm Auto Shot once per target. Core autorepeat keeps it firing — do not
// re-cast every tick and do not gate the GCD rotation on this.
void PlayerbotAI::StartHunterAutoShot(Unit* target)
{
    if (!_bot || !target || !target->IsAlive())
        return;
    if (!IsHunterRanged())
        return;
    if (!_bot->HasSpell(75))
        _bot->learnSpell(75, false);
    if (!_bot->HasSpell(75))
    {
        HunterDebugLog("AutoShot SKIP no spell 75");
        return;
    }

    if (Spell* autoRepeat = _bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
    {
        if (autoRepeat->GetSpellInfo()->Id == 75
            && autoRepeat->m_targets.GetUnitTargetGUID() == target->GetGUID())
        {
            _autoShotGuid = target->GetGUID();
            return;
        }
        HunterDebugLog(std::string("AutoShot REARM wrong channel ") + BuildHunterStateSnapshot(target));
    }

    // Lost the channel — allow re-arm.
    if (_autoShotGuid == target->GetGUID())
        _autoShotGuid = 0;

    // Don't clip a Steady cast bar just to re-arm Auto Shot.
    if (Spell* generic = _bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
        if (generic->getState() == SPELL_STATE_PREPARING && generic->GetCastTime() > 1000)
        {
            HunterDebugLog(std::string("AutoShot SKIP rearm during cast ") + BuildHunterStateSnapshot(target));
            return;
        }

    _bot->SetSelection(target->GetGUID());
    _bot->CastSpell(target, 75, false);
    if (_bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL)
        && _bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL)->GetSpellInfo()->Id == 75)
    {
        _autoShotGuid = target->GetGUID();
        DebugCombat("Auto Shot");
        HunterDebugLog(std::string("AutoShot ARMED ") + BuildHunterStateSnapshot(target));
    }
    else
        HunterDebugLog(std::string("AutoShot FAIL arm ") + BuildHunterStateSnapshot(target));
}

void PlayerbotAI::BackOffSpell(uint32 spellId)
{
    if (spellId)
        _spellBackoff[spellId] = getMSTime() + BOT_SPELL_BACKOFF_MS;
}

bool PlayerbotAI::IsSpellBackedOff(uint32 spellId) const
{
    auto const it = _spellBackoff.find(spellId);
    return it != _spellBackoff.end() && int32(it->second - getMSTime()) > 0;
}

void PlayerbotAI::NoteSpellCast(uint32 spellId, uint64 targetGuid)
{
    if (spellId)
        _lastSpellCast[spellId] = std::make_pair(targetGuid, getMSTime());
}

bool PlayerbotAI::CastOnRecently(uint32 spellId, uint64 targetGuid, uint32 withinMs) const
{
    auto const it = _lastSpellCast.find(spellId);
    return it != _lastSpellCast.end() && it->second.first == targetGuid
        && getMSTimeDiff(it->second.second, getMSTime()) < withinMs;
}

void PlayerbotAI::DoRotation(Unit* target)
{
    if (!target || !target->IsAlive())
        return;

    if (_bot->HasUnitState(UNIT_STATE_CASTING) && !_bot->IsNonMeleeSpellCasted(false, false, true))
        _bot->ClearUnitState(UNIT_STATE_CASTING);

    // Wait out cast bars (Steady ~2s, Arcane ammo-delay ~500ms). Auto Shot stays on.
    if (Spell* casting = _bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
    {
        if (casting->getState() == SPELL_STATE_PREPARING && casting->GetCastTime() > 0)
        {
            if (IsHunterRanged())
                HunterDebugLog(std::string("WAIT preparing ") + BuildHunterStateSnapshot(target));
            return;
        }
    }
    if (_bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        if (IsHunterRanged())
            HunterDebugLog(std::string("WAIT channeled ") + BuildHunterStateSnapshot(target));
        return;
    }

    if (ShouldThrottleThreat(target))
    {
        if (IsHunterRanged())
            HunterDebugLog(std::string("SKIP threat-throttle ") + BuildHunterStateSnapshot(target));
        return;
    }

    if (BotRotation::TryInterrupt(_bot, target))
        return;
    // Raid buffs (MotW etc.) are one party-wide cast. Keep them up in combat so a
    // combat-rez'd ally gets the buff without waiting for the fight to end.
    if (BotRotation::TryMaintainBuffs(_bot))
        return;
    if (BotRotation::IsBursting(_bot) && BotRotation::TryTrinkets(_bot))
        return;

    if (!_clientControlled && !_stay && !_bot->IsStopped())
        BotMovement::StopAndIdle(_bot);

    // Hunter: Auto Shot stays armed. GCD shots only:
    //   focus >= 30 → Arcane Shot (30 focus)
    //   else         → Steady Shot (builder)
    if (IsHunterRanged())
    {
        if (!_bot->HasSpell(3044))
            _bot->learnSpell(3044, false);
        if (!_bot->HasSpell(56641))
            _bot->learnSpell(56641, false);

        SpellInfo const* gcdInfo = sSpellMgr->GetSpellInfo(3044);
        if (gcdInfo && _bot->GetGlobalCooldownMgr().HasGlobalCooldown(gcdInfo))
        {
            HunterDebugLog(std::string("WAIT gcd ") + BuildHunterStateSnapshot(target));
            return;
        }

        uint32 const focus = _bot->GetPower(POWER_FOCUS);
        HunterDebugLog(std::string("TICK decide ") + BuildHunterStateSnapshot(target));

        if (uint32 const specShot = BotRotation::SelectNextSpell(_bot, target))
        {
            bool const cast = BotRotation::IsSelfCastSpell(specShot)
                ? BotRotation::CastSpell(_bot, target, specShot)
                : BotRotation::CastHunterShot(_bot, target, specShot);
            if (cast)
                return;
            HunterDebugLog(std::string("Spec shot FAILED id=") + std::to_string(specShot));
            BackOffSpell(specShot);
        }

        if (_bot->HasSpell(1978) && !target->GetAura(118253) && !target->GetAura(1978))
        {
            HunterDebugLog("TRY Serpent Sting");
            if (BotRotation::CastHunterShot(_bot, target, 1978))
                return;
        }

        if (focus >= 30)
        {
            HunterDebugLog("TRY Arcane Shot (focus>=30)");
            if (BotRotation::CastHunterShot(_bot, target, 3044))
                return;
            HunterDebugLog(std::string("Arcane FAILED fallthrough ") + BuildHunterStateSnapshot(target));
        }
        else
        {
            HunterDebugLog("TRY Steady Shot (focus<30)");
            if (BotRotation::CastHunterShot(_bot, target, 56641))
                return;
            HunterDebugLog(std::string("Steady FAILED ") + BuildHunterStateSnapshot(target));
            if (BotRotation::CastHunterShot(_bot, target, 77767))
                return;
        }

        // Focus high but Arcane failed — still try Steady rather than idle.
        if (focus >= 30)
        {
            HunterDebugLog("TRY Steady fallback after Arcane fail");
            if (BotRotation::CastHunterShot(_bot, target, 56641))
                return;
            if (BotRotation::CastHunterShot(_bot, target, 77767))
                return;
        }

        if (_debugCombat)
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "idle focus=%u", focus);
            DebugCombat(buf);
            HunterDebugLog(std::string("IDLE ") + BuildHunterStateSnapshot(target));
        }
        return;
    }

    uint32 const selected = BotRotation::SelectNextSpell(_bot, target);
    if (selected)
    {
        if (BotRotation::CastSpell(_bot, target, selected))
            return;
        BackOffSpell(selected);
    }

    if (BotRotation::TryRacial(_bot, target))
        return;
    if (BotRotation::TryTrinkets(_bot))
        return;

    uint32 const filler = GetFillerSpell();
    if (filler && filler != selected && BotRotation::CastSpell(_bot, target, filler))
        return;
}

bool PlayerbotAI::ShouldThrottleThreat(Unit* target) const
{
    if (!_threat || !_bot || !target)
        return false;
    // Self-bot: the player is steering — never freeze their swing/rotation.
    if (_clientControlled)
        return false;
    if (GetCombatRole() == CombatRole::Tank && _tankMode)
        return false;
    if (!target->CanHaveThreatList())
        return false;

    float const myThreat = target->getThreatManager().getThreat(_bot);
    if (myThreat <= 0.0f)
        return false;

    // Only throttle against an actual party tank's threat. Falling back to the
    // mob's current top threat made solo / all-DPS bots AttackStop themselves
    // after the first hit (they were their own reference).
    float tankThreat = 0.0f;
    if (Group* group = _bot->GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsAlive() || member == _bot)
                continue;

            uint32 specId = member->GetTalentSpecialization(member->GetActiveSpec());
            uint8 cls = member->getClass();
            uint32 const* specs = GetClassSpecializations(cls);
            bool isTank = false;
            if (specs)
            {
                switch (cls)
                {
                    case CLASS_WARRIOR: isTank = (specId == specs[2]); break;
                    case CLASS_PALADIN: isTank = (specId == specs[1]); break;
                    case CLASS_DEATH_KNIGHT: isTank = (specId == specs[0]); break;
                    case CLASS_DRUID: isTank = (specId == specs[2]); break;
                    case CLASS_MONK: isTank = (specId == specs[0]); break;
                    default: break;
                }
            }
            if (!isTank)
                continue;

            float const t = target->getThreatManager().getThreat(member);
            if (t > tankThreat)
                tankThreat = t;
        }
    }

    if (tankThreat <= 0.0f)
        return false;

    float const pct = sPlayerbotMgr->GetThreatThrottlePct() / 100.0f;
    return myThreat >= tankThreat * pct;
}

bool PlayerbotAI::TryAcceptResurrect()
{
    if (!_bot || _bot->IsAlive())
        return false;
    // Real self-bots see the rez popup on their client — leave the click to them.
    if (_clientControlled)
        return false;
    if (!_bot->IsRessurectRequested())
        return false;

    _bot->ResurectUsingRequestData();

    // ResurectUsingRequestData teleports to the caster, then schedules
    // DELAYED_RESURRECT_PLAYER while IsBeingTeleported(). Socketless bots never
    // send the teleport ack, so finalize now (same pattern as LFG/summon).
    if (WorldSession* session = _bot->GetSession())
    {
        if (session->IsBot() && _bot->IsBeingTeleported())
            session->FinalizeBotTeleport();
    }

    return _bot->IsAlive();
}

bool PlayerbotAI::HandleResurrect()
{
    // Mid cast-bar (Steady, Flash Heal, etc.): do not start a rez, but return
    // false so combat/movement still run — they already wait on PREPARING.
    // Returning true here froze hunters for the whole Steady cast.
    if (IsBusyCasting(_bot))
        return false;

    // Never rez in combat — keep heals / DPS on the living group first.
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty())
        return false;

    Player* dead = BotRotation::FindPartyMemberToResurrect(_bot);
    if (!dead)
        return false;

    uint32 rezId = BotRotation::SelectResurrectSpell(_bot);
    if (!rezId)
        return false;

    constexpr float REZ_RANGE = 30.0f;
    if (!_bot->IsWithinDistInMap(dead, REZ_RANGE))
    {
        if (!_clientControlled)
        {
            _bot->GetMotionMaster()->Clear();
            _bot->GetMotionMaster()->MoveChase(dead, 0.0f);
            _chaseGuid = dead->GetGUID();
        }
        return true;
    }

    if (!_clientControlled && !_bot->IsWithinMeleeRange(dead))
        _bot->StopMoving();

    _bot->SetSelection(dead->GetGUID());
    return BotRotation::CastHealSpell(_bot, dead, rezId);
}

bool PlayerbotAI::HandleHealing(float belowPct)
{
    if (IsBusyCasting(_bot))
        return true;

    Player* ally = SelectHealTarget(belowPct);
    if (!ally)
        return false;

    float const allyPct = ally->GetMaxHealth()
        ? (100.0f * float(ally->GetHealth()) / float(ally->GetMaxHealth())) : 100.0f;
    bool const urgent = allyPct < 40.0f;

    // When someone is dying, skip buffs / utility — they ate the GCD every tick.
    if (!urgent)
    {
        if (BotRotation::TryMaintainBuffs(_bot))
            return true;

        Unit* utilityTarget = _bot->GetVictim();
        if (!IsSafeAttackTarget(utilityTarget))
        {
            utilityTarget = nullptr;
            for (Unit* attacker : _bot->getAttackers())
            {
                if (IsSafeAttackTarget(attacker))
                {
                    utilityTarget = attacker;
                    break;
                }
            }
        }
        if (BotRotation::TryCombatUtilities(_bot, utilityTarget))
            return true;
    }

    uint32 healId = BotRotation::SelectNextHeal(_bot, ally, _saveMana,
        sPlayerbotMgr->GetSaveManaThreshold());
    if (!healId)
        healId = GetHealSpell();
    if (!healId || !BotRotation::CanTryCast(_bot, healId))
        return false;

    constexpr float HEAL_RANGE = 40.0f;
    if (!_bot->IsWithinDistInMap(ally, HEAL_RANGE) || !_bot->IsWithinLOSInMap(ally))
    {
        if (!_clientControlled)
            HoldRangedCombatPosition(ally, HEAL_RANGE);
        return true;
    }

    if (!_clientControlled)
    {
        if (!_bot->IsStopped())
            _bot->StopMoving();
        _bot->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
    }

    _bot->SetSelection(ally->GetGUID());
    if (BotRotation::CastHealSpell(_bot, ally, healId))
        return true;

    // Selected cast failed to start — try the class filler heal once.
    uint32 const filler = GetHealSpell();
    if (filler && filler != healId && BotRotation::CanTryCast(_bot, filler)
        && BotRotation::CastHealSpell(_bot, ally, filler))
        return true;
    return false;
}

bool PlayerbotAI::CanOffHealClass() const
{
    if (!_bot)
        return false;
    switch (_bot->getClass())
    {
        case CLASS_PALADIN:
        case CLASS_PRIEST:
        case CLASS_SHAMAN:
        case CLASS_DRUID:
        case CLASS_MONK:
            return true;
        default:
            return false;
    }
}

Unit* PlayerbotAI::SelectTankTarget()
{
    // Without tank assist, only fight what is already on us.
    if (!_tankAssist)
    {
        for (Unit* attacker : _bot->getAttackers())
            if (attacker && attacker->IsAlive() && _bot->IsValidAttackTarget(attacker))
                return attacker;
        return nullptr;
    }

    Group* group = _bot->GetGroup();
    if (!group)
    {
        for (Unit* attacker : _bot->getAttackers())
            if (attacker && attacker->IsAlive() && _bot->IsValidAttackTarget(attacker))
                return attacker;
        return nullptr;
    }

    Unit* best = nullptr;
    int bestScore = -1;

    auto consider = [&](Unit* attacker, Player* threatenedMember)
    {
        if (!attacker || !attacker->IsAlive() || !_bot->IsValidAttackTarget(attacker))
            return;
        if (!_bot->IsWithinDistInMap(attacker, 60.0f))
            return;

        int memberPriority = threatenedMember ? ScoreTankPeelMember(threatenedMember) : 1;
        Unit* victim = attacker->GetVictim();
        int score = 0;
        if (victim && victim != _bot)
            score += 1000 + memberPriority * 100;
        else if (victim == _bot)
            score += 100; // already on us — still a candidate for multi-target
        else
            score += 50;

        // Prefer closer when scores tie.
        score -= int(_bot->GetDistance(attacker));

        if (score > bestScore)
        {
            bestScore = score;
            best = attacker;
        }
    };

    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || !member->IsAlive() || !_bot->IsInMap(member))
            continue;

        for (Unit* attacker : member->getAttackers())
            consider(attacker, member);
    }

    // Healing threat can put a mob on a healer before getAttackers() fills in.
    // Also catch hostiles whose current victim is a group ally.
    {
        std::list<Unit*> nearby;
        Skyfire::AnyUnfriendlyUnitInObjectRangeCheck check(_bot, _bot, 40.0f);
        Skyfire::UnitListSearcher<Skyfire::AnyUnfriendlyUnitInObjectRangeCheck> searcher(_bot, nearby, check);
        _bot->VisitNearbyObject(40.0f, searcher);
        for (Unit* unit : nearby)
        {
            if (!unit || !unit->IsAlive())
                continue;
            Unit* victim = unit->GetVictim();
            if (!victim || victim == _bot)
                continue;
            Player* ally = victim->ToPlayer();
            if (!ally || !_bot->IsInMap(ally) || !group->IsMember(ally->GetGUID()))
                continue;
            consider(unit, ally);
        }
    }

    return best;
}

int PlayerbotAI::ScoreTankPeelMember(Player* member) const
{
    if (!member)
        return 1;

    uint32 specId = member->GetTalentSpecialization(member->GetActiveSpec());
    uint8 cls = member->getClass();
    uint32 const* specs = GetClassSpecializations(cls);
    bool isHeal = false;
    bool isTank = false;
    if (specs)
    {
        switch (cls)
        {
            case CLASS_PALADIN: isTank = (specId == specs[1]); isHeal = (specId == specs[0]); break;
            case CLASS_WARRIOR: isTank = (specId == specs[2]); break;
            case CLASS_DEATH_KNIGHT: isTank = (specId == specs[0]); break;
            case CLASS_DRUID: isTank = (specId == specs[2]); isHeal = (specId == specs[3]); break;
            case CLASS_MONK: isTank = (specId == specs[0]); isHeal = (specId == specs[1]); break;
            case CLASS_PRIEST: isHeal = (specId != specs[2]); break;
            case CLASS_SHAMAN: isHeal = (specId == specs[2]); break;
            default: break;
        }
    }
    if (isHeal)
        return 3;
    if (!isTank)
        return 2;
    return 0; // another tank
}

bool PlayerbotAI::IsUrgentTankPeel(Unit* target) const
{
    if (!target || !_bot)
        return false;
    Unit* victim = target->GetVictim();
    if (!victim || victim == _bot)
        return false;
    Player* ally = victim->ToPlayer();
    if (!ally || !_bot->IsInMap(ally))
        return false;
    Group* group = _bot->GetGroup();
    if (!group || !group->IsMember(ally->GetGUID()))
        return false;
    // Do not treat "on another tank" as urgent — hold the pack instead.
    return ScoreTankPeelMember(ally) > 0;
}

bool PlayerbotAI::TryWarriorGapClose(Unit* target)
{
    if (!_bot || !target || _bot->getClass() != CLASS_WARRIOR)
        return false;
    if (_bot->HasUnitState(UNIT_STATE_CASTING))
        return false;

    float const dist = _bot->GetDistance(target);
    auto trySpell = [&](uint32 id, float minDist, float maxDist) -> bool
    {
        if (dist < minDist || dist > maxDist)
            return false;
        if (!_bot->HasSpell(id) || _bot->HasSpellCooldown(id))
            return false;
        if (!BotRotation::CanTryCast(_bot, id))
            return false;
        return BotRotation::CastSpell(_bot, target, id);
    };

    // 1) Charge — primary pull / engage. Never follow with Throw this tick.
    if (trySpell(100, 8.0f, 25.0f))
    {
        ArmPullPackAoeIfNeeded(target);
        return true;
    }
    // 2) Heroic Leap if Charge is unavailable (CD / OOR / unknown).
    if (trySpell(6544, 8.0f, 40.0f))
    {
        ArmPullPackAoeIfNeeded(target);
        return true;
    }
    // 3) Heroic Throw only when Charge cannot be used (on CD or out of Charge range).
    bool const chargeReady = _bot->HasSpell(100) && !_bot->HasSpellCooldown(100)
        && BotRotation::CanTryCast(_bot, 100);
    bool const inChargeRange = dist >= 8.0f && dist <= 25.0f;
    if (chargeReady && inChargeRange)
        return false; // Charge should have fired; do not Throw over it.
    if (trySpell(57755, 0.0f, 30.0f))
    {
        // Thrown into a pack — still want TC once we close.
        ArmPullPackAoeIfNeeded(target);
        return true;
    }
    return false;
}

uint32 PlayerbotAI::CountEnemiesNear(Unit* center, float range) const
{
    if (!_bot || !center)
        return 0;

    std::list<Unit*> list;
    Skyfire::AnyUnfriendlyUnitInObjectRangeCheck check(center, _bot, range);
    Skyfire::UnitListSearcher<Skyfire::AnyUnfriendlyUnitInObjectRangeCheck> searcher(center, list, check);
    center->VisitNearbyObject(range, searcher);

    uint32 count = 0;
    for (Unit* u : list)
        if (u && u->IsAlive() && _bot->IsValidAttackTarget(u))
            ++count;
    return count;
}

void PlayerbotAI::ArmPullPackAoeIfNeeded(Unit* target)
{
    if (!target || !_bot)
        return;
    // Only tanks presume pack AoE on pull.
    if (GetCombatRole() != CombatRole::Tank || !_tankMode)
        return;
    // Count around the Charge destination (the pull target), not where we stand.
    if (CountEnemiesNear(target, 10.0f) >= 2)
        _pendingPullAoe = true;
}

uint32 PlayerbotAI::GetPullPackAoeSpell() const
{
    if (!_bot)
        return 0;

    // Prefer instant pack threat on landing — Thunder Clap before Shockwave.
    uint32 candidates[3] = { 0, 0, 0 };
    switch (_bot->getClass())
    {
        case CLASS_WARRIOR:
            candidates[0] = 6343;     // Thunder Clap
            candidates[1] = 46968;    // Shockwave
            break;
        case CLASS_PALADIN:
            candidates[0] = 53595;    // Hammer of the Righteous
            candidates[1] = 26573;    // Consecration
            break;
        case CLASS_DEATH_KNIGHT:
            candidates[0] = 48721;    // Blood Boil
            candidates[1] = 43265;    // Death and Decay
            break;
        case CLASS_DRUID:
            candidates[0] = 77758;    // Thrash
            candidates[1] = 106785;   // Swipe
            break;
        case CLASS_MONK:
            candidates[0] = 121253;   // Keg Smash
            candidates[1] = 115181;   // Breath of Fire
            break;
        default:
            return 0;
    }

    for (uint32 id : candidates)
        if (id && _bot->HasSpell(id) && !_bot->HasSpellCooldown(id)
            && BotRotation::CanTryCast(_bot, id))
            return id;
    return 0;
}

bool PlayerbotAI::TryConsumePullPackAoe(Unit* target)
{
    if (!_pendingPullAoe || !_bot || !target)
        return false;

    // Wait until Charge has delivered us into the pack.
    if (!_bot->IsWithinMeleeRange(target) && _bot->GetDistance(target) > 8.0f)
        return false;

    uint32 const nearbyCount = BotRotation::CountNearbyEnemies(_bot, 10.0f);
    if (nearbyCount < 2)
    {
        _pendingPullAoe = false;
        return false;
    }

    uint32 const aoe = GetPullPackAoeSpell();
    if (!aoe)
    {
        _pendingPullAoe = false;
        return false;
    }

    if (BotRotation::CastSpell(_bot, target, aoe))
    {
        _pendingPullAoe = false;
        DebugCombat("Pull pack AoE");
        return true;
    }
    return false;
}

bool PlayerbotAI::TryTankTaunt(Unit* target)
{
    if (!_bot || !target)
        return false;

    Unit* victim = target->GetVictim();
    if (!victim || victim == _bot)
        return false;

    uint32 taunt = GetTauntSpell();
    if (!taunt || !BotRotation::CanTryCast(_bot, taunt))
        return false;

    // Righteous Defense is cast on the ally being attacked, not the mob.
    if (taunt == 31789)
    {
        if (Player* ally = victim->ToPlayer())
            return BotRotation::CastSpell(_bot, ally, taunt);
        return false;
    }
    return BotRotation::CastSpell(_bot, target, taunt);
}

bool PlayerbotAI::DoTankExtras(Unit* target, bool closing)
{
    if (!target || _bot->HasUnitState(UNIT_STATE_CASTING))
        return false;

    // Closing: Charge first, then off-GCD taunt. Never Charge+Throw same tick.
    if (closing)
    {
        if (_bot->getClass() == CLASS_WARRIOR)
            TryWarriorGapClose(target);
        else if (uint32 opener = GetPullOpenerSpell(target))
        {
            if (BotRotation::CanTryCast(_bot, opener)
                && BotRotation::CastSpell(_bot, target, opener))
                ArmPullPackAoeIfNeeded(target);
        }

        // Taunt can ride with Charge (no GCD / no StopMoving).
        if (GetCombatRole() == CombatRole::Tank && _tankMode)
            TryTankTaunt(target);
        return false; // Charge/taunt — rotation still blocked while closing
    }

    // First GCD after landing in a pack: Thunder Clap (etc.) before peels/rotation.
    if (TryConsumePullPackAoe(target))
        return true;

    // In melee: peel with taunt when focus is on someone else.
    if (GetCombatRole() == CombatRole::Tank && _tankMode)
        TryTankTaunt(target);

    // AoE threat only when something in the pack is not on us — never spam
    // Thunder Clap every GCD (that starved Shield Slam / Revenge / Devastate).
    if (BotRotation::CountNearbyEnemies(_bot, 10.0f) < 2)
        return false;

    bool needPackThreat = false;
    {
        std::list<Unit*> nearby;
        Skyfire::AnyUnfriendlyUnitInObjectRangeCheck check(_bot, _bot, 10.0f);
        Skyfire::UnitListSearcher<Skyfire::AnyUnfriendlyUnitInObjectRangeCheck> searcher(_bot, nearby, check);
        _bot->VisitNearbyObject(10.0f, searcher);
        for (Unit* enemy : nearby)
        {
            if (!enemy || !enemy->IsAlive() || !IsSafeAttackTarget(enemy))
                continue;
            Unit* ev = enemy->GetVictim();
            if (ev && ev != _bot)
            {
                needPackThreat = true;
                break;
            }
        }
    }
    if (!needPackThreat)
        return false;

    if (uint32 aoe = GetAoeThreatSpell())
        if (BotRotation::CanTryCast(_bot, aoe) && BotRotation::CastSpell(_bot, target, aoe))
            return true;
    return false;
}

Unit* PlayerbotAI::SelectGroupThreatTarget()
{
    Group* group = _bot->GetGroup();
    if (!group)
        return nullptr;

    Unit* best = nullptr;
    float bestHp = 2.0f; // fraction

    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || !member->IsAlive() || !_bot->IsInMap(member))
            continue;

        for (Unit* attacker : member->getAttackers())
        {
            if (!attacker || !attacker->IsAlive() || !_bot->IsValidAttackTarget(attacker))
                continue;
            if (!_bot->IsWithinDistInMap(attacker, 60.0f))
                continue;

            float const hp = attacker->GetMaxHealth()
                ? float(attacker->GetHealth()) / float(attacker->GetMaxHealth())
                : 1.0f;
            if (!best || hp < bestHp)
            {
                best = attacker;
                bestHp = hp;
            }
        }
    }

    return best;
}

// A dungeon boss in a phase where it fights nobody (floating, channelling) has no tank target to assist,
// so following the tank leaves damage dealers on its adds for the rest of the fight.
bool PlayerbotAI::IsUntankedBoss(Unit const* unit) const
{
    Creature const* creature = unit ? unit->ToCreature() : nullptr;
    if (!creature || !creature->IsAlive() || !creature->IsInCombat())
        return false;
    if (!creature->IsDungeonBoss() && !creature->isWorldBoss())
        return false;
    return !creature->GetVictim();
}

Unit* PlayerbotAI::SelectUntankedBoss()
{
    if (!_bot->IsInCombat())
        return nullptr;
    Map* map = _bot->GetMap();
    if (!map || !map->IsDungeon())
        return nullptr;

    std::list<Unit*> nearby;
    Skyfire::AnyUnfriendlyUnitInObjectRangeCheck check(_bot, _bot, 60.0f);
    Skyfire::UnitListSearcher<Skyfire::AnyUnfriendlyUnitInObjectRangeCheck> searcher(_bot, nearby, check);
    _bot->VisitNearbyObject(60.0f, searcher);
    for (Unit* unit : nearby)
    {
        if (!IsUntankedBoss(unit) || !IsSafeAttackTarget(unit) || !_bot->IsValidAttackTarget(unit))
            continue;
        // On the threat list at all: threat resets (e.g. on a boss blink) leave everyone at 0
        if (!unit->getThreatManager().getOnlineContainer().getReferenceByTarget(_bot))
            continue;
        return unit;
    }
    return nullptr;
}

bool PlayerbotAI::AvoidedMovingHazardRecently() const
{
    return _lastAvoidMoving && _lastAvoidMs && getMSTimeDiff(_lastAvoidMs, getMSTime()) < 15000;
}

Unit* PlayerbotAI::SelectAssistTankTarget()
{
    Group* group = _bot->GetGroup();
    if (!group)
        return nullptr;

    auto isTankSpec = [](Player* member) -> bool
    {
        if (!member)
            return false;
        uint32 specId = member->GetTalentSpecialization(member->GetActiveSpec());
        uint8 cls = member->getClass();
        uint32 const* specs = GetClassSpecializations(cls);
        if (!specs)
            return false;
        switch (cls)
        {
            case CLASS_WARRIOR: return specId == specs[2];
            case CLASS_PALADIN: return specId == specs[1];
            case CLASS_DEATH_KNIGHT: return specId == specs[0];
            case CLASS_DRUID: return specId == specs[2];
            case CLASS_MONK: return specId == specs[0];
            default: return false;
        }
    };

    auto usableFocus = [this](Unit* victim) -> Unit*
    {
        if (!victim || !victim->IsAlive() || !_bot->IsValidAttackTarget(victim))
            return nullptr;
        if (!_bot->IsWithinDistInMap(victim, 60.0f))
            return nullptr;
        return victim;
    };

    // Prefer the group's marked main tank when present.
    Player* preferredTank = nullptr;
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        if (!(slot.flags & uint8(GroupMemberFlags::MEMBER_FLAG_MAINTANK)))
            continue;
        if (Player* tank = ObjectAccessor::FindPlayer(slot.guid))
            if (tank->IsAlive() && tank != _bot && _bot->IsInMap(tank))
            {
                preferredTank = tank;
                break;
            }
    }

    auto focusFromTank = [&](Player* tank) -> Unit*
    {
        if (!tank)
            return nullptr;
        if (Unit* victim = usableFocus(tank->GetVictim()))
            return victim;
        if (Unit* selected = usableFocus(ObjectAccessor::GetUnit(*_bot, tank->GetTarget())))
            return selected;
        return nullptr;
    };

    if (Unit* focus = focusFromTank(preferredTank))
        return focus;

    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || !member->IsAlive() || member == _bot || !_bot->IsInMap(member))
            continue;
        if (!isTankSpec(member))
            continue;
        if (Unit* focus = focusFromTank(member))
            return focus;
    }

    return nullptr;
}

Unit* PlayerbotAI::SelectLowestHpGroupEnemy()
{
    // Alias of SelectGroupThreatTarget — kept separate so SelectTarget's priority
    // list stays readable if we later weight them differently.
    return SelectGroupThreatTarget();
}

Player* PlayerbotAI::SelectHealTarget(float belowPct)
{
    Group* group = _bot->GetGroup();
    Player* best = nullptr;
    float bestPct = belowPct; // only heal when strictly below this

    auto consider = [&](Player* member)
    {
        if (!member || !member->IsAlive() || !_bot->IsInMap(member))
            return;
        if (!_bot->IsWithinDistInMap(member, 50.0f))
            return;
        if (member->GetMaxHealth() == 0)
            return;
        float const pct = 100.0f * float(member->GetHealth()) / float(member->GetMaxHealth());
        if (pct < bestPct)
        {
            bestPct = pct;
            best = member;
        }
    };

    if (group)
    {
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            consider(itr->GetSource());
    }
    else
        consider(_bot);

    return best;
}

Unit* PlayerbotAI::SelectHealerCombatAnchor()
{
    if (!_bot)
        return nullptr;

    if (Player* hurt = SelectHealTarget())
        return hurt;

    Group* group = _bot->GetGroup();
    if (!group)
        return nullptr;

    Player* tank = nullptr;
    Player* fighting = nullptr;
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || !member->IsAlive() || member == _bot)
            continue;
        if (!_bot->IsInMap(member) || !_bot->IsWithinDistInMap(member, 60.0f))
            continue;

        uint32 const specId = member->GetTalentSpecialization(member->GetActiveSpec());
        uint8 const cls = member->getClass();
        uint32 const* specs = GetClassSpecializations(cls);
        bool isTank = false;
        if (specs)
        {
            switch (cls)
            {
                case CLASS_WARRIOR: isTank = (specId == specs[2]); break;
                case CLASS_PALADIN: isTank = (specId == specs[1]); break;
                case CLASS_DEATH_KNIGHT: isTank = (specId == specs[0]); break;
                case CLASS_DRUID: isTank = (specId == specs[2]); break;
                case CLASS_MONK: isTank = (specId == specs[0]); break;
                default: break;
            }
        }
        if (isTank && !tank)
            tank = member;
        if ((member->IsInCombat() || !member->getAttackers().empty()) && !fighting)
            fighting = member;
    }

    if (tank)
        return tank;
    if (fighting)
        return fighting;

    Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
    if (leader && leader->IsAlive() && _bot->IsInMap(leader)
        && _bot->IsWithinDistInMap(leader, 60.0f))
        return leader;
    return nullptr;
}

void PlayerbotAI::HoldRangedCombatPosition(Unit* focus, float maxRange)
{
    if (!_bot || _clientControlled || _stay || !focus || !focus->IsAlive())
        return;
    if (maxRange < 5.0f)
        maxRange = BOT_CAST_DIST;

    bool const casting = IsBusyCasting(_bot);
    if (casting)
        return;

    float const dist = _bot->GetDistance(focus);
    bool const hasLos = _bot->IsWithinLOSInMap(focus);

    // Too close — step out only when dungeon kiting is enabled.
    if (_bot->IsWithinMeleeRange(focus) || dist < maxRange * 0.40f)
    {
        if (WantsRangedKiting() && TryCombatFlee(focus))
            return;
    }

    if (dist <= maxRange && hasLos)
    {
        BotMovement::StopAndIdle(_bot);
        BotMovement::FaceUnit(_bot, focus);
        _chaseGuid = focus->GetGUID();
        _followGuid = 0;
        return;
    }

    float destX, destY, destZ;
    // Without LoS, keep closing on the focus instead of parking at max range.
    float const standDist = hasLos ? (maxRange * 0.85f) : std::max(5.0f, dist * 0.55f);
    float const absAngle = focus->GetAngle(_bot);
    focus->GetNearPoint(_bot, destX, destY, destZ, _bot->GetObjectSize(), standDist, absAngle);
    _bot->UpdateAllowedPositionZ(destX, destY, destZ);

    MovementGeneratorType moveType = _bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
    bool const reissue = _chaseGuid != focus->GetGUID()
        || moveType != POINT_MOTION_TYPE
        || _bot->IsStopped()
        || !hasLos;
    if (reissue)
    {
        MoveToPosition(destX, destY, destZ);
        _chaseGuid = focus->GetGUID();
        _followGuid = 0;
    }
}

uint32 PlayerbotAI::GetTauntSpell() const
{
    // Prefer a known, off-cooldown taunt. MoP DBC names 62124 "Reckoning"
    // (Hand of Reckoning in the spellbook).
    uint32 candidates[3] = { 0, 0, 0 };
    switch (_bot->getClass())
    {
        case CLASS_WARRIOR:
            candidates[0] = 355;      // Taunt
            break;
        case CLASS_PALADIN:
            candidates[0] = 62124;    // Reckoning / Hand of Reckoning
            candidates[1] = 31789;    // Righteous Defense (multi, needs ally)
            break;
        case CLASS_DEATH_KNIGHT:
            candidates[0] = 56222;    // Dark Command
            candidates[1] = 49576;    // Death Grip
            break;
        case CLASS_DRUID:
            candidates[0] = 6795;     // Growl
            break;
        case CLASS_MONK:
            candidates[0] = 115546;   // Provoke
            break;
        default:
            return 0;
    }

    for (uint32 id : candidates)
        if (id && _bot->HasSpell(id) && !_bot->HasSpellCooldown(id))
            return id;
    return 0;
}

uint32 PlayerbotAI::GetAoeThreatSpell() const
{
    uint32 candidates[3] = { 0, 0, 0 };
    switch (_bot->getClass())
    {
        case CLASS_WARRIOR:
            candidates[0] = 46968;    // Shockwave
            candidates[1] = 6343;     // Thunder Clap
            candidates[2] = 1680;     // Whirlwind
            break;
        case CLASS_PALADIN:
            candidates[0] = 31935;    // Avenger's Shield (ranged)
            candidates[1] = 53595;    // Hammer of the Righteous
            candidates[2] = 26573;    // Consecration
            break;
        case CLASS_DEATH_KNIGHT:
            candidates[0] = 48721;    // Blood Boil
            candidates[1] = 43265;    // Death and Decay
            break;
        case CLASS_DRUID:
            candidates[0] = 77758;    // Thrash
            candidates[1] = 106785;   // Swipe
            break;
        case CLASS_MONK:
            candidates[0] = 121253;   // Keg Smash
            candidates[1] = 115181;   // Breath of Fire
            break;
        default:
            return 0;
    }

    for (uint32 id : candidates)
        if (id && _bot->HasSpell(id) && !_bot->HasSpellCooldown(id))
            return id;
    return 0;
}

uint32 PlayerbotAI::GetHealSpell() const
{
    float lowestPct = 100.0f;
    if (Group* group = _bot->GetGroup())
    {
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsAlive() || member->GetMaxHealth() == 0)
                continue;
            float const pct = 100.0f * float(member->GetHealth()) / float(member->GetMaxHealth());
            if (pct < lowestPct)
                lowestPct = pct;
        }
    }
    else if (_bot->GetMaxHealth())
        lowestPct = 100.0f * float(_bot->GetHealth()) / float(_bot->GetMaxHealth());

    // Fallback only — prefer SelectNextHeal. No big heals above ~65%.
    bool const urgent = lowestPct < 40.0f;
    bool const big = lowestPct < 65.0f;
    bool const topOff = lowestPct < 85.0f;
    if (!topOff)
        return 0;

    switch (_bot->getClass())
    {
        case CLASS_PALADIN:
            if (urgent && BotRotation::SpellReady(_bot, 19750))
                return 19750; // Flash of Light
            if (BotRotation::SpellReady(_bot, 20473))
                return 20473; // Holy Shock
            if (big && BotRotation::SpellReady(_bot, 635))
                return 635; // Holy Light
            return 0;
        case CLASS_PRIEST:
            if (urgent && BotRotation::SpellReady(_bot, 2061))
                return 2061; // Flash Heal
            if (big && BotRotation::SpellReady(_bot, 2060))
                return 2060; // Heal / Greater Heal
            if (BotRotation::SpellReady(_bot, 139))
                return 139; // Renew
            return 0;
        case CLASS_SHAMAN:
            if (urgent && BotRotation::SpellReady(_bot, 8004))
                return 8004; // Healing Surge
            if (big && BotRotation::SpellReady(_bot, 77472))
                return 77472; // Greater Healing Wave
            if (topOff && BotRotation::SpellReady(_bot, 331))
                return 331; // Healing Wave
            return 0;
        case CLASS_DRUID:
            if (urgent && BotRotation::SpellReady(_bot, 8936))
                return 8936; // Regrowth
            if (big && BotRotation::SpellReady(_bot, 5185))
                return 5185; // Healing Touch
            if (BotRotation::SpellReady(_bot, 774))
                return 774; // Rejuvenation
            return 0;
        case CLASS_MONK:
            if (big && BotRotation::SpellReady(_bot, 116694))
                return 116694; // Surging Mist
            return BotRotation::SpellReady(_bot, 115175) ? 115175 : 0; // Soothing Mist
        default:
            return 0;
    }
}

uint32 PlayerbotAI::GetPullOpenerSpell(Unit* target) const
{
    if (!_bot || !target)
        return 0;

    float const dist = _bot->GetDistance(target);
    auto pick = [&](uint32 spellId, float minDist = 0.0f, float maxDist = 40.0f) -> uint32
    {
        if (!spellId || !_bot->HasSpell(spellId) || _bot->HasSpellCooldown(spellId))
            return 0;
        if (dist < minDist || dist > maxDist)
            return 0;
        return spellId;
    };

    switch (_bot->getClass())
    {
        case CLASS_WARRIOR:
            // Pull opener is ONE spell: Charge, else Throw. Taunt is separate (TryTankTaunt).
            if (uint32 charge = pick(100, 8.0f, 25.0f))
                return charge;
            if (uint32 leap = pick(6544, 8.0f, 40.0f))
                return leap;
            if (uint32 ht = pick(57755, 0.0f, 30.0f))
                return ht;
            return 0;
        case CLASS_PALADIN:
            if (uint32 as = pick(31935, 0.0f, 30.0f)) // Avenger's Shield
                return as;
            if (uint32 judge = pick(20271, 0.0f, 30.0f)) // Judgment
                return judge;
            break;
        case CLASS_DEATH_KNIGHT:
            if (uint32 grip = pick(49576, 0.0f, 30.0f)) // Death Grip
                return grip;
            break;
        case CLASS_DRUID:
            if (uint32 ff = pick(770, 0.0f, 35.0f)) // Faerie Fire
                return ff;
            break;
        case CLASS_MONK:
            if (uint32 wave = pick(115098, 0.0f, 40.0f)) // Chi Wave
                return wave;
            break;
        default:
            break;
    }
    // Non-warrior pull fallback only — never bundle taunt into warrior Charge/Throw.
    return GetTauntSpell();
}

float PlayerbotAI::GetPullOpenerRange(Unit* target) const
{
    if (!_bot)
        return 8.0f;

    auto known = [&](uint32 spellId) -> bool
    {
        return spellId && _bot->HasSpell(spellId);
    };

    switch (_bot->getClass())
    {
        case CLASS_WARRIOR:
            if (known(100)) // Charge
                return 20.0f;
            if (known(57755)) // Heroic Throw
                return 25.0f;
            break;
        case CLASS_PALADIN:
            if (known(31935) || known(20271))
                return 25.0f;
            break;
        case CLASS_DEATH_KNIGHT:
            if (known(49576))
                return 25.0f;
            break;
        case CLASS_DRUID:
            if (known(770))
                return 30.0f;
            break;
        case CLASS_MONK:
            if (known(115098)) // Chi Wave
                return 30.0f;
            break;
        default:
            break;
    }
    (void)target;
    return 8.0f;
}

void PlayerbotAI::BeginPullSequence(Unit* target)
{
    if (!target)
        return;
    SetForcedTarget(target);
    _pullPhase = PullPhase::Reach;
    _pullStartTime = time(nullptr);
    _combatStartTime = 0;
    _holdAssist = false;
}

void PlayerbotAI::EndPullSequence(bool keepForcedTarget)
{
    _pullPhase = PullPhase::None;
    _pullStartTime = 0;
    if (!keepForcedTarget)
        ClearForcedTarget();
}

bool PlayerbotAI::HandlePullSequence()
{
    if (_pullPhase == PullPhase::None || !_bot)
        return false;
    if (_clientControlled)
    {
        EndPullSequence(true);
        return false;
    }

    Unit* target = GetForcedTarget();
    if (!target)
        target = _targets.GetPullTarget(this);
    if (!target || !target->IsAlive() || !_bot->IsValidAttackTarget(target))
    {
        EndPullSequence(false);
        return false;
    }

    // Time out a stuck pull after 15s (AC PullStrategy max).
    if (_pullStartTime && time(nullptr) - _pullStartTime > 15)
    {
        EndPullSequence(true);
        return false;
    }

    // Once the pack is swinging, leave sequenced pull and fight normally.
    if (target->IsInCombat() && (_bot->IsInCombat() || GroupInCombat()))
    {
        EndPullSequence(true);
        return false;
    }

    float const range = GetPullOpenerRange(target);
    float const dist = _bot->GetDistance(target);
    StopResting();
    _followGuid = 0;
    _lootGuid = 0;

    if (_pullPhase == PullPhase::Reach)
    {
        if (dist <= range && _bot->IsWithinLOSInMap(target))
        {
            _pullPhase = PullPhase::Opener;
        }
        else
        {
            MovementGeneratorType moveType = _bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
            bool reissue = _chaseGuid != target->GetGUID()
                || moveType != CHASE_MOTION_TYPE
                || _bot->IsStopped();
            if (reissue)
            {
                if (BotMovement::MoveChase(_bot, target, std::max(0.0f, range - 2.0f)))
                    _chaseGuid = target->GetGUID();
            }
            return true;
        }
    }

    if (_pullPhase == PullPhase::Opener)
    {
        if (!_bot->HasInArc(static_cast<float>(M_PI), target))
            _bot->SetInFront(target);
        _bot->SetSelection(target->GetGUID());

        if (IsBusyCasting(_bot))
            return true;

        // Warriors: do not StopAndIdle before Charge (planting + Throw used to cancel it).
        // One gap-closer only; taunt is separate and off-GCD.
        if (_bot->getClass() == CLASS_WARRIOR)
        {
            bool const charged = TryWarriorGapClose(target);
            if (GetCombatRole() == CombatRole::Tank && _tankMode)
                TryTankTaunt(target);
            if (charged)
            {
                EndPullSequence(true);
                return true;
            }
            // Still closing — keep pull active until Charge/Throw lands or melee.
            if (!_bot->IsWithinMeleeRange(target))
                return true;
            EndPullSequence(true);
            return false;
        }

        if (_bot->isMoving())
            BotMovement::StopAndIdle(_bot);

        if (uint32 opener = GetPullOpenerSpell(target))
        {
            if (BotRotation::CanTryCast(_bot, opener)
                && BotRotation::CastSpell(_bot, target, opener))
            {
                EndPullSequence(true);
                return true;
            }
        }

        // No opener available — engage melee / normal combat.
        EndPullSequence(true);
        return false;
    }

    return false;
}

bool PlayerbotAI::TryAvoidAoe()
{
    if (!_bot || _clientControlled || _stay || !_avoidAoe)
        return false;

    // Ignore room-wide effects. Same cap the upstream avoid action uses.
    float constexpr kMaxRadius = 15.0f;

    std::list<WorldObject*> objects;
    float const scanRange = 40.0f;
    Skyfire::AllWorldObjectsInRange check(_bot, scanRange);
    Skyfire::WorldObjectListSearcher<Skyfire::AllWorldObjectsInRange> searcher(_bot, objects, check);
    _bot->VisitNearbyObject(scanRange, searcher);

    struct Hazard
    {
        float x, y, z;
        float radius;
        float dist;
        uint32 spellId;
    };
    Hazard best{ 0, 0, 0, 0, 0, 0 };
    bool found = false;

    auto consider = [&](float x, float y, float z, float radius, uint32 spellId)
    {
        if (radius < 0.5f || radius > kMaxRadius)
            return;
        if (std::fabs(_bot->GetPositionZ() - z) > 8.0f)
            return;
        float const dist = _bot->GetExactDist2d(x, y);
        if (dist > radius)
            return;
        if (!found || dist < best.dist)
        {
            best = { x, y, z, radius, dist, spellId };
            found = true;
        }
    };

    auto spellRadius = [](SpellInfo const* info, Unit* caster) -> float
    {
        float radius = 0.0f;
        if (!info)
            return radius;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            float const r = info->Effects[i].CalcRadius(caster);
            if (r > radius)
                radius = r;
        }
        return radius;
    };

    for (WorldObject* obj : objects)
    {
        if (!obj)
            continue;

        if (obj->GetTypeId() == TypeID::TYPEID_DYNAMICOBJECT)
        {
            DynamicObject* dyn = static_cast<DynamicObject*>(obj);
            SpellInfo const* info = sSpellMgr->GetSpellInfo(dyn->GetSpellId());
            if (!info || info->IsPositive())
                continue;

            Unit* caster = dyn->GetCaster();
            if (caster && (caster == _bot || _bot->IsFriendlyTo(caster)))
                continue;

            bool damaging = false;
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                SpellEffectInfo const& effect = info->Effects[i];
                if (!effect.IsEffect())
                    continue;
                if (effect.IsEffect(SPELL_EFFECT_SCHOOL_DAMAGE)
                    || effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE
                    || effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE_PERCENT
                    || effect.ApplyAuraName == SPELL_AURA_PERIODIC_LEECH)
                {
                    damaging = true;
                    break;
                }
            }
            if (!damaging)
                continue;

            consider(dyn->GetPositionX(), dyn->GetPositionY(), dyn->GetPositionZ(), dyn->GetRadius(), dyn->GetSpellId());
            continue;
        }

        if (obj->GetTypeId() == TypeID::TYPEID_GAMEOBJECT)
        {
            GameObject* go = obj->ToGameObject();
            GameObjectTemplate const* goInfo = go ? go->GetGOInfo() : nullptr;
            if (!go || !goInfo)
                continue;

            if (Unit* owner = go->GetOwner())
                if (owner == _bot || _bot->IsFriendlyTo(owner))
                    continue;

            if (goInfo->type == GAMEOBJECT_TYPE_TRAP && goInfo->trap.type == 0 && goInfo->trap.spellId)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(goInfo->trap.spellId);
                if (!info || info->IsPositive())
                    continue;
                float radius = float(goInfo->trap.radius);
                if (radius < 0.5f)
                    radius = spellRadius(info, nullptr);
                consider(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(), radius, goInfo->trap.spellId);
            }
            else if (goInfo->type == GAMEOBJECT_TYPE_AREADAMAGE
                && (goInfo->areadamage.damageMin || goInfo->areadamage.damageMax))
            {
                consider(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(),
                    float(goInfo->areadamage.radius), 0);
            }
            continue;
        }

        // Unselectable creature pulsing school damage (fissures, void zones). Moving ones are sweeps, handled below.
        if (obj->GetTypeId() != TypeID::TYPEID_UNIT)
            continue;

        Unit* unit = obj->ToUnit();
        if (!unit || !unit->IsAlive() || unit->GetTypeId() == TypeID::TYPEID_PLAYER || unit->isMoving())
            continue;
        if (!unit->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE))
            continue;
        if (_bot->IsFriendlyTo(unit))
            continue;
        if (!_bot->IsWithinDist(unit, kMaxRadius, false))
            continue;

        uint32 spellId = 0;
        float const radius = BotMovement::PulsedDamageRadius(unit, &spellId);
        consider(unit->GetPositionX(), unit->GetPositionY(), unit->GetPositionZ(), radius, spellId);
    }

    // Sweeping hazards (rotating fire walls) are a line of fires that outrun a bot at the far end: judge every
    // spot against all of them over the next seconds instead of against where the closest one is now.
    float constexpr kSweepHold = 1.5f;
    std::vector<BotMovement::SweepHazard> sweeps;
    BotMovement::CollectSweepHazards(_bot, scanRange, sweeps);
    float const stayExposure = sweeps.empty() ? 0.0f
        : BotMovement::SweepExposure(_bot, sweeps, _bot->GetPositionX(), _bot->GetPositionY(), kSweepHold);
    bool const moving = stayExposure > 0.0f;

    if (!found && !moving)
    {
        // Safe where we stand, but the current path may walk into a sweep: wait for it to pass
        if (!sweeps.empty() && _bot->movespline && !_bot->movespline->Finalized()
            && (!_lastAvoidMs || getMSTimeDiff(_lastAvoidMs, getMSTime()) > 1000))
        {
            G3D::Vector3 const dest = _bot->movespline->FinalDestination();
            if (BotMovement::PathCrossesSweep(_bot, dest.x, dest.y))
            {
                BotMovement::StopAndIdle(_bot);
                _chaseGuid = 0;
            }
        }
        return false;
    }

    Unit* target = _bot->GetVictim();
    if (!target || !target->IsAlive() || !_bot->IsValidAttackTarget(target))
        target = SelectTarget();
    if (target && (!target->IsAlive() || !_bot->IsValidAttackTarget(target)))
        target = nullptr;

    bool const melee = !IsRangedClass() && (GetCombatRole() != CombatRole::Healer || IsMeleeHealerDps());

    struct AngleTry
    {
        float angle;
        bool strict;
        float dist;
        bool orbit;
    };
    std::vector<AngleTry> tries;
    float fleeDis = best.radius + 1.0f;
    if (moving)
    {
        BotMovement::SweepHazard const* nearest = nullptr;
        float nearestDist = 0.0f;
        for (BotMovement::SweepHazard const& h : sweeps)
        {
            float const d = _bot->GetExactDist2d(h.x, h.y);
            if (!nearest || d < nearestDist)
            {
                nearest = &h;
                nearestDist = d;
            }
        }

        // Ahead of the sweep or already behind it
        float const along = (_bot->GetPositionX() - nearest->x) * nearest->dirX + (_bot->GetPositionY() - nearest->y) * nearest->dirY;
        float const sweepAngle = std::atan2(nearest->dirY, nearest->dirX);

        // Close to an untanked boss the sweep is usually turning around it, slowly that close in: strafe around
        // the boss ahead of it at the same range and keep attacking, instead of running off.
        if (target && IsUntankedBoss(target) && _bot->GetExactDist2d(target) <= 12.0f)
        {
            float const relX = nearest->x - target->GetPositionX();
            float const relY = nearest->y - target->GetPositionY();
            float turn = (relX * nearest->dirY - relY * nearest->dirX) >= 0.0f ? 1.0f : -1.0f;
            if (along < 0.0f)
                turn = -turn;

            float const minRange = melee ? 2.0f : 6.0f;
            float const maxRange = melee ? 4.0f : 9.0f;
            float const range = std::max(minRange, std::min(_bot->GetExactDist2d(target), maxRange));
            float const botAngle = target->GetAngle(_bot);
            for (float step : { 75.0f, 100.0f, 50.0f, 130.0f })
            {
                float const a = botAngle + turn * step * float(M_PI) / 180.0f;
                float const px = target->GetPositionX() + range * std::cos(a);
                float const py = target->GetPositionY() + range * std::sin(a);
                tries.push_back({ _bot->GetAngle(px, py), false, _bot->GetExactDist2d(px, py), true });
            }
        }

        // Outrun it, or let it pass by stepping back out of its lane
        float const aheadDis = nearest->radius + 2.0f + nearest->speed * kSweepHold;
        float const behindDis = nearest->radius * 2.0f + 2.0f;
        float const forward = along < 0.0f ? sweepAngle + float(M_PI) : sweepAngle;
        for (float offset : { 0.0f, float(M_PI) / 4.0f, -float(M_PI) / 4.0f, float(M_PI) / 2.0f, -float(M_PI) / 2.0f })
        {
            tries.push_back({ forward + offset, false, along < 0.0f ? behindDis : aheadDis, false });
            tries.push_back({ forward + float(M_PI) + offset, false, along < 0.0f ? aheadDis : behindDis, false });
        }
    }
    else
    {
        if (target)
        {
            float const angle = _bot->GetAngle(target);
            bool const tanking = target->GetVictim() == _bot;
            tries.push_back({ angle + float(M_PI) / 2.0f, false, fleeDis, false });
            tries.push_back({ angle - float(M_PI) / 2.0f, false, fleeDis, false });
            tries.push_back({ angle, true, fleeDis, false });
            if (tanking)
                tries.push_back({ angle + float(M_PI), false, fleeDis, false });
            else if (!melee)
                tries.push_back({ angle + float(M_PI), true, fleeDis, false });
        }
        tries.push_back({ _bot->GetAngle(best.x, best.y) + float(M_PI), false, fleeDis, false });
    }

    auto reversedRecently = [&](float angle)
    {
        if (!_lastAvoidMs || getMSTimeDiff(_lastAvoidMs, getMSTime()) > 5000)
            return false;
        float const cur = Position::NormalizeOrientation(angle);
        float const rev = Position::NormalizeOrientation(_lastAvoidAngle + float(M_PI));
        float diff = std::fabs(rev - cur);
        if (diff > float(M_PI))
            diff = (2.0f * float(M_PI)) - diff;
        return diff < float(M_PI) / 4.0f;
    };

    // Static hazards: further from the hazard is better. Sweeps: less time in reach, then more room.
    float bestScore = best.dist;
    float bestExposure = stayExposure;
    bool bestOrbit = false;
    bool have = false;
    float destX = 0.0f, destY = 0.0f, destZ = 0.0f, destAngle = 0.0f;

    auto attempt = [&](bool allowReverse)
    {
        for (AngleTry const& tryAngle : tries)
        {
            float const angle = Position::NormalizeOrientation(tryAngle.angle);
            if (!moving && !allowReverse && reversedRecently(angle))
                continue;

            // Stop short of walls and game objects; a destination behind one makes the bot path around it
            // (or not move at all) while it stands in the hazard
            Position pos;
            _bot->GetPosition(&pos);
            _bot->MovePositionToFirstCollision(pos, tryAngle.dist, angle - _bot->GetOrientation());
            float x = pos.GetPositionX();
            float y = pos.GetPositionY();
            float z = pos.GetPositionZ();
            _bot->UpdateAllowedPositionZ(x, y, z);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                continue;
            if (std::fabs(z - _bot->GetPositionZ()) > 6.0f)
                continue;

            float const straight = _bot->GetExactDist2d(x, y);
            if (straight < 1.0f)
                continue;

            PathGenerator path(_bot);
            if (!path.CalculatePath(x, y, z) || (path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)))
                continue;
            Movement::PointsArray const& points = path.GetPath();
            float pathLength = 0.0f;
            for (size_t i = 1; i < points.size(); ++i)
                pathLength += (points[i] - points[i - 1]).length();
            if (pathLength > straight * 1.5f + 1.0f)
                continue;

            if (tryAngle.strict && target)
            {
                float const destDist = target->GetExactDist2d(x, y);
                if (melee)
                {
                    if (_bot->IsWithinMeleeRange(target) && destDist - target->GetCombatReach() > 8.0f)
                        continue;
                }
                else if (destDist > BOT_CAST_DIST || destDist < 8.0f)
                    continue;
            }

            if (moving)
            {
                float clear = 0.0f;
                float const exposure = BotMovement::SweepExposure(_bot, sweeps, x, y, kSweepHold, &clear);
                if (exposure >= stayExposure)
                    continue;
                // Strafe spots keep the bot on the boss: take one whenever it is as safe as the best so far
                bool better = !have || exposure < bestExposure - 0.05f;
                if (!better && exposure <= bestExposure + 0.05f)
                    better = tryAngle.orbit ? (!bestOrbit || clear > bestScore) : (!bestOrbit && clear > bestScore);
                if (!better)
                    continue;
                bestExposure = exposure;
                bestScore = clear;
                bestOrbit = tryAngle.orbit;
            }
            else
            {
                float const score = std::sqrt((x - best.x) * (x - best.x) + (y - best.y) * (y - best.y));
                if (score <= bestScore)
                    continue;
                bestScore = score;
            }

            destX = x;
            destY = y;
            destZ = z;
            destAngle = angle;
            have = true;
        }
    };

    attempt(false);
    if (!have && !moving)
        attempt(true);
    if (!have)
        return false;

    if (IsBusyCasting(_bot))
        _bot->InterruptNonMeleeSpells(false);

    if (!BotMovement::MovePoint(_bot, destX, destY, destZ, true, false))
        return false;

    _lastAvoidAngle = destAngle;
    _lastAvoidMs = getMSTime();
    _lastAvoidMoving = moving;
    _chaseGuid = 0;
    uint32 const spellId = moving ? sweeps.front().spellId : best.spellId;
    if (spellId)
        DebugCombat("Avoid " + std::to_string(spellId));
    else
        DebugCombat("Avoid hazard");
    return true;
}

bool PlayerbotAI::WantsRangedKiting() const
{
    if (!_bot || _clientControlled)
        return false;

    // Emergency: low HP always allow backing off if flee is enabled.
    if (_bot->GetHealthPct() <= 30.0f)
        return true;

    // Above 30%: only hold cast range when a tank can keep the mob and we're
    // in a dungeon/instance. Open-world solo casters plant and cast instead of
    // endlessly kiting a chasing mob.
    Map* map = _bot->FindMap();
    if (!map || !map->IsDungeon())
        return false;

    Group* group = _bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || member == _bot || !member->IsAlive() || !member->IsInWorld())
            continue;
        if (member->GetMap() != map)
            continue;

        uint32 const specId = member->GetTalentSpecialization(member->GetActiveSpec());
        uint8 const cls = member->getClass();
        uint32 const* specs = GetClassSpecializations(cls);
        if (!specs)
            continue;

        bool isTank = false;
        switch (cls)
        {
            case CLASS_WARRIOR: isTank = (specId == specs[2]); break;
            case CLASS_PALADIN: isTank = (specId == specs[1]); break;
            case CLASS_DEATH_KNIGHT: isTank = (specId == specs[0]); break;
            case CLASS_DRUID: isTank = (specId == specs[2]); break;
            case CLASS_MONK: isTank = (specId == specs[0]); break;
            default: break;
        }
        if (isTank)
            return true;
    }
    return false;
}

bool PlayerbotAI::TryCombatFlee(Unit* focus)
{
    if (!_bot || _clientControlled || _stay)
        return false;
    if (!sPlayerbotMgr->IsFleeEnabled())
        return false;
    if (GetCombatRole() == CombatRole::Tank && _tankMode)
        return false;
    if (!IsRangedClass() && (GetCombatRole() != CombatRole::Healer || IsMeleeHealerDps()))
        return false;
    if (!WantsRangedKiting())
        return false;
    if (IsBusyCasting(_bot))
        return false;

    time_t const now = time(nullptr);
    if (_lastCombatFlee && now - _lastCombatFlee < 2)
        return false;

    BotFleeManager manager(_bot, sPlayerbotMgr->GetFleeDistance(), focus);
    if (!manager.IsUseful())
        return false;

    float x, y, z;
    if (!manager.CalculateDestination(x, y, z))
        return false;

    if (!BotMovement::MovePoint(_bot, x, y, z))
        return false;

    _lastCombatFlee = now;
    _chaseGuid = 0;
    return true;
}

bool PlayerbotAI::TryAutoMarkRti()
{
    if (!_bot || _clientControlled)
        return false;
    if (!_bot->IsInCombat() && !GroupInCombat())
        return false;
    if (GetCombatRole() != CombatRole::Tank || !_tankMode)
        return false;
    if (_bot->InBattleground())
        return false;

    Group* group = _bot->GetGroup();
    if (!group)
        return false;

    int32 const index = BotTargetValues::RtiIndexFromName(_targets.GetRti());
    if (index < 0)
        return false;

    uint64 const existing = group->GetTargetIcon(uint8(index));
    if (existing)
    {
        Unit* marked = ObjectAccessor::GetUnit(*_bot, existing);
        if (marked && marked->IsAlive() && _bot->IsValidAttackTarget(marked)
            && _bot->IsInMap(marked) && _bot->IsWithinDistInMap(marked, 80.0f, false))
            return false; // preferred icon already on a live hostile
    }

    Unit* best = nullptr;
    float bestHp = 2.0f;
    auto consider = [&](Unit* unit)
    {
        if (!unit || !unit->IsAlive() || unit->ToPlayer())
            return;
        if (!_bot->IsValidAttackTarget(unit) || !_bot->IsWithinDistInMap(unit, 60.0f))
            return;
        for (uint8 i = 0; i < TARGETICONCOUNT; ++i)
            if (group->GetTargetIcon(i) == unit->GetGUID())
                return; // already marked
        float const hp = unit->GetMaxHealth()
            ? float(unit->GetHealth()) / float(unit->GetMaxHealth()) : 1.0f;
        if (!best || hp < bestHp)
        {
            best = unit;
            bestHp = hp;
        }
    };

    for (Unit* attacker : _bot->getAttackers())
        consider(attacker);

    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || !member->IsAlive() || !_bot->IsInMap(member))
            continue;
        for (Unit* attacker : member->getAttackers())
            consider(attacker);
    }

    if (!best)
        return false;

    group->SetTargetIcon(uint8(index), ObjectGuid(_bot->GetGUID()), ObjectGuid(best->GetGUID()), 0);
    return true;
}

// Auto-accept trades and duels. Trades use the real accept opcode path so a
// pending mutual accept completes properly; duels mirror HandleDuelResponseOpcode.
void PlayerbotAI::HandleInteractions()
{
    // Trade: accept once the window is open. If the other side already accepted,
    // HandleAcceptTradeOpcode finishes the exchange; otherwise it just flags us
    // accepted so their next Accept completes it. Re-runs after item/money changes
    // because those clear the accepted flag on both sides.
    if (TradeData* trade = _bot->GetTradeData())
    {
        if (!trade->IsAccepted())
        {
            WorldPacket data(CMSG_ACCEPT_TRADE);
            _bot->GetSession()->HandleAcceptTradeOpcode(data);
        }
    }

    // Duel: accept a pending challenge (startTimer still 0) from someone else.
    if (_bot->duel && _bot->duel->startTime == 0 && _bot->duel->startTimer == 0 &&
        _bot->duel->initiator && _bot->duel->initiator != _bot &&
        _bot->duel->opponent)
    {
        time_t now = time(nullptr);
        _bot->duel->startTimer = now;
        _bot->duel->opponent->duel->startTimer = now;
        _bot->SendDuelCountdown(3000);
        _bot->duel->opponent->SendDuelCountdown(3000);
    }
}

// Walk to and loot nearby corpses the bot is allowed to take from. Returns true
// while the bot is busy with loot so follow/wander don't yank it away mid-run.
bool PlayerbotAI::HandleLoot()
{
    if (!_loot)
        return false;

    // Still pathing toward a corpse we already picked.
    if (_lootGuid)
    {
        Creature* corpse = _bot->GetMap()->GetCreature(_lootGuid);
        if (!corpse || corpse->IsAlive()
            || !HasTakeableLoot(_bot, corpse, &_lootBagFullSkip, &_lootRollOpened))
        {
            _lootGuid = 0;
            return false;
        }

        // Do not open while a real player is already looking at this corpse.
        Player* leader = nullptr;
        if (Group* group = _bot->GetGroup())
        {
            if (uint64 const leaderGuid = group->GetLeaderGUID())
                if (leaderGuid != _bot->GetGUID())
                    leader = ObjectAccessor::FindPlayer(leaderGuid);
        }
        if (leader && leader->GetLootGUID() == corpse->GetGUID())
            return true;

        if (!_bot->IsWithinDistInMap(corpse, INTERACTION_DISTANCE))
        {
            if (_bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
                MoveToPosition(corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ());
            return true;
        }

        // In range: open loot (starts group rolls), take free loot that fits.
        // Never StoreLootItem on roll-blocked slots — that calls SendLootRelease
        // and aborts Need/Greed.
        uint64 const corpseGuid = corpse->GetGUID();
        _bot->SendLoot(corpseGuid, LootType::LOOT_CORPSE);
        if (HasPendingLootRolls(&corpse->loot))
            _lootRollOpened.insert(corpseGuid);

        Loot* loot = &corpse->loot;
        uint32 maxSlot = loot->GetMaxSlotInLootFor(_bot);
        bool storedOrTookGold = false;
        bool sawFreeItem = false;
        bool bagFull = false;
        for (uint32 slot = 0; slot < maxSlot; ++slot)
        {
            LootItem* item = loot->LootItemInSlot(slot, _bot);
            if (!item || item->is_blocked)
                continue;
            sawFreeItem = true;
            if (!CanFitLootItem(_bot, item->itemid, item->count))
            {
                bagFull = true;
                continue;
            }
            _bot->StoreLootItem(uint8(slot), loot, corpseGuid);
            storedOrTookGold = true;
        }

        if (loot->gold)
        {
            WorldPacket money(CMSG_LOOT_MONEY);
            _bot->GetSession()->HandleLootMoneyOpcode(money);
            storedOrTookGold = true;
        }

        _bot->GetSession()->DoLootRelease(corpseGuid);

        // Full bags and nothing stored: do not keep pathing back to this corpse.
        // Rolls were opened above if needed; HandleLootRolls covers voting.
        if (bagFull && !storedOrTookGold && sawFreeItem)
            _lootBagFullSkip.insert(corpseGuid);
        else if (storedOrTookGold)
            _lootBagFullSkip.erase(corpseGuid);

        _lootGuid = 0;
        _followGuid = 0;
        return false;
    }

    Creature* corpse = FindNearbyLoot();
    if (!corpse)
        return false;

    if (_bot->IsWithinDistInMap(corpse, INTERACTION_DISTANCE))
    {
        _lootGuid = corpse->GetGUID();
        return HandleLoot(); // re-enter the in-range branch above
    }

    _lootGuid = corpse->GetGUID();
    _followGuid = 0;
    _chaseGuid = 0;
    MoveToPosition(corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ());
    return true;
}

Creature* PlayerbotAI::FindNearbyLoot()
{
    std::list<Creature*> corpses;
    BotLootCreatureCheck check(_bot, BOT_LOOT_SEEK_DIST, &_lootBagFullSkip, &_lootRollOpened);
    Skyfire::CreatureListSearcher<BotLootCreatureCheck> searcher(_bot, corpses, check);
    _bot->VisitNearbyGridObject(BOT_LOOT_SEEK_DIST, searcher);

    // Prefer corpses near the group leader so bots don't march off to distant
    // sparkles while follow keeps yanking them back.
    Player* leader = nullptr;
    if (Group* group = _bot->GetGroup())
    {
        uint64 const leaderGuid = group->GetLeaderGUID();
        if (leaderGuid && leaderGuid != _bot->GetGUID())
            leader = ObjectAccessor::FindPlayer(leaderGuid);
    }

    Creature* best = nullptr;
    float bestDist = BOT_LOOT_SEEK_DIST + 1.0f;
    for (Creature* c : corpses)
    {
        if (leader && leader->IsInWorld() && leader->GetMap() == _bot->GetMap())
        {
            if (leader->GetDistance(c) > BOT_LOOT_LEADER_RADIUS)
                continue;
        }
        float d = _bot->GetDistance(c);
        if (d < bestDist)
        {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

bool PlayerbotAI::HasNearbyLootPublic() const
{
    if (!_bot || !_loot || _clientControlled)
        return false;
    if (_bot->IsInCombat() || GroupInCombat())
        return false;
    if (_lootGuid)
        return true;
    return const_cast<PlayerbotAI*>(this)->FindNearbyLoot() != nullptr;
}

bool PlayerbotAI::HandleLootRolls()
{
    if (!_bot || _bot->GetTypeId() != TypeID::TYPEID_PLAYER || !_bot->IsInWorld())
        return false;

    Group* group = _bot->GetGroup();
    if (!group || !group->isRollLootActive())
        return false;

    bool voted = false;
    for (Roll* roll : group->GetRolls())
    {
        if (!roll || !roll->isValid())
            continue;

        auto voteItr = roll->playerVote.find(_bot->GetGUID());
        if (voteItr == roll->playerVote.end() || voteItr->second != RollType::MAX_ROLL_TYPE)
            continue;

        RollType vote = RollType::ROLL_GREED;
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(roll->itemid))
        {
            // Simple heuristic: greed on most loot; need on usable weapons/armor
            // the bot can equip (keeps LFG thresholds moving).
            if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
            {
                if (_bot->CanUseItem(proto) == EQUIP_ERR_OK)
                    vote = RollType::ROLL_NEED;
            }
        }

        // Master loot / FFA rolls still expect a vote so the timer clears.
        LootMethod const method = group->GetLootMethod();
        if (method == LootMethod::MASTER_LOOT || method == LootMethod::FREE_FOR_ALL)
            vote = RollType::ROLL_PASS;

        group->CountRollVote(_bot->GetGUID(), roll->itemGUID, vote);
        voted = true;
        break; // one vote per tick
    }
    return voted;
}

// Out of combat, keep formation on the group leader. If the leader is on
// another map, or too far to catch up on foot, the bot teleports to them.
void PlayerbotAI::HandleFollow()
{
    Group* group = _bot->GetGroup();
    uint64 leaderGuid = group ? group->GetLeaderGUID() : 0;

    Player* leader = nullptr;
    if (leaderGuid && leaderGuid != _bot->GetGUID())
        leader = ObjectAccessor::FindPlayer(leaderGuid);

    // Cancel solo wander state so a mid-walk / pause never resumes after invite.
    _wanderTimer = 0;

    // Mid-loot path: do not overwrite POINT motion with MoveFollow (that causes
    // the loot↔follow thrash when a corpse is still in seek range).
    if (_lootGuid)
        return;

    if (!leader || !leader->IsInWorld() || !leader->IsAlive())
    {
        if (_followGuid)
        {
            BotMovement::StopAndIdle(_bot);
            _followGuid = 0;
        }
        return;
    }

    // Warp to the leader when we can't reasonably run there (different map or
    // very far). Following resumes automatically once we arrive on the map.
    if (leader->GetMap() != _bot->GetMap() || _bot->GetDistance(leader) > BOT_TELEPORT_DIST)
    {
        TeleportToLeader(leader);
        return;
    }

    // Keep MoP phase sets aligned so the master can see the bot without GM mode.
    SyncPhaseWithMaster(leader);

    // Mirror the master's mount before follow moves — remount must not lose to
    // a ground MovePoint that runs first and cancels the cast.
    SyncMountWithMaster();

    // Never interrupt casts for follow adjustments (AC Follow/ChaseCastStop pitfall).
    if (BotMovement::IsCasting(_bot))
        return;

    float const dist = _bot->GetDistance(leader);
    MovementGeneratorType const moveType = _bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
    float const followDist = BotFormation::FollowDistance(this);
    float const followAngle = BotFormation::FollowAngle(this);

    // In flyable zones the client often keeps MOVEMENTFLAG_FLYING while you run
    // along the ground on a flyer. Use height-above-ground, not flags alone.
    bool const air = IsMasterAirborne(leader);

    if (air)
    {
        EnsureFlightMountCapability();
        SetBotFlyingMovement(true);

        float slotX = 0.0f, slotY = 0.0f, slotZ = 0.0f;
        if (!BotMovement::ComputeFollowPoint(leader, _bot, followDist, followAngle,
                slotX, slotY, slotZ, true))
            return;

        float const xyDist = _bot->GetDistance2d(leader);
        float const zDelta = leader->GetPositionZ() - _bot->GetPositionZ();

        // Far below / away — snap into the air near the master (follow can't climb).
        if (xyDist > BOT_TELEPORT_DIST * 0.5f || std::fabs(zDelta) > 60.0f)
        {
            _bot->NearTeleportTo(slotX, slotY, slotZ, leader->GetOrientation());
            if (_bot->GetSession() && _bot->GetSession()->IsBot())
                _bot->GetSession()->FinalizeBotTeleport();
            SetBotFlyingMovement(true);
            _followGuid = leaderGuid;
            _chaseGuid = 0;
            return;
        }

        bool const needMove = xyDist > followDist + 2.5f
            || std::fabs(zDelta) > 2.5f
            || _followGuid != leaderGuid
            || (moveType != POINT_MOTION_TYPE && !_bot->IsStopped());

        if (needMove
            && BotMovement::MovePoint(_bot, slotX, slotY, slotZ, /*generatePath=*/false))
        {
            _followGuid = leaderGuid;
            _chaseGuid = 0;
        }
        return;
    }

    // Master is ground-running (including on a flyer) — strip fly flags and use
    // the mount's ground SpeedModSpell so bots are not stuck at walk speed.
    SetBotFlyingMovement(false);
    if (_bot->IsMounted() || _bot->HasAuraType(SPELL_AURA_MOUNTED))
        EnsureGroundMountCapability();
    ClampBotToGround();

    float slotX = 0.0f, slotY = 0.0f, slotZ = 0.0f;
    bool const haveSlot = BotMovement::ComputeFollowPoint(leader, _bot, followDist, followAngle,
        slotX, slotY, slotZ, false);
    float const slotDist = haveSlot
        ? _bot->GetDistance(slotX, slotY, slotZ)
        : dist;

    // Prefer continuous MoveFollow while the master is moving — parking then
    // re-issuing short MovePoints looked like walk→run→snail crawl in LFG.
    // MovePoint only when clipped into geometry (Follow can slide into walls).
    bool const masterMoving = leader->isMoving();
    bool const badlyOff = BotMovement::IsBadlyOffGround(_bot);
    bool const outOfSlot = !haveSlot || slotDist > 3.0f || dist > followDist + 4.0f;
    bool const needFollow = badlyOff || outOfSlot || masterMoving
        || _followGuid != leaderGuid
        || (moveType != FOLLOW_MOTION_TYPE && moveType != IDLE_MOTION_TYPE);

    if (needFollow)
    {
        bool moved = false;
        if (badlyOff && haveSlot)
            moved = BotMovement::MoveToFollowSlot(_bot, leader, followDist, followAngle, false);
        if (!moved)
            moved = BotMovement::MoveFollowLeader(_bot, leader, followDist, followAngle);
        if (moved)
        {
            _followGuid = leaderGuid;
            _chaseGuid = 0;
        }
    }
    else
    {
        // Master stopped and we are in formation — park.
        BotMovement::StopAndIdle(_bot);
        BotMovement::ClearDeadSelection(_bot);
    }
}

// Solo bots pick a nearby ground point and walk there, then pause before the
// next pick. Skip while a trade/duel window is open so we don't walk away mid-UI.
void PlayerbotAI::HandleWander()
{
    if (_bot->GetTradeData() || _bot->duel)
        return;

    MovementGeneratorType moveType = _bot->GetMotionMaster()->GetCurrentMovementGeneratorType();
    if (moveType == POINT_MOTION_TYPE || moveType == CHASE_MOTION_TYPE)
        return; // still walking somewhere

    if (_wanderTimer)
        return; // resting between walks

    float angle = float(std::rand() % 360) * (TWO_PI / 360.0f);
    float dist = 4.0f + float(std::rand() % int(BOT_WANDER_RADIUS - 3.0f));
    float x = _bot->GetPositionX() + dist * std::cos(angle);
    float y = _bot->GetPositionY() + dist * std::sin(angle);
    float z = _bot->GetPositionZ();
    _bot->UpdateAllowedPositionZ(x, y, z);

    // Reject obviously bad Z (into the void / ceiling) and try again next tick.
    if (std::fabs(z - _bot->GetPositionZ()) > 12.0f)
    {
        _wanderTimer = 1000;
        return;
    }

    _followGuid = 0;
    MoveToPosition(x, y, z);
    _wanderTimer = BOT_WANDER_PAUSE_MIN + uint32(std::rand() % (BOT_WANDER_PAUSE_MAX - BOT_WANDER_PAUSE_MIN + 1));
}

void PlayerbotAI::HandleStay()
{
    // Hold still unless combat already issued a chase this tick (combat returns
    // before HandleStay). Clear follow so a later "follow" re-issues MoveFollow.
    if (_bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
    {
        _bot->GetMotionMaster()->Clear();
        _bot->GetMotionMaster()->MoveIdle();
    }
    _followGuid = 0;
    _lootGuid = 0;
}

// Opportunistic repair + sell gray/green junk at nearby vendors or hubs.
void PlayerbotAI::HandleVendor()
{
    if (!_bot || _clientControlled)
    {
        _forceVendor = false;
        _forceSellAll = false;
        return;
    }
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty())
    {
        _forceVendor = false;
        _forceSellAll = false;
        return;
    }

    bool const needRepair = NeedsRepair();
    bool const needSell = HasVendorJunk() || _forceVendor;
    if (!needRepair && !needSell)
    {
        _forceVendor = false;
        _forceSellAll = false;
        return;
    }

    Creature* npc = nullptr;
    if (needSell)
        npc = FindNearbyVendor();
    if (!npc && needRepair)
        npc = FindNearbyRepairer();
    if (!npc && needSell)
        npc = FindNearbyRepairer();

    if (!npc)
    {
        if (TravelToVendorHub(needRepair && !needSell))
            return;
        _forceVendor = false;
        _forceSellAll = false;
        return;
    }

    if (!_bot->IsWithinDistInMap(npc, INTERACTION_DISTANCE))
    {
        if (_bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != POINT_MOTION_TYPE)
        {
            _followGuid = 0;
            MoveToPosition(npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ());
        }
        return;
    }

    TryGossipForVendorOrQuest(npc, needSell, false);

    if (needSell && npc->HasFlag(UNIT_FIELD_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR))
        SellVendorJunk(npc);

    if (needRepair && npc->HasFlag(UNIT_FIELD_NPC_FLAGS, UNIT_NPC_FLAG_REPAIR))
        _bot->DurabilityRepairAll(false, 1.0f, false);

    _forceVendor = false;
    _forceSellAll = false;
}

bool PlayerbotAI::TravelToVendorHub(bool preferRepair)
{
    if (!_bot || HasTravelDestination())
        return false;

    BotVendorHub const* hub = sBotVendorHubs->FindNearest(_bot, preferRepair);
    if (!hub)
        return false;

    float const maxDist = sPlayerbotMgr->GetVendorHubMaxDistance();
    float const dx = hub->x - _bot->GetPositionX();
    float const dy = hub->y - _bot->GetPositionY();
    float const dz = hub->z - _bot->GetPositionZ();
    if ((dx * dx + dy * dy + dz * dz) > maxDist * maxDist)
        return false;

    return SetTravelDestination(hub->mapId, hub->x, hub->y, hub->z);
}

bool PlayerbotAI::NeedsVendorWorkPublic() const
{
    if (!_bot || _clientControlled)
        return false;
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty())
        return false;
    return NeedsRepair() || HasVendorJunk() || _forceVendor;
}

bool PlayerbotAI::NeedsUrgentVendorPublic() const
{
    if (!_bot || _clientControlled)
        return false;
    return NeedsRepair() || CountFreeBagSlots() < 3;
}

uint32 PlayerbotAI::CountFreeBagSlots() const
{
    if (!_bot)
        return 0;
    uint32 free = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (!_bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++free;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag* pBag = _bot->GetBagByPos(bag))
        {
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                if (!_bot->GetItemByPos(bag, slot))
                    ++free;
        }
    }
    return free;
}

bool PlayerbotAI::ShouldVendorDumpItem(Item* item) const
{
    if (!item || item->IsNotEmptyBag())
        return false;
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto || !proto->SellPrice)
        return false;
    if (_bot->GetGUID() != item->GetOwnerGUID())
        return false;
    if (_bot->GetLootGUID() == item->GetGUID())
        return false;
    if (item->HasFlag(ITEM_FIELD_DYNAMIC_FLAGS, ITEM_FLAG_REFUNDABLE))
        return false;
    if (item->IsSoulBound())
        return false;
    if (proto->Class == ITEM_CLASS_QUEST || proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1)
        return false;
    if (proto->StartQuest)
        return false;
    if (proto->Quality == ITEM_QUALITY_POOR)
        return true;
    if (!sPlayerbotMgr->GetVendorSellGreens() && !_forceSellAll)
        return false;
    if (proto->Quality < ITEM_QUALITY_NORMAL)
        return false;
    // Keep usable gear at or above bot level-ish; dump junk greens.
    if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
    {
        if (_bot->CanUseItem(proto) == EQUIP_ERR_OK && int32(proto->RequiredLevel) + 10 >= int32(_bot->getLevel()))
            return false;
    }
    return true;
}

bool PlayerbotAI::HasGrayJunk() const
{
    return HasVendorJunk();
}

bool PlayerbotAI::HasVendorJunk() const
{
    if (!_bot)
        return false;

    auto check = [this](Item* item) -> bool { return ShouldVendorDumpItem(item); };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (check(_bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
            return true;

    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag* pBag = _bot->GetBagByPos(bag))
        {
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                if (check(_bot->GetItemByPos(bag, slot)))
                    return true;
        }
    }
    return false;
}

uint32 PlayerbotAI::SellGrayJunk(Creature* vendor)
{
    return SellVendorJunk(vendor);
}

uint32 PlayerbotAI::SellVendorJunk(Creature* vendor)
{
    if (!_bot || !vendor || !vendor->HasFlag(UNIT_FIELD_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR))
        return 0;

    std::vector<Item*> toSell;
    auto collect = [&](Item* item)
    {
        if (ShouldVendorDumpItem(item))
            toSell.push_back(item);
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        collect(_bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag* pBag = _bot->GetBagByPos(bag))
        {
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                collect(_bot->GetItemByPos(bag, slot));
        }
    }

    uint32 sold = 0;
    for (Item* item : toSell)
    {
        if (!item)
            continue;
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            continue;
        uint32 const count = item->GetCount();
        uint32 const money = proto->SellPrice * count;
        _bot->ItemRemovedQuestCheck(item->GetEntry(), count);
        _bot->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
        item->RemoveFromUpdateQueueOf(_bot);
        _bot->AddItemToBuyBackSlot(item);
        _bot->ModifyMoney(money);
        ++sold;
    }
    return sold;
}

Creature* PlayerbotAI::FindNearbyRepairer()
{
    std::list<Creature*> list;
    BotRepairerCheck check(_bot, BOT_REPAIR_SEEK_DIST);
    Skyfire::CreatureListSearcher<BotRepairerCheck> searcher(_bot, list, check);
    _bot->VisitNearbyGridObject(BOT_REPAIR_SEEK_DIST, searcher);

    Creature* best = nullptr;
    float bestDist = BOT_REPAIR_SEEK_DIST + 1.0f;
    for (Creature* c : list)
    {
        float d = _bot->GetDistance(c);
        if (d < bestDist)
        {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

Creature* PlayerbotAI::FindNearbyVendor()
{
    std::list<Creature*> list;
    BotVendorCheck check(_bot, BOT_VENDOR_SEEK_DIST);
    Skyfire::CreatureListSearcher<BotVendorCheck> searcher(_bot, list, check);
    _bot->VisitNearbyGridObject(BOT_VENDOR_SEEK_DIST, searcher);

    Creature* best = nullptr;
    float bestDist = BOT_VENDOR_SEEK_DIST + 1.0f;
    for (Creature* c : list)
    {
        float d = _bot->GetDistance(c);
        if (d < bestDist)
        {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

Creature* PlayerbotAI::FindNearbyQuestgiver()
{
    std::list<Creature*> list;
    BotQuestgiverCheck check(_bot, BOT_QUEST_SEEK_DIST, this);
    Skyfire::CreatureListSearcher<BotQuestgiverCheck> searcher(_bot, list, check);
    _bot->VisitNearbyGridObject(BOT_QUEST_SEEK_DIST, searcher);

    Creature* best = nullptr;
    float bestDist = BOT_QUEST_SEEK_DIST + 1.0f;
    for (Creature* c : list)
    {
        float d = _bot->GetDistance(c);
        if (d < bestDist)
        {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

bool PlayerbotAI::IsQuestNpcIgnored(uint32 entry) const
{
    auto it = _ignoredQuestNpcEntries.find(entry);
    if (it == _ignoredQuestNpcEntries.end())
        return false;
    return getMSTime() < it->second;
}

void PlayerbotAI::IgnoreQuestNpc(uint32 entry, uint32 durationMs)
{
    if (!entry)
        return;
    uint32 const dur = durationMs ? durationMs : BOT_QUEST_NPC_IGNORE_MS;
    _ignoredQuestNpcEntries[entry] = getMSTime() + dur;
}

bool PlayerbotAI::QuestgiverHasUsefulWork(Creature const* npc) const
{
    if (!_bot || !npc)
        return false;

    uint32 const entry = npc->GetEntry();
    if (IsQuestNpcIgnored(entry))
        return false;

    QuestRelationBounds const involved = sObjectMgr->GetCreatureQuestInvolvedRelationBounds(entry);
    for (auto itr = involved.first; itr != involved.second; ++itr)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(itr->second);
        if (!quest)
            continue;
        QuestStatus const status = _bot->GetQuestStatus(itr->second);
        if (status == QUEST_STATUS_COMPLETE && _bot->CanRewardQuest(quest, false))
            return true;
    }

    QuestRelationBounds const starters = sObjectMgr->GetCreatureQuestRelationBounds(entry);
    for (auto itr = starters.first; itr != starters.second; ++itr)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(itr->second);
        if (!quest)
            continue;
        if (_bot->GetQuestStatus(itr->second) != QUEST_STATUS_NONE)
            continue;
        if (_bot->CanTakeQuest(quest, false) && _bot->CanAddQuest(quest, false))
            return true;
    }

    // QUESTGIVER flag with no relations (e.g. Verner Osgood 415) or nothing
    // this bot can use right now — do not path to them.
    return false;
}

bool PlayerbotAI::NeedsRepair() const
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = _bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        uint32 maxDur = item->GetUInt32Value(ITEM_FIELD_MAX_DURABILITY);
        if (!maxDur)
            continue;

        uint32 curDur = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        if (float(curDur) / float(maxDur) < BOT_REPAIR_THRESHOLD)
            return true;
    }
    return false;
}

uint32 PlayerbotAI::FindMountSpell(BotMountKind preferred) const
{
    if (!_bot)
        return 0;

    bool const wantFly = preferred >= BotMountKind::Flying;
    uint32 const preferSpell = sBotPreferredMounts->Get(GUID_LOPART(_bot->GetGUID()),
        wantFly ? BotPreferredMounts::Flying : BotPreferredMounts::Ground);
    if (preferSpell)
    {
        auto it = _bot->GetSpellMap().find(preferSpell);
        if (it != _bot->GetSpellMap().end())
        {
            PlayerSpell const* ps = it->second;
            if (ps && ps->state != PLAYERSPELL_REMOVED && !ps->disabled)
            {
                BotMountKind const kind = ClassifyMountSpell(sSpellMgr->GetSpellInfo(preferSpell));
                if (kind != BotMountKind::None)
                {
                    bool const isFly = kind >= BotMountKind::Flying;
                    if (preferred == BotMountKind::None || isFly == wantFly)
                        return preferSpell;
                }
            }
        }
    }

    uint32 bestId = 0;
    int bestScore = -1;
    uint32 anyId = 0;
    int anyScore = -1;

    for (auto const& pair : _bot->GetSpellMap())
    {
        PlayerSpell const* ps = pair.second;
        if (!ps || ps->state == PLAYERSPELL_REMOVED || ps->disabled)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(pair.first);
        BotMountKind const kind = ClassifyMountSpell(info);
        if (kind == BotMountKind::None)
            continue;

        int const score = int(kind) * 1000 + int(GetMountSpellSpeedBonus(info));
        if (score > anyScore)
        {
            anyScore = score;
            anyId = pair.first;
        }

        if (preferred != BotMountKind::None)
        {
            bool const isFly = kind >= BotMountKind::Flying;
            if (isFly != wantFly)
                continue;
        }

        if (score > bestScore)
        {
            bestScore = score;
            bestId = pair.first;
        }
    }

    // Prefer a correct-family mount; only fall back to "any" when none exist.
    return bestId ? bestId : ((preferred == BotMountKind::None) ? anyId : 0);
}

float PlayerbotAI::GetMountSpellSpeedBonus(SpellInfo const* info)
{
    if (!info)
        return 0.0f;

    float best = 0.0f;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (info->Effects[i].IsAura(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED)
            || info->Effects[i].IsAura(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED))
            best = std::max(best, float(info->Effects[i].BasePoints + 1));
    }

    // MoP mounts often put speed on MountCapability.SpeedModSpell, not the mount spell.
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (!info->Effects[i].IsAura(SPELL_AURA_MOUNTED))
            continue;
        uint32 const mountType = uint32(info->Effects[i].MiscValueB);
        if (!mountType)
            continue;
        MountTypeEntry const* typeEntry = sMountTypeStore.LookupEntry(mountType);
        if (!typeEntry)
            continue;
        for (uint32 c = 0; c < MAX_MOUNT_CAPABILITIES; ++c)
        {
            MountCapabilityEntry const* cap = sMountCapabilityStore.LookupEntry(typeEntry->MountCapability[c]);
            if (!cap || !cap->SpeedModSpell)
                continue;
            if (SpellInfo const* speedInfo = sSpellMgr->GetSpellInfo(cap->SpeedModSpell))
            {
                for (uint8 e = 0; e < MAX_SPELL_EFFECTS; ++e)
                {
                    if (speedInfo->Effects[e].IsAura(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED)
                        || speedInfo->Effects[e].IsAura(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED))
                        best = std::max(best, float(speedInfo->Effects[e].BasePoints + 1));
                }
            }
        }
    }
    return best;
}

bool PlayerbotAI::MountSpellCanFly(SpellInfo const* info)
{
    if (!info)
        return false;
    if (info->HasAura(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED) || info->HasAura(SPELL_AURA_FLY))
        return true;

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (!info->Effects[i].IsAura(SPELL_AURA_MOUNTED))
            continue;
        uint32 const mountType = uint32(info->Effects[i].MiscValueB);
        if (!mountType)
            continue;
        MountTypeEntry const* typeEntry = sMountTypeStore.LookupEntry(mountType);
        if (!typeEntry)
            continue;
        for (uint32 c = 0; c < MAX_MOUNT_CAPABILITIES; ++c)
        {
            MountCapabilityEntry const* cap = sMountCapabilityStore.LookupEntry(typeEntry->MountCapability[c]);
            if (MountCapabilityIsFlight(cap))
                return true;
        }
    }
    return false;
}

bool PlayerbotAI::MountCapabilityIsFlight(MountCapabilityEntry const* cap)
{
    if (!cap)
        return false;
    if (cap->SpeedModSpell)
    {
        if (SpellInfo const* speedInfo = sSpellMgr->GetSpellInfo(cap->SpeedModSpell))
        {
            if (speedInfo->HasAura(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED)
                || speedInfo->HasAura(SPELL_AURA_FLY)
                || speedInfo->HasAura(SPELL_AURA_MOD_MOUNTED_FLIGHT_SPEED_ALWAYS)
                || speedInfo->HasAura(SPELL_AURA_MOD_FLIGHT_SPEED_NOT_STACK)
                || speedInfo->HasAura(SPELL_AURA_MOD_INCREASE_FLIGHT_SPEED))
                return true;
            // Explicit ground speed aura — not flight, even if flags are mixed.
            if (speedInfo->HasAura(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED)
                || speedInfo->HasAura(SPELL_AURA_MOD_MOUNTED_SPEED_ALWAYS)
                || speedInfo->HasAura(SPELL_AURA_MOD_MOUNTED_SPEED_NOT_STACK))
                return false;
        }
    }
    // Capability flag 0x2 = usable for flying (see Unit::GetMountCapability).
    return (cap->Flags & 0x2) != 0;
}

MountCapabilityEntry const* PlayerbotAI::FindBotMountCapability(Player const* bot, uint32 mountType, bool preferFlight)
{
    if (!bot || !mountType)
        return nullptr;

    MountTypeEntry const* typeEntry = sMountTypeStore.LookupEntry(mountType);
    if (!typeEntry)
        return nullptr;

    uint32 zoneId = 0, areaId = 0;
    bot->GetZoneAndAreaId(zoneId, areaId);

    MountCapabilityEntry const* best = nullptr;
    int bestScore = -1;

    for (uint32 i = 0; i < MAX_MOUNT_CAPABILITIES; ++i)
    {
        MountCapabilityEntry const* cap = sMountCapabilityStore.LookupEntry(typeEntry->MountCapability[i]);
        if (!cap)
            continue;
        if (preferFlight && !cap->SpeedModSpell && !(cap->Flags & 0x2))
            continue;
        if (!preferFlight && !cap->SpeedModSpell)
            continue;

        // Soft gates only — bots mirror the master and often lack cold-weather /
        // license spells that GetMountCapability() would require.
        if (cap->RequiredMap != -1 && int32(bot->GetMapId()) != cap->RequiredMap)
            continue;
        if (cap->RequiredArea && cap->RequiredArea != zoneId && cap->RequiredArea != areaId)
            continue;

        bool const isFlight = MountCapabilityIsFlight(cap);
        if (preferFlight != isFlight)
            continue;

        int score = int(cap->RequiredRidingSkill) * 10;
        if (cap->SpeedModSpell)
            if (SpellInfo const* speedInfo = sSpellMgr->GetSpellInfo(cap->SpeedModSpell))
                score += int(GetMountSpellSpeedBonus(speedInfo));

        if (score > bestScore)
        {
            bestScore = score;
            best = cap;
        }
    }
    return best;
}

bool PlayerbotAI::IsMasterAirborne(Unit const* master)
{
    if (!master || !master->GetMap())
        return false;

    // Only trust height — flyable zones keep FLYING while ground-running on a flyer.
    // Search only a few yards down so bridges/roofs are not treated as mid-air.
    float const x = master->GetPositionX();
    float const y = master->GetPositionY();
    float const z = master->GetPositionZ();
    float const ground = master->GetMap()->GetHeight(master->GetPhaseMask(), x, y, z + 0.5f, true, 8.0f);
    if (ground < -50000.0f)
        return false;

    return (z - ground) > 5.0f;
}

void PlayerbotAI::ClampBotToGround()
{
    if (!_bot || !_bot->IsInWorld() || !_bot->GetMap())
        return;
    if (_bot->IsInWater())
        return;

    float const x = _bot->GetPositionX();
    float const y = _bot->GetPositionY();
    float const z = _bot->GetPositionZ();
    float const ground = _bot->GetMap()->GetHeight(_bot->GetPhaseMask(), x, y, z + 0.5f, true, 12.0f);
    if (ground < -50000.0f)
        return;

    if ((z - ground) > 0.75f && (z - ground) < 15.0f)
    {
        float const newZ = ground + 0.05f;
        _bot->Relocate(x, y, newZ);
        _bot->m_movementInfo.guid = _bot->GetGUID();
        _bot->m_movementInfo.time = getMSTime();
        _bot->m_movementInfo.pos.Relocate(x, y, newZ, _bot->GetOrientation());
        WorldPacket data(SMSG_PLAYER_MOVE);
        _bot->WriteMovementInfo(data);
        _bot->SendMessageToSet(&data, true);
    }
}

bool PlayerbotAI::EnsureFlightMountCapability()
{
    if (!_bot || (!_bot->IsMounted() && !_bot->HasAuraType(SPELL_AURA_MOUNTED)))
        return false;

    if (_bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED)
        || _bot->HasAuraType(SPELL_AURA_FLY))
        return true;

    Unit::AuraEffectList const& mounts = _bot->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
    for (AuraEffect* aurEff : mounts)
    {
        if (!aurEff)
            continue;

        MountCapabilityEntry const* flightCap =
            FindBotMountCapability(_bot, uint32(aurEff->GetMiscValueB()), /*preferFlight=*/true);
        if (!flightCap || !flightCap->SpeedModSpell)
            continue;

        // Drop the standing/ground SpeedModSpell if mount amount pointed there.
        if (MountCapabilityEntry const* oldCap = sMountCapabilityStore.LookupEntry(uint32(aurEff->GetAmount())))
        {
            if (oldCap->SpeedModSpell && oldCap->SpeedModSpell != flightCap->SpeedModSpell)
                _bot->RemoveAurasDueToSpell(oldCap->SpeedModSpell, _bot->GetGUID());
        }

        // Keep dismount cleanup aligned with the capability we actually use.
        aurEff->SetAmount(int32(flightCap->Id));

        if (!_bot->HasAura(flightCap->SpeedModSpell))
            _bot->CastSpell(_bot, flightCap->SpeedModSpell, true);

        if (_bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED)
            || _bot->HasAuraType(SPELL_AURA_FLY))
            return true;
    }
    return false;
}

bool PlayerbotAI::EnsureGroundMountCapability()
{
    if (!_bot || (!_bot->IsMounted() && !_bot->HasAuraType(SPELL_AURA_MOUNTED)))
        return false;

    // Only strip flight movement flags — do not touch SpeedModSpell auras until
    // we have a ground replacement ready (avoids aura churn / remount thrash).
    SetBotFlyingMovement(false);

    // Already have ground mount speed — just refresh run speed.
    if (_bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED)
        || _bot->HasAuraType(SPELL_AURA_MOD_MOUNTED_SPEED_ALWAYS)
        || _bot->HasAuraType(SPELL_AURA_MOD_MOUNTED_SPEED_NOT_STACK))
    {
        // Drop flight speed if both somehow present.
        if (_bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED)
            || _bot->HasAuraType(SPELL_AURA_FLY))
        {
            _bot->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
            _bot->RemoveAurasByType(SPELL_AURA_FLY);
            SetBotFlyingMovement(false);
        }
        _bot->UpdateSpeed(MOVE_RUN, true);
        return true;
    }

    Unit::AuraEffectList const& mounts = _bot->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
    for (AuraEffect* aurEff : mounts)
    {
        if (!aurEff)
            continue;

        MountCapabilityEntry const* groundCap =
            FindBotMountCapability(_bot, uint32(aurEff->GetMiscValueB()), /*preferFlight=*/false);
        if (!groundCap || !groundCap->SpeedModSpell)
            continue;

        if (MountCapabilityEntry const* oldCap = sMountCapabilityStore.LookupEntry(uint32(aurEff->GetAmount())))
        {
            if (oldCap->SpeedModSpell && oldCap->SpeedModSpell != groundCap->SpeedModSpell)
                _bot->RemoveAurasDueToSpell(oldCap->SpeedModSpell, _bot->GetGUID());
        }
        else
        {
            // No prior capability amount — clear flight speed before applying ground.
            _bot->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
            _bot->RemoveAurasByType(SPELL_AURA_FLY);
        }

        aurEff->SetAmount(int32(groundCap->Id));

        if (!_bot->HasAura(groundCap->SpeedModSpell))
            _bot->CastSpell(_bot, groundCap->SpeedModSpell, true);

        SetBotFlyingMovement(false);
        _bot->UpdateSpeed(MOVE_RUN, true);
        if (_bot->HasAura(groundCap->SpeedModSpell)
            || _bot->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED))
            return true;
    }
    return false;
}

void PlayerbotAI::SetBotFlyingMovement(bool enable)
{
    if (!_bot)
        return;

    auto broadcastMove = [this]()
    {
        // Clients only learn FLYING (mount flap / fly-idle) from a player-move update.
        // SetCanFly / SetDisableGravity opcodes alone leave them in ground-idle in the air.
        if (!_bot->IsInWorld())
            return;
        _bot->m_movementInfo.guid = _bot->GetGUID();
        _bot->m_movementInfo.time = getMSTime();
        _bot->m_movementInfo.pos.Relocate(_bot->GetPositionX(), _bot->GetPositionY(),
            _bot->GetPositionZ(), _bot->GetOrientation());
        WorldPacket data(SMSG_PLAYER_MOVE);
        _bot->WriteMovementInfo(data);
        _bot->SendMessageToSet(&data, true);
    };

    if (enable)
    {
        bool const already = _bot->HasUnitMovementFlag(MOVEMENTFLAG_CAN_FLY)
            && _bot->IsLevitating()
            && _bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING);
        if (already)
            return;

        // Same three flags AC playerbots use — nothing fancier.
        if (!_bot->HasUnitMovementFlag(MOVEMENTFLAG_CAN_FLY))
            _bot->SetCanFly(true);
        if (!_bot->IsLevitating())
            _bot->SetDisableGravity(true);
        if (!_bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING))
            _bot->AddUnitMovementFlag(MOVEMENTFLAG_FLYING);
        _bot->SetFall(false);
        _bot->UpdateSpeed(MOVE_FLIGHT, true);
        broadcastMove();
    }
    else
    {
        bool const hadFlyState = _bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING)
            || _bot->IsLevitating()
            || _bot->HasUnitMovementFlag(MOVEMENTFLAG_CAN_FLY)
            || _bot->HasUnitMovementFlag(MOVEMENTFLAG_HOVER);

        if (_bot->HasUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_ASCENDING
                | MOVEMENTFLAG_DESCENDING | MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_HOVER))
            _bot->RemoveUnitMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_ASCENDING
                | MOVEMENTFLAG_DESCENDING | MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_HOVER);
        if (_bot->IsLevitating())
            _bot->SetDisableGravity(false);
        // Always clear CAN_FLY when grounding — keeping it (because of a flight
        // SpeedModSpell aura) is what left bots hovering 2yd up on flyers.
        if (_bot->HasUnitMovementFlag(MOVEMENTFLAG_CAN_FLY))
            _bot->SetCanFly(false);

        if (hadFlyState)
            broadcastMove();
    }
}

PlayerbotAI::BotMountKind PlayerbotAI::ClassifyMountSpell(SpellInfo const* info)
{
    if (!info || !info->HasAura(SPELL_AURA_MOUNTED))
        return BotMountKind::None;

    bool const flying = MountSpellCanFly(info);
    float const speed = GetMountSpellSpeedBonus(info);

    if (flying)
        return speed >= 250.0f ? BotMountKind::SwiftFlying : BotMountKind::Flying;
    return speed >= 90.0f ? BotMountKind::SwiftGround : BotMountKind::Ground;
}

PlayerbotAI::BotMountKind PlayerbotAI::ClassifyUnitMount(Unit const* unit)
{
    if (!unit || (!unit->IsMounted() && !unit->HasAuraType(SPELL_AURA_MOUNTED)))
        return BotMountKind::None;

    // Prefer the MOUNTED spell / mount-type capabilities. Live SpeedModSpell auras
    // flip between ground and flight while grounded on a flyer — using them for
    // family checks caused mount/dismount thrash every AI tick.
    BotMountKind best = BotMountKind::None;
    Unit::AuraEffectList const& mounts = unit->GetAuraEffectsByType(SPELL_AURA_MOUNTED);
    for (AuraEffect const* aurEff : mounts)
    {
        if (!aurEff)
            continue;
        BotMountKind const kind = ClassifyMountSpell(aurEff->GetSpellInfo());
        if (int(kind) > int(best))
            best = kind;
    }
    if (best != BotMountKind::None)
        return best;

    // Fallback when the mount spell has no capability data: use live speed auras.
    float const flight = unit->GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
    if (flight > 0.0f || unit->HasAuraType(SPELL_AURA_FLY))
        return flight >= 250.0f ? BotMountKind::SwiftFlying : BotMountKind::Flying;

    float const ground = unit->GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED);
    if (ground >= 90.0f)
        return BotMountKind::SwiftGround;
    if (ground > 0.0f)
        return BotMountKind::Ground;

    return BotMountKind::Ground;
}

bool PlayerbotAI::TryMount(BotMountKind preferred)
{
    if (!_bot || _clientControlled)
        return false;
    // Stale mount flag without aura blocks every remount — force clean first.
    if (_bot->IsMounted() && !_bot->HasAuraType(SPELL_AURA_MOUNTED))
        _bot->Dismount();
    if (_bot->IsMounted() || _bot->HasAuraType(SPELL_AURA_MOUNTED))
        return false;
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty()
        || HasEngageTarget())
        return false;
    if (_bot->getLevel() < 20 || _bot->IsInWater() || BotMovement::IsCasting(_bot))
        return false;
    if (!_bot->GetMap() || !_bot->GetMap()->IsOutdoors(_bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ()))
        return false;

    if (preferred == BotMountKind::None)
        preferred = BotMountKind::SwiftGround;

    uint32 spellId = FindMountSpell(preferred);
    // If master is flying and we have no flyer yet, do not sit on a ground mount.
    if (!spellId && preferred >= BotMountKind::Flying)
        return false;
    if (!spellId)
        spellId = FindMountSpell(BotMountKind::None);
    if (!spellId)
        return false;

    SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
    if (!info)
        return false;

    // Stop so the mount cast isn't cancelled mid-path (real cast time, not instant).
    BotMovement::StopAndIdle(_bot);
    _bot->CastSpell(_bot, spellId, false);

    if (BotMovement::IsCasting(_bot) || _bot->IsMounted() || _bot->HasAuraType(SPELL_AURA_MOUNTED))
    {
        if (_bot->IsMounted() || _bot->HasAuraType(SPELL_AURA_MOUNTED))
            EnsureGroundMountCapability();
        return true;
    }
    return false;
}

void PlayerbotAI::TryDismount()
{
    if (!_bot)
        return;

    if (_bot->HasAuraType(SPELL_AURA_MOUNTED))
        _bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    if (_bot->IsMounted())
        _bot->Dismount();

    _bot->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
    _bot->RemoveAurasByType(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED);
    SetBotFlyingMovement(false);
}

void PlayerbotAI::SyncMountWithMaster()
{
    if (!_bot || _clientControlled)
        return;
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty()
        || HasEngageTarget())
    {
        TryDismount();
        return;
    }
    // Allow remount even while a short GCD is rolling; only skip mid-cast.
    if (_bot->getLevel() < 20 || _bot->IsInWater())
        return;
    if (IsBusyCasting(_bot))
        return;

    Player* leader = nullptr;
    if (Group* group = _bot->GetGroup())
    {
        uint64 const leaderGuid = group->GetLeaderGUID();
        if (leaderGuid && leaderGuid != _bot->GetGUID())
            leader = ObjectAccessor::FindPlayer(leaderGuid);
    }

    // Solo / no master: mount only while traveling.
    if (!leader || !leader->IsInWorld() || !leader->IsAlive())
    {
        if (HasTravelDestination())
            TryMount(BotMountKind::SwiftGround);
        return;
    }

    BotMountKind const masterKind = ClassifyUnitMount(leader);
    if (masterKind == BotMountKind::None)
    {
        // Master on foot — stay on foot (formation / town).
        TryDismount();
        return;
    }

    if (!_bot->GetMap()
        || !_bot->GetMap()->IsOutdoors(_bot->GetPositionX(), _bot->GetPositionY(), _bot->GetPositionZ()))
    {
        TryDismount();
        return;
    }

    BotMountKind const mine = ClassifyUnitMount(_bot);
    if (mine != BotMountKind::None)
    {
        bool const masterFly = masterKind >= BotMountKind::Flying;
        bool const myFly = mine >= BotMountKind::Flying;
        // Same mount family (flyer vs ground mount spell). Do not remount when we
        // only swapped SpeedModSpell ground↔flight while staying on the same mount.
        if (masterFly == myFly)
        {
            if (myFly && IsMasterAirborne(leader))
                EnsureFlightMountCapability();
            else
                EnsureGroundMountCapability();
            return;
        }
        // Wrong mount type — swap.
        TryDismount();
    }

    TryMount(masterKind);
}

bool PlayerbotAI::HandleQuestNpcs()
{
    if (!_bot || _clientControlled)
        return false;
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty())
        return false;
    if (HasEngageTarget())
        return false;
    if (!_bot->PlayerTalkClass)
        return false;

    // Walk toward a nearby useful questgiver first when out of interaction range.
    if (!HasTravelDestination())
    {
        if (Creature* nearQg = FindNearbyQuestgiver())
        {
            if (!_bot->IsWithinDistInMap(nearQg, INTERACTION_DISTANCE))
            {
                MoveToPosition(nearQg->GetPositionX(), nearQg->GetPositionY(), nearQg->GetPositionZ());
                return true;
            }
        }
    }

    std::list<Creature*> list;
    BotQuestgiverCheck check(_bot, BOT_QUEST_SEEK_DIST, this);
    Skyfire::CreatureListSearcher<BotQuestgiverCheck> searcher(_bot, list, check);
    _bot->VisitNearbyGridObject(BOT_QUEST_SEEK_DIST, searcher);

    for (Creature* npc : list)
    {
        if (!npc || !_bot->IsWithinDistInMap(npc, INTERACTION_DISTANCE))
            continue;

        TryGossipForVendorOrQuest(npc, false, true);

        _bot->PrepareQuestMenu(npc->GetGUID());
        QuestMenu& qm = _bot->PlayerTalkClass->GetQuestMenu();
        bool useful = false;
        for (uint8 i = 0; i < qm.GetMenuItemCount(); ++i)
        {
            QuestMenuItem const& mi = qm.GetItem(i);
            Quest const* quest = sObjectMgr->GetQuestTemplate(mi.QuestId);
            if (!quest)
                continue;

            QuestStatus const status = _bot->GetQuestStatus(mi.QuestId);
            if (status == QUEST_STATUS_COMPLETE && _bot->CanRewardQuest(quest, false))
            {
                uint32 const reward = BestRewardIndex(quest);
                if (_bot->CanRewardQuest(quest, reward, false))
                {
                    _bot->RewardQuest(quest, reward, npc, true);
                    return true;
                }
            }
            if (status == QUEST_STATUS_NONE)
            {
                if (_bot->CanTakeQuest(quest, false) && _bot->CanAddQuest(quest, false))
                {
                    _bot->AddQuest(quest, npc);
                    return true;
                }
            }
            // Menu shows something for this bot even if we couldn't act yet
            // (e.g. full log) — don't blacklist permanently this tick.
            useful = true;
        }

        // Empty or unusable questgiver (bugged flag / conditions). Skip for a while
        // so we leave and grind / find another NPC instead of pathing in place.
        if (!useful)
            IgnoreQuestNpc(npc->GetEntry());
    }
    return false;
}

uint32 PlayerbotAI::BestRewardIndex(Quest const* quest) const
{
    if (!_bot || !quest)
        return 0;
    uint32 const count = quest->GetRewChoiceItemsCount();
    if (count <= 1 || !sPlayerbotMgr->GetQuestAutoPickReward())
        return 0;

    int bestIdx = 0;
    int bestScore = -1;
    for (uint32 i = 0; i < count; ++i)
    {
        uint32 const itemId = quest->RewardChoiceItemId[i];
        if (!itemId)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
            continue;

        int score = int(proto->SellPrice);
        if (_bot->CanUseItem(proto) == EQUIP_ERR_OK)
        {
            score += int(proto->ItemLevel) * 1000;
            if (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR)
                score += 5000;
        }
        if (score > bestScore)
        {
            bestScore = score;
            bestIdx = int(i);
        }
    }
    return uint32(bestIdx);
}

bool PlayerbotAI::SelectGossipOption(Creature* npc, uint32 gossipListId)
{
    if (!_bot || !npc || !_bot->PlayerTalkClass)
        return false;
    GossipMenu& menu = _bot->PlayerTalkClass->GetGossipMenu();
    if (!menu.GetItem(gossipListId))
        return false;
    _bot->OnGossipSelect(npc, gossipListId, menu.GetMenuId());
    return true;
}

bool PlayerbotAI::TryGossipForVendorOrQuest(Creature* npc, bool wantVendor, bool wantQuest)
{
    if (!_bot || !npc || !_bot->PlayerTalkClass)
        return false;

    // Never open trainer menus — bots learn spells via InitializeBot.
    if (npc->HasFlag(UNIT_FIELD_NPC_FLAGS, UNIT_NPC_FLAG_TRAINER))
        return false;

    // Already has the needed npcflag — no gossip hop required.
    if (wantVendor && npc->HasFlag(UNIT_FIELD_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR))
        return false;
    if (wantQuest && npc->HasFlag(UNIT_FIELD_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
    {
        _bot->PrepareQuestMenu(npc->GetGUID());
        if (!_bot->PlayerTalkClass->GetQuestMenu().Empty())
            return false;
    }

    uint32 menuId = Player::GetDefaultGossipMenuForSource(npc);
    for (uint32 hop = 0; hop < 2; ++hop)
    {
        _bot->PrepareGossipMenu(npc, menuId, wantQuest);
        GossipMenu& gmenu = _bot->PlayerTalkClass->GetGossipMenu();
        if (wantQuest && !_bot->PlayerTalkClass->GetQuestMenu().Empty())
            return true;

        int pick = -1;
        for (auto const& kv : gmenu.GetMenuItems())
        {
            GossipMenuItem const& item = kv.second;
            uint32 const id = kv.first;
            // Never select trainer / dual-spec / unlearn options.
            if (item.OptionType == GOSSIP_OPTION_TRAINER
                || item.OptionType == GOSSIP_OPTION_UNLEARNTALENTS
                || item.OptionType == GOSSIP_OPTION_UNLEARNPETTALENTS
                || item.OptionType == GOSSIP_OPTION_LEARNDUALSPEC
                || item.OptionType == GOSSIP_OPTION_UNLEARN_SPEC
                || item.MenuItemIcon == GOSSIP_ICON_TRAINER)
                continue;

            if (wantVendor && (item.OptionType == GOSSIP_OPTION_VENDOR
                || item.MenuItemIcon == GOSSIP_ICON_VENDOR))
            {
                pick = int(id);
                break;
            }
            if (wantQuest && item.OptionType == GOSSIP_OPTION_QUESTGIVER)
            {
                pick = int(id);
                break;
            }
            if (pick < 0 && item.OptionType == GOSSIP_OPTION_GOSSIP
                && item.MenuItemIcon != GOSSIP_ICON_TRAINER)
                pick = int(id);
        }
        if (pick < 0)
            return false;

        uint32 const beforeMenu = gmenu.GetMenuId();
        SelectGossipOption(npc, uint32(pick));
        menuId = _bot->PlayerTalkClass->GetGossipMenu().GetMenuId();
        if (menuId == beforeMenu && hop == 0)
            break;
    }
    return true;
}

bool PlayerbotAI::TravelToNearbyQuestgiver()
{
    Creature* npc = FindNearbyQuestgiver();
    if (!npc)
        return false;
    if (_bot->IsWithinDistInMap(npc, INTERACTION_DISTANCE))
        return false;
    MoveToPosition(npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ());
    return true;
}

bool PlayerbotAI::TravelToQuestObjective()
{
    if (!_bot || HasTravelDestination())
        return false;

    // Objective already in combat/hunt range — let SelectTarget pull it.
    if (SelectQuestObjectiveTarget())
        return false;

    for (auto const& qs : _bot->getQuestStatusMap())
    {
        if (qs.second.Status != QUEST_STATUS_INCOMPLETE)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(qs.first);
        if (!quest)
            continue;

        uint16 const slot = _bot->FindQuestSlot(qs.first);
        if (slot >= MAX_QUEST_LOG_SIZE)
            continue;

        for (QuestObjective const* obj : quest->m_questObjectives)
        {
            if (!obj)
                continue;
            if (obj->Type != QUEST_OBJECTIVE_TYPE_NPC && obj->Type != QUEST_OBJECTIVE_TYPE_NPC_INTERACT
                && obj->Type != QUEST_OBJECTIVE_TYPE_GO)
                continue;
            uint16 const have = _bot->GetQuestSlotCounter(slot, obj->Index);
            if (have >= uint16(obj->Amount > 0 ? obj->Amount : 1))
                continue;

            // Nearest same-map spawn for this objective entry.
            QueryResult result = WorldDatabase.PQuery(
                "SELECT position_x, position_y, position_z FROM creature WHERE id = %u AND map = %u LIMIT 8",
                obj->ObjectId, _bot->GetMapId());
            if (!result && obj->Type == QUEST_OBJECTIVE_TYPE_GO)
                result = WorldDatabase.PQuery(
                    "SELECT position_x, position_y, position_z FROM gameobject WHERE id = %u AND map = %u LIMIT 8",
                    obj->ObjectId, _bot->GetMapId());
            if (!result)
                continue;

            float bestX = 0, bestY = 0, bestZ = 0;
            float bestD2 = -1.0f;
            do
            {
                Field* f = result->Fetch();
                float const x = f[0].GetFloat();
                float const y = f[1].GetFloat();
                float const z = f[2].GetFloat();
                float const dx = x - _bot->GetPositionX();
                float const dy = y - _bot->GetPositionY();
                float const dz = z - _bot->GetPositionZ();
                float const d2 = dx * dx + dy * dy + dz * dz;
                if (bestD2 < 0.0f || d2 < bestD2)
                {
                    bestD2 = d2;
                    bestX = x;
                    bestY = y;
                    bestZ = z;
                }
            } while (result->NextRow());

            if (bestD2 >= 0.0f)
            {
                // Already in the hunt area — let combat/wander search locally.
                if (bestD2 <= BOT_QUEST_HUNT_RADIUS * BOT_QUEST_HUNT_RADIUS)
                    return false;
                return SetTravelDestination(_bot->GetMapId(), bestX, bestY, bestZ);
            }
        }
    }
    return false;
}

bool PlayerbotAI::HandleAutoQuesting()
{
    if (!_bot || _clientControlled)
    {
        _forceQuest = false;
        return false;
    }
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty())
    {
        _forceQuest = false;
        return false;
    }
    if (HasEngageTarget())
        return false;

    // Do not path to quests/vendors while drinking — MovePoint stands us up.
    if (NeedsRestPublic() || _resting || HasFoodOrDrinkAura() || _bot->IsSitState())
        return false;

    // Bag pressure → vendor hub before questing.
    if (CountFreeBagSlots() < 3)
    {
        if (!FindNearbyVendor() && TravelToVendorHub(false))
            return true;
        HandleVendor();
        return true;
    }

    if (HasTravelDestination())
        return false;

    if (HandleQuestNpcs())
    {
        _forceQuest = false;
        return true;
    }

    if (_quests || _forceQuest)
    {
        if (TravelToQuestObjective())
            return true;
        if (TravelToNearbyQuestgiver())
            return true;
    }

    _forceQuest = false;
    return false;
}

void PlayerbotAI::MoveToPosition(float x, float y, float z)
{
    if (BotMovement::PathCrossesClosedDoor(_bot, x, y, z))
        return;
    if (BotMovement::PathCrossesSweep(_bot, x, y))
        return;
    _bot->GetMotionMaster()->Clear();
    _bot->GetMotionMaster()->MovePoint(1, x, y, z, true);
}

void PlayerbotAI::ReplyTo(Player* from, std::string const& text)
{
    if (!from || !_bot)
        return;

    // Self-bot whispering to yourself re-enters OnChat. If the reply looks like
    // a co/nc command (e.g. "co +healer dps"), that recurses until stack overflow.
    if (from->GetGUID() == _bot->GetGUID())
    {
        if (WorldSession* session = from->GetSession())
            ChatHandler(session).SendSysMessage(text.c_str());
        return;
    }

    _bot->Whisper(text, Language::LANG_UNIVERSAL, from->GetGUID());
}

void PlayerbotAI::ReportClassSpells(Player* from)
{
    if (!from || !_bot)
        return;

    uint8 const cls = _bot->getClass();
    uint32 family = SPELLFAMILY_GENERIC;
    switch (cls)
    {
        case CLASS_WARRIOR:      family = SPELLFAMILY_WARRIOR; break;
        case CLASS_PALADIN:      family = SPELLFAMILY_PALADIN; break;
        case CLASS_HUNTER:       family = SPELLFAMILY_HUNTER; break;
        case CLASS_ROGUE:        family = SPELLFAMILY_ROGUE; break;
        case CLASS_PRIEST:       family = SPELLFAMILY_PRIEST; break;
        case CLASS_DEATH_KNIGHT: family = SPELLFAMILY_DEATHKNIGHT; break;
        case CLASS_SHAMAN:       family = SPELLFAMILY_SHAMAN; break;
        case CLASS_MAGE:         family = SPELLFAMILY_MAGE; break;
        case CLASS_WARLOCK:      family = SPELLFAMILY_WARLOCK; break;
        case CLASS_MONK:         family = SPELLFAMILY_MONK; break;
        case CLASS_DRUID:        family = SPELLFAMILY_DRUID; break;
        default: break;
    }

    uint32 const specId = _bot->GetTalentSpecialization(_bot->GetActiveSpec());
    std::unordered_set<uint32> specSpellIds;
    if (specId)
        if (std::vector<uint32> const* specSpells = GetSpecializationSpells(specId))
            for (uint32 id : *specSpells)
                specSpellIds.insert(id);

    struct Entry { uint32 id; std::string name; bool passive; };
    std::vector<Entry> known;
    known.reserve(64);

    for (auto const& pair : _bot->GetSpellMap())
    {
        uint32 const spellId = pair.first;
        PlayerSpell const* ps = pair.second;
        if (!ps || ps->state == PLAYERSPELL_REMOVED || ps->disabled || !ps->active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !info->SpellName)
            continue;

        // Class family spells, or explicit specialization grants (Shadowform, etc.).
        bool const classFamily = (info->SpellFamilyName == family);
        bool const fromSpec = specSpellIds.count(spellId) != 0;
        if (!classFamily && !fromSpec)
            continue;

        // Skip talent ranks — too noisy; init applies those separately.
        if (GetTalentSpellCost(spellId) > 0)
            continue;

        known.push_back({ spellId, info->SpellName, info->IsPassive() });
    }

    std::sort(known.begin(), known.end(),
        [](Entry const& a, Entry const& b) { return a.id < b.id; });

    std::ostringstream header;
    char const* classLabel = "?";
    switch (cls)
    {
        case CLASS_WARRIOR: classLabel = "Warrior"; break;
        case CLASS_PALADIN: classLabel = "Paladin"; break;
        case CLASS_HUNTER: classLabel = "Hunter"; break;
        case CLASS_ROGUE: classLabel = "Rogue"; break;
        case CLASS_PRIEST: classLabel = "Priest"; break;
        case CLASS_DEATH_KNIGHT: classLabel = "DK"; break;
        case CLASS_SHAMAN: classLabel = "Shaman"; break;
        case CLASS_MAGE: classLabel = "Mage"; break;
        case CLASS_WARLOCK: classLabel = "Warlock"; break;
        case CLASS_MONK: classLabel = "Monk"; break;
        case CLASS_DRUID: classLabel = "Druid"; break;
        default: break;
    }
    char const* specLabel = "?";
    switch (specId)
    {
        case SPEC_PRIEST_DISCIPLINE: specLabel = "Discipline"; break;
        case SPEC_PRIEST_HOLY: specLabel = "Holy"; break;
        case SPEC_PRIEST_SHADOW: specLabel = "Shadow"; break;
        case SPEC_MAGE_ARCANE: specLabel = "Arcane"; break;
        case SPEC_MAGE_FIRE: specLabel = "Fire"; break;
        case SPEC_MAGE_FROST: specLabel = "Frost"; break;
        case SPEC_WARLOCK_AFFLICTION: specLabel = "Affliction"; break;
        case SPEC_WARLOCK_DEMONOLOGY: specLabel = "Demonology"; break;
        case SPEC_WARLOCK_DESTRUCTION: specLabel = "Destruction"; break;
        case SPEC_DRUID_BALANCE: specLabel = "Balance"; break;
        case SPEC_DRUID_FERAL: specLabel = "Feral"; break;
        case SPEC_DRUID_GUARDIAN: specLabel = "Guardian"; break;
        case SPEC_DRUID_RESTORATION: specLabel = "Restoration"; break;
        case SPEC_WARRIOR_ARMS: specLabel = "Arms"; break;
        case SPEC_WARRIOR_FURY: specLabel = "Fury"; break;
        case SPEC_WARRIOR_PROTECTION: specLabel = "Protection"; break;
        case SPEC_PALADIN_HOLY: specLabel = "Holy"; break;
        case SPEC_PALADIN_PROTECTION: specLabel = "Protection"; break;
        case SPEC_PALADIN_RETRIBUTION: specLabel = "Retribution"; break;
        case SPEC_HUNTER_BEAST_MASTERY: specLabel = "BM"; break;
        case SPEC_HUNTER_MARKSMANSHIP: specLabel = "MM"; break;
        case SPEC_HUNTER_SURVIVAL: specLabel = "Survival"; break;
        case SPEC_ROGUE_ASSASSINATION: specLabel = "Assassination"; break;
        case SPEC_ROGUE_COMBAT: specLabel = "Combat"; break;
        case SPEC_ROGUE_SUBTLETY: specLabel = "Subtlety"; break;
        case SPEC_SHAMAN_ELEMENTAL: specLabel = "Elemental"; break;
        case SPEC_SHAMAN_ENHANCEMENT: specLabel = "Enhancement"; break;
        case SPEC_SHAMAN_RESTORATION: specLabel = "Restoration"; break;
        case SPEC_DEATH_KNIGHT_BLOOD: specLabel = "Blood"; break;
        case SPEC_DEATH_KNIGHT_FROST: specLabel = "Frost"; break;
        case SPEC_DEATH_KNIGHT_UNHOLY: specLabel = "Unholy"; break;
        case SPEC_MONK_BREWMASTER: specLabel = "Brewmaster"; break;
        case SPEC_MONK_MISTWEAVER: specLabel = "Mistweaver"; break;
        case SPEC_MONK_WINDWALKER: specLabel = "Windwalker"; break;
        default:
            if (specId)
            {
                static char specBuf[16];
                snprintf(specBuf, sizeof(specBuf), "spec%u", specId);
                specLabel = specBuf;
            }
            else
                specLabel = "none";
            break;
    }
    header << _bot->GetName() << " L" << uint32(_bot->getLevel()) << ' '
           << classLabel << ' ' << specLabel
           << " — " << known.size() << " class/spec spells:";
    ReplyTo(from, header.str());

    if (known.empty())
    {
        ReplyTo(from, "(none known — re-run .playerbots init)");
        return;
    }

    // Whisper length limit ~255; keep chunks under 220.
    std::string line;
    auto flush = [&]()
    {
        if (!line.empty())
        {
            ReplyTo(from, line);
            line.clear();
        }
    };

    for (Entry const& e : known)
    {
        std::ostringstream piece;
        piece << e.id << ':' << e.name;
        if (e.passive)
            piece << '*';
        std::string const part = piece.str();
        if (!line.empty() && line.size() + 2 + part.size() > 220)
            flush();
        if (!line.empty())
            line += ", ";
        line += part;
    }
    flush();
}

float PlayerbotAI::HealthPct() const
{
    if (!_bot || !_bot->GetMaxHealth())
        return 100.0f;
    return 100.0f * float(_bot->GetHealth()) / float(_bot->GetMaxHealth());
}

float PlayerbotAI::ManaPct() const
{
    if (!_bot || !UsesMana() || !_bot->GetMaxPower(POWER_MANA))
        return 100.0f;
    return 100.0f * float(_bot->GetPower(POWER_MANA)) / float(_bot->GetMaxPower(POWER_MANA));
}

bool PlayerbotAI::UsesMana() const
{
    return _bot && _bot->GetMaxPower(POWER_MANA) > 0;
}

bool PlayerbotAI::GroupInCombat() const
{
    if (!_bot)
        return false;
    if (_bot->IsInCombat())
        return true;
    Group* group = _bot->GetGroup();
    if (!group)
        return false;
    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (member && member->IsAlive() && member->IsInCombat() && _bot->IsInMap(member))
            return true;
    }
    return false;
}

void PlayerbotAI::StopResting()
{
    _forceRest = false;
    _resting = false;
    if (!_bot)
        return;
    // Drop food/drink before standing — leftover OBS_MOD_POWER refreshment ticks
    // were spiking displayed mana toward 100% while already in combat.
    CancelRestConsumables();
    if (_bot->getStandState() != UNIT_STAND_STATE_STAND)
        _bot->SetStandState(UNIT_STAND_STATE_STAND);
}

bool PlayerbotAI::HasFoodOrDrinkAura() const
{
    return MemberHasFoodOrDrinkAura(_bot);
}

bool PlayerbotAI::MemberHasFoodOrDrinkAura(Player const* player)
{
    if (!player)
        return false;

    // Explicit refreshment aura IDs (do not require NOT_SEATED — some MoP
    // wrappers omit that flag and we must not re-cast every AI tick).
    if (player->HasAura(BOT_REFRESHMENT_SPELL)
        || HasAnyRestAura(player, BotFoodTiers)
        || HasAnyRestAura(player, BotDrinkTiers))
        return true;

    Unit::AuraApplicationMap const& auras = player->GetAppliedAuras();
    for (Unit::AuraApplicationMap::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
    {
        AuraApplication const* app = itr->second;
        if (!app || !app->GetBase())
            continue;
        SpellInfo const* info = app->GetBase()->GetSpellInfo();
        if (!info)
            continue;
        if (!(info->AuraInterruptFlags & AURA_INTERRUPT_FLAG_NOT_SEATED))
            continue;
        if (info->HasAura(SPELL_AURA_OBS_MOD_HEALTH)
            || info->HasAura(SPELL_AURA_MOD_POWER_REGEN)
            || info->HasAura(SPELL_AURA_MOD_POWER_REGEN_PERCENT)
            || info->HasAura(SPELL_AURA_PERIODIC_ENERGIZE)
            || info->HasAura(SPELL_AURA_MOD_INCREASE_ENERGY_PERCENT)
            || info->HasAura(SPELL_AURA_OBS_MOD_POWER))
            return true;
    }
    return false;
}

void PlayerbotAI::CancelRestConsumables()
{
    if (!_bot)
        return;
    // Only interrupt while seated (food/drink cast). StopResting() runs every
    // combat tick — interrupting here was canceling Lightning Bolt / heals after
    // ~1s and also canceling the player's own casts in self-bot mode.
    if (_bot->IsSitState() && _bot->IsNonMeleeSpellCasted(false))
        _bot->InterruptNonMeleeSpells(false);
    _bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_NOT_SEATED);
    _bot->RemoveAurasDueToSpell(BOT_REFRESHMENT_SPELL);
    RemoveRestAuras(_bot, BotFoodTiers);
    RemoveRestAuras(_bot, BotDrinkTiers);
}

bool PlayerbotAI::CastRefreshmentSpell()
{
    if (!_bot || _bot->IsInCombat())
        return false;
    // Already eating/drinking — never re-cast (re-casts stacked regen auras and
    // dumped mana to 100% in one pulse, then HandleRest stood us up).
    if (_bot->IsNonMeleeSpellCasted(false) || HasFoodOrDrinkAura())
        return true;

    bool const needHp = HealthPct() < 98.0f;
    bool const needMana = UsesMana() && ManaPct() < 98.0f;
    if (!needHp && !needMana)
        return false;

    // Sit only when we are about to apply regen auras.
    if (_bot->getStandState() != UNIT_STAND_STATE_SIT)
        _bot->SetStandState(UNIT_STAND_STATE_SIT);

    // Apply Food / Drink auras directly. CastSpell(false) often fails without an
    // item cast context, so self-bot / socket bots sat but never gained the aura.
    // Food and drink match the bot's level (see BotFoodTiers / BotDrinkTiers).
    uint8 const level = _bot->getLevel();
    uint32 const foodSpell = RestSpellForLevel(BotFoodTiers, level);
    uint32 const drinkSpell = RestSpellForLevel(BotDrinkTiers, level);
    if (needHp && sSpellMgr->GetSpellInfo(foodSpell))
        _bot->AddAura(foodSpell, _bot);
    if (needMana && sSpellMgr->GetSpellInfo(drinkSpell))
        _bot->AddAura(drinkSpell, _bot);

    if (HasFoodOrDrinkAura())
        return true;

    if (_bot->getStandState() != UNIT_STAND_STATE_STAND)
        _bot->SetStandState(UNIT_STAND_STATE_STAND);
    return false;
}

void PlayerbotAI::ApplyDirectRestRegen()
{
    // Disabled: SetHealth/SetPower every AI tick fights food/drink aura ticks and
    // causes the mana bar to flash 100% ↔ real value (self-bot and socket bots).
    // Recovery is spell-based only (CastRefreshmentSpell / 128701).
}

bool PlayerbotAI::PartyNeedsRest() const
{
    if (!_bot || !_food)
        return false;

    auto memberNeeds = [&](Player* member) -> bool
    {
        if (!member || !member->IsAlive() || member->IsInCombat())
            return false;
        float const hp = member->GetMaxHealth()
            ? (100.0f * float(member->GetHealth()) / float(member->GetMaxHealth())) : 100.0f;
        if (hp < sPlayerbotMgr->GetRestHealthPct())
            return true;
        if (member->GetMaxPower(POWER_MANA) > 0)
        {
            float const mana = 100.0f * float(member->GetPower(POWER_MANA))
                / float(member->GetMaxPower(POWER_MANA));
            if (mana < sPlayerbotMgr->GetRestManaPct())
                return true;
        }
        return false;
    };

    if (memberNeeds(_bot))
        return true;

    Group* group = _bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (member && member != _bot && memberNeeds(member))
            return true;
    }
    return false;
}

bool PlayerbotAI::PartyNotAlmostReady() const
{
    if (!_bot || !_food)
        return false;
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty())
        return false;

    float const almostHp = sPlayerbotMgr->GetAlmostFullHealthPct();
    float const mediumMana = sPlayerbotMgr->GetMediumManaPct();

    auto memberNotReady = [&](Player* member) -> bool
    {
        if (!member || !member->IsAlive() || member->IsInCombat())
            return false;
        if (!_bot->IsInMap(member) || !_bot->IsWithinDistInMap(member, 50.0f))
            return false;
        // Aura alone is not "not ready" — bots stand at ~98% even if the buff
        // still has duration. Only wait on allies who still need resources.
        if (MemberHasFoodOrDrinkAura(member))
        {
            float const hp = member->GetMaxHealth()
                ? (100.0f * float(member->GetHealth()) / float(member->GetMaxHealth())) : 100.0f;
            if (hp < almostHp)
                return true;
            if (member->GetMaxPower(POWER_MANA) > 0)
            {
                float const mana = 100.0f * float(member->GetPower(POWER_MANA))
                    / float(member->GetMaxPower(POWER_MANA));
                if (mana < mediumMana)
                    return true;
            }
            return false;
        }

        float const hp = member->GetMaxHealth()
            ? (100.0f * float(member->GetHealth()) / float(member->GetMaxHealth())) : 100.0f;
        if (hp < almostHp)
            return true;
        if (member->GetMaxPower(POWER_MANA) > 0)
        {
            float const mana = 100.0f * float(member->GetPower(POWER_MANA))
                / float(member->GetMaxPower(POWER_MANA));
            if (mana < mediumMana)
                return true;
        }
        return false;
    };

    if (memberNotReady(_bot))
        return true;

    Group* group = _bot->GetGroup();
    if (!group)
        return false;

    for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (member && member != _bot && memberNotReady(member))
            return true;
    }
    return false;
}

bool PlayerbotAI::IsMasterWaitingForRest() const
{
    if (!_bot)
        return false;

    Group* group = _bot->GetGroup();
    if (!group)
        return true; // solo — drink whenever low

    uint64 const leaderGuid = group->GetLeaderGUID();
    if (!leaderGuid || leaderGuid == _bot->GetGUID())
        return true; // we are the leader

    Player* leader = ObjectAccessor::FindPlayer(leaderGuid);
    if (!leader || !leader->IsInWorld() || !leader->IsAlive())
        return true;
    if (!_bot->IsInMap(leader))
        return false;

    // Master is traveling — keep following; drink at the next stop.
    if (leader->isMoving())
        return false;

    // Reach the formation slot first. Sitting at ~35 yd caused stop/go thrash
    // while still pathing into the follow point.
    float const followDist = BotFormation::FollowDistance(const_cast<PlayerbotAI*>(this));
    float const followAngle = BotFormation::FollowAngle(const_cast<PlayerbotAI*>(this));
    float slotX = 0.0f, slotY = 0.0f, slotZ = 0.0f;
    float slotDist = _bot->GetDistance(leader);
    if (BotMovement::ComputeFollowPoint(leader, _bot, followDist, followAngle, slotX, slotY, slotZ))
        slotDist = _bot->GetDistance(slotX, slotY, slotZ);

    constexpr float SLOT_READY_DIST = 3.0f;
    if (slotDist > SLOT_READY_DIST)
        return false;

    // Still sprinting into the slot — finish the move, then drink.
    if (!_bot->IsStopped() && slotDist > 1.25f)
        return false;

    return true;
}

bool PlayerbotAI::StartRefreshment()
{
    if (!_bot)
        return false;

    TryDismount();

    if (!_clientControlled)
    {
        _bot->GetMotionMaster()->Clear();
        _bot->GetMotionMaster()->MoveIdle();
        _followGuid = 0;
        _chaseGuid = 0;
        if (_bot->GetVictim())
            _bot->AttackStop();
    }

    // Already eating/drinking — keep seated for the aura.
    if (_bot->IsNonMeleeSpellCasted(false) || HasFoodOrDrinkAura())
    {
        if (_bot->getStandState() != UNIT_STAND_STATE_SIT)
            _bot->SetStandState(UNIT_STAND_STATE_SIT);
        _resting = true;
        return true;
    }

    if (!CastRefreshmentSpell())
    {
        // Never leave an empty sit (no food/drink aura).
        _resting = false;
        if (_bot->getStandState() != UNIT_STAND_STATE_STAND)
            _bot->SetStandState(UNIT_STAND_STATE_STAND);
        return false;
    }

    _resting = true;
    return true;
}

bool PlayerbotAI::HandleRest()
{
    if (!_bot)
        return false;
    // Never sit/drink while anyone in the party is fighting (self-bot healers
    // were parking mid-pull because only self-combat was checked).
    if (_bot->IsInCombat() || GroupInCombat() || !_bot->getAttackers().empty())
    {
        CancelRestConsumables();
        StopResting();
        return false;
    }

    // Not at the follow slot yet (or master is moving) — let Follow run.
    // Do not cancel an in-progress drink for brief master fidgets; only stand
    // up when we are not already regenerating — or when already topped up.
    if (!_forceRest && !IsMasterWaitingForRest())
    {
        float const earlyHp = HealthPct();
        float const earlyMana = ManaPct();
        bool const earlyFull = earlyHp >= 98.0f && (!UsesMana() || earlyMana >= 98.0f);
        if (earlyFull)
        {
            CancelRestConsumables();
            StopResting();
            return false;
        }
        if (HasFoodOrDrinkAura() || (_bot->IsSitState() && _bot->IsNonMeleeSpellCasted(false)))
            return StartRefreshment(); // finish the glass
        if (_resting || _bot->IsSitState())
            StopResting();
        return false;
    }

    // Safety: stand if seated with no regen (empty sit contagion leftover).
    if (_bot->IsSitState() && !HasFoodOrDrinkAura() && !_bot->IsNonMeleeSpellCasted(false)
        && !_forceRest)
        _bot->SetStandState(UNIT_STAND_STATE_STAND);

    float const hpPct = HealthPct();
    float const manaPct = ManaPct();
    bool const needHp = hpPct < sPlayerbotMgr->GetRestHealthPct();
    bool const needMana = UsesMana() && manaPct < sPlayerbotMgr->GetRestManaPct();
    bool const lowResources = needHp || needMana;
    bool const nearlyFull = hpPct >= 98.0f && (!UsesMana() || manaPct >= 98.0f);
    bool const itemResting = HasFoodOrDrinkAura() || _bot->IsNonMeleeSpellCasted(false)
        || _bot->HasAura(BOT_REFRESHMENT_SPELL);

    bool const shouldRest = _forceRest || _resting || (_food && lowResources) || itemResting;
    if (!shouldRest)
        return false;

    // Fully topped up — stand immediately even if the food/drink aura still has
    // duration left (previously waited for the aura to expire).
    if (nearlyFull && !_forceRest)
    {
        CancelRestConsumables();
        StopResting();
        return false;
    }

    if (lowResources || itemResting || _forceRest)
        return StartRefreshment();

    _resting = false;
    return false;
}

std::string PlayerbotAI::FormatStrategies(bool combat) const
{
    return _strategies.Format(combat ? BotState::Combat : BotState::NonCombat);
}

bool PlayerbotAI::StrategyAllowed(bool combat, std::string const& name) const
{
    // Kept for callers; engine enforces the same gates.
    CombatRole const role = GetCombatRole();
    std::string n = name;
    if (n == "tankassist") n = "tank assist";
    if (n == "healdps" || n == "heal dps") n = "healer dps";
    if (n == "savemana") n = "save mana";
    if (n == "dpsassist") n = "dps assist";

    if (n == "passive" || n == "grind")
        return true;
    if (!combat)
        return n == "food" || n == "follow" || n == "stay" || n == "loot" || n == "heal";
    if (n == "aoe" || n == "boost" || n == "cc" || n == "avoid aoe" || n == "debug")
        return true;
    if (role == CombatRole::Tank)
        return n == "tank" || n == "tank assist" || n == "dps";
    if (role == CombatRole::Healer)
        return n == "heal" || n == "healer dps" || n == "save mana" || n == "wait for attack";
    return n == "dps" || n == "dps assist" || n == "threat" || n == "wait for attack" || n == "offheal";
}

bool PlayerbotAI::HandleStrategyCommand(Player* from, std::string const& cmd, bool /*acknowledge*/)
{
    bool combat = false;
    std::string body;
    if (cmd == "co" || cmd == "co?" || cmd == "co ?")
    {
        combat = true;
        body = "?";
    }
    else if (cmd == "nc" || cmd == "nc?" || cmd == "nc ?")
    {
        combat = false;
        body = "?";
    }
    else if (cmd.rfind("co ", 0) == 0)
    {
        combat = true;
        body = cmd.substr(3);
    }
    else if (cmd.rfind("nc ", 0) == 0)
    {
        combat = false;
        body = cmd.substr(3);
    }
    else
        return false;

    while (!body.empty() && body.front() == ' ')
        body.erase(body.begin());
    while (!body.empty() && body.back() == ' ')
        body.pop_back();

    auto reply = [&](std::string const& msg)
    {
        ReplyTo(from, msg);
    };

    BotState const state = combat ? BotState::Combat : BotState::NonCombat;

    if (body.empty() || body == "?")
    {
        reply(std::string(combat ? "co: " : "nc: ") + FormatStrategies(combat));
        reply(std::string(combat ? "nc: " : "co: ") + FormatStrategies(!combat));
        return true;
    }

    std::string const report = _strategies.ChangeStrategy(body, state);
    SyncFlagsFromStrategies();

    if (!_food)
        StopResting();
    if (_stay && !_clientControlled)
    {
        ClearForcedTarget();
        _bot->GetMotionMaster()->Clear();
        _bot->GetMotionMaster()->MoveIdle();
        _followGuid = 0;
        _chaseGuid = 0;
    }
    else if (!_stay)
    {
        _forceRest = false;
        if (_bot->getStandState() != UNIT_STAND_STATE_STAND)
            _bot->SetStandState(UNIT_STAND_STATE_STAND);
        _followGuid = 0;
    }
    if (!_loot)
        _lootGuid = 0;
    if (_passive)
        ClearForcedTarget();

    // "co:" / "nc:" (no space) so status replies never re-parse as commands.
    reply(std::string(combat ? "co:" : "nc:") + report);
    reply(std::string(combat ? "co: " : "nc: ") + FormatStrategies(combat));
    return true;
}

bool PlayerbotAI::HandleChatCommand(Player* from, std::string const& text, bool acknowledge)
{
    if (!from || text.empty())
        return false;

    // Normalize: lowercase, collapse whitespace.
    std::string cmd;
    cmd.reserve(text.size());
    bool prevSpace = true;
    for (unsigned char ch : text)
    {
        if (std::isspace(ch))
        {
            if (!prevSpace)
            {
                cmd.push_back(' ');
                prevSpace = true;
            }
            continue;
        }
        cmd.push_back(char(std::tolower(ch)));
        prevSpace = false;
    }
    while (!cmd.empty() && cmd.back() == ' ')
        cmd.pop_back();

    if (cmd.empty())
        return false;

    // AC-style combat / non-combat strategy engine: "co +save mana", "nc -food", "co ?"
    if (HandleStrategyCommand(from, cmd, acknowledge))
        return true;

    auto ack = [&](char const* msg)
    {
        if (acknowledge)
            ReplyTo(from, msg);
    };

    if (cmd == "help")
    {
        ack("Orders: stay, follow, flee, leave, summon, grind, reset, passive, aggressive, attack, tank/dps attack, pull, rti, go, position, sell, sell all, mount, mount prefer, gossip, quests, eat/drink, maintenance, autogear [reset] [match|quality|ilvl], spells, debug. Strategies: co/nc +name,-name,~name or co?/nc?. Filters: @tank/@dps/@heal/@ranged.");
        return true;
    }

    if (cmd == "spells" || cmd == "spell")
    {
        ReportClassSpells(from);
        return true;
    }

    if (cmd == "debug" || cmd == "debug on")
    {
        _debugCombat = true;
        _strategies.ChangeStrategy("+debug", BotState::Combat);
        _hunterDebugSeq = 0;
        _hunterDebugLogPath.clear();
        if (IsHunterRanged())
        {
            HunterDebugLog("=== hunter debug ON ===");
            std::string msg = std::string("Combat debug ON. Hunter log: ") + _hunterDebugLogPath;
            ack(msg.c_str());
        }
        else
            ack("Combat debug ON — saying casts in chat.");
        return true;
    }
    if (cmd == "debug off")
    {
        if (IsHunterRanged())
            HunterDebugLog("=== hunter debug OFF ===");
        _debugCombat = false;
        _strategies.ChangeStrategy("-debug", BotState::Combat);
        ack("Combat debug OFF.");
        return true;
    }

    if (cmd == "sell" || cmd == "sell junk" || cmd == "sell all")
    {
        if (_clientControlled)
        {
            ack("Self-bot: sell from your bags at a vendor.");
            return true;
        }
        _forceVendor = true;
        _forceSellAll = (cmd == "sell all");
        HandleVendor();
        if (HasVendorJunk() || NeedsRepair())
            ack(_forceSellAll ? "Walking to vendor to sell dumpables." : "Walking to vendor to sell junk.");
        else
            ack("Nothing to sell (or traveling to a vendor hub).");
        return true;
    }

    if (cmd.compare(0, 12, "mount prefer") == 0)
    {
        if (_clientControlled)
        {
            ack("Self-bot: set preferred mounts on this character if desired.");
            return true;
        }
        std::string args = cmd.size() > 12 ? cmd.substr(12) : "";
        while (!args.empty() && args.front() == ' ')
            args.erase(args.begin());
        uint32 const guidLow = GUID_LOPART(_bot->GetGUID());
        if (args == "clear" || args == "none")
        {
            sBotPreferredMounts->ClearAll(guidLow);
            ack("Cleared preferred mounts.");
            return true;
        }
        uint32 spellId = uint32(atoi(args.c_str()));
        if (!spellId)
        {
            ack("Usage: mount prefer <spellId>|clear");
            return true;
        }
        BotMountKind const kind = ClassifyMountSpell(sSpellMgr->GetSpellInfo(spellId));
        if (kind == BotMountKind::None)
        {
            ack("That spell is not a mount.");
            return true;
        }
        bool const fly = kind >= BotMountKind::Flying;
        sBotPreferredMounts->Set(guidLow, fly ? BotPreferredMounts::Flying : BotPreferredMounts::Ground, spellId);
        ack(fly ? "Preferred flying mount set." : "Preferred ground mount set.");
        return true;
    }

    if (cmd == "mount")
    {
        if (_clientControlled)
        {
            ack("Self-bot: you control mounts.");
            return true;
        }
        BotMountKind kind = BotMountKind::SwiftGround;
        if (Group* group = _bot->GetGroup())
        {
            if (Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID()))
            {
                BotMountKind const masterKind = ClassifyUnitMount(leader);
                if (masterKind != BotMountKind::None)
                    kind = masterKind;
            }
        }
        if (TryMount(kind))
            ack("Mounting.");
        else
            ack("Cannot mount now.");
        return true;
    }

    if (cmd == "dismount")
    {
        TryDismount();
        ack("Dismounted.");
        return true;
    }

    if (cmd.compare(0, 6, "gossip") == 0)
    {
        if (_clientControlled)
        {
            ack("Self-bot: use the gossip UI.");
            return true;
        }
        Creature* npc = FindNearbyQuestgiver();
        if (!npc)
            npc = FindNearbyVendor();
        if (!npc || !_bot->IsWithinDistInMap(npc, INTERACTION_DISTANCE))
        {
            ack("No nearby NPC for gossip.");
            return true;
        }
        std::string args = cmd.size() > 6 ? cmd.substr(6) : "";
        while (!args.empty() && args.front() == ' ')
            args.erase(args.begin());
        if (args.empty())
        {
            _bot->PrepareGossipMenu(npc, Player::GetDefaultGossipMenuForSource(npc), true);
            GossipMenu& gmenu = _bot->PlayerTalkClass->GetGossipMenu();
            std::ostringstream oss;
            oss << "Gossip options:";
            for (auto const& kv : gmenu.GetMenuItems())
                oss << " [" << kv.first << "]";
            if (gmenu.Empty())
                oss << " (none)";
            ack(oss.str().c_str());
            return true;
        }
        uint32 const idx = uint32(atoi(args.c_str()));
        _bot->PrepareGossipMenu(npc, Player::GetDefaultGossipMenuForSource(npc), true);
        if (SelectGossipOption(npc, idx))
            ack("Selected gossip option.");
        else
            ack("Invalid gossip option.");
        return true;
    }

    if (cmd == "quests" || cmd == "accept" || cmd == "turn in" || cmd == "turnin")
    {
        _forceQuest = true;
        if (HandleAutoQuesting())
            ack("Handling quests.");
        else
            ack("No nearby quest to accept or turn in.");
        return true;
    }

    if (cmd == "go" || cmd.rfind("go ", 0) == 0)
    {
        std::string const args = (cmd == "go") ? std::string() : cmd.substr(3);
        return HandleGoCommand(from, args, acknowledge);
    }

    if (cmd == "position" || cmd.rfind("position ", 0) == 0)
    {
        std::string const args = (cmd == "position") ? std::string() : cmd.substr(9);
        return HandlePositionCommand(from, args, acknowledge);
    }

    if (cmd == "leave")
    {
        if (_clientControlled)
        {
            ack("Self-bot ignores leave.");
            return true;
        }
        if (!_bot->GetGroup())
        {
            ack("Not in a group.");
            return true;
        }
        _bot->RemoveFromGroup(GROUP_REMOVEMETHOD_LEAVE);
        _followGuid = 0;
        ack("Leaving group.");
        return true;
    }

    if (cmd == "stay")
    {
        if (_clientControlled)
        {
            ack("Self-bot: you control movement.");
            return true;
        }
        _strategies.ApplyStayPack();
        SyncFlagsFromStrategies();
        ClearForcedTarget();
        ClearTravelDestination();
        if (_bot->GetVictim())
            _bot->AttackStop();
        _bot->GetMotionMaster()->Clear();
        _bot->GetMotionMaster()->MoveIdle();
        _followGuid = 0;
        _lootGuid = 0;
        _chaseGuid = 0;
        ack("Staying.");
        return true;
    }

    if (cmd == "follow" || cmd == "come")
    {
        if (_clientControlled)
        {
            ack("Self-bot: you control movement.");
            return true;
        }
        _strategies.ApplyFollowPack();
        SyncFlagsFromStrategies();
        _forceRest = false;
        _holdAssist = false;
        ClearForcedTarget();
        ClearTravelDestination();
        if (_bot->getStandState() != UNIT_STAND_STATE_STAND)
            _bot->SetStandState(UNIT_STAND_STATE_STAND);
        _followGuid = 0;
        ack("Following.");
        return true;
    }

    if (cmd == "flee")
    {
        if (_clientControlled)
        {
            ack("Self-bot: you control movement.");
            return true;
        }
        // AC flee pack: follow + passive (don't fight while running to master).
        _strategies.ApplyFleePack();
        SyncFlagsFromStrategies();
        ClearForcedTarget();
        ClearTravelDestination();
        if (_bot->GetVictim())
            _bot->AttackStop();
        _chaseGuid = 0;
        _followGuid = 0;
        TeleportToPlayer(from);
        ack("Fleeing to you.");
        return true;
    }

    if (cmd == "summon")
    {
        if (_clientControlled)
        {
            ack("Self-bot ignores summon.");
            return true;
        }
        if (!TeleportToPlayer(from))
        {
            ack("Could not summon.");
            return true;
        }
        _strategies.ApplyFollowPack();
        SyncFlagsFromStrategies();
        ClearTravelDestination();
        _followGuid = 0;
        ack("Summoned.");
        return true;
    }

    if (cmd == "grind")
    {
        _strategies.ApplyGrindPack();
        SyncFlagsFromStrategies();
        ClearForcedTarget();
        ack("Grinding.");
        return true;
    }

    if (cmd == "reset")
    {
        _strategies.ApplyResetPack();
        SyncFlagsFromStrategies();
        _forceRest = false;
        _holdAssist = false;
        ClearForcedTarget();
        ClearTravelDestination();
        if (_bot->GetVictim())
            _bot->AttackStop();
        _chaseGuid = 0;
        _followGuid = 0;
        if (!_clientControlled)
        {
            _bot->GetMotionMaster()->Clear();
            _bot->GetMotionMaster()->MoveIdle();
        }
        if (_bot->IsSitState() && _bot->IsNonMeleeSpellCasted(false))
            _bot->InterruptNonMeleeSpells(false);
        if (_bot->getStandState() != UNIT_STAND_STATE_STAND)
            _bot->SetStandState(UNIT_STAND_STATE_STAND);
        ack("Reset.");
        return true;
    }

    if (cmd == "maintenance")
    {
        if (!sPlayerbotMgr->IsMaintenanceCommandEnabled())
        {
            ack("maintenance command is not allowed, please check the configuration.");
            return true;
        }
        sPlayerbotMgr->MaintainBot(_bot);
        ack("Maintenance done: spells, talents, glyphs, mounts and repairs refreshed.");
        return true;
    }

    if (cmd == "autogear" || cmd.rfind("autogear ", 0) == 0)
        return HandleAutoGearCommand(from, cmd == "autogear" ? std::string() : cmd.substr(9), acknowledge);

    if (cmd == "eat" || cmd == "drink" || cmd == "food")
    {
        if (_bot->IsInCombat())
        {
            ack("In combat — cannot eat/drink.");
            return true;
        }
        _forceRest = true;
        _strategies.Add("food", BotState::NonCombat);
        _strategies.Remove("grind", BotState::NonCombat);
        _strategies.Remove("grind", BotState::Combat);
        SyncFlagsFromStrategies();
        ClearForcedTarget();
        if (_bot->GetVictim())
            _bot->AttackStop();
        // Socket bots sit via StartRefreshment. Self-bot: sit only if standing so
        // an explicit eat/drink order can start; never fight an already-sitting player.
        if (_clientControlled && _bot->getStandState() != UNIT_STAND_STATE_SIT)
            _bot->SetStandState(UNIT_STAND_STATE_SIT);
        StartRefreshment();
        ack(_clientControlled
            ? "Refreshing (level-based food/drink; auto when low with nc +food; cancels at full)."
            : "Refreshing.");
        return true;
    }

    if (cmd == "heal")
    {
        std::string const report = _strategies.ChangeStrategy("+heal", BotState::Combat);
        _strategies.ChangeStrategy("+heal", BotState::NonCombat);
        SyncFlagsFromStrategies();
        ack(report.c_str());
        return true;
    }

    if (cmd == "healer dps" || cmd == "healdps" || cmd == "heal dps")
    {
        std::string const report = _strategies.ChangeStrategy("+healer dps", BotState::Combat);
        SyncFlagsFromStrategies();
        ack(report.c_str());
        return true;
    }

    if (cmd == "save mana" || cmd == "savemana" || cmd == "save mana on")
    {
        std::string const report = _strategies.ChangeStrategy("+save mana", BotState::Combat);
        SyncFlagsFromStrategies();
        ack(report.c_str());
        return true;
    }

    if (cmd == "save mana off" || cmd == "savemana off")
    {
        std::string const report = _strategies.ChangeStrategy("-save mana", BotState::Combat);
        SyncFlagsFromStrategies();
        ack(report.c_str());
        return true;
    }

    if (cmd == "passive")
    {
        _strategies.ApplyPassivePack();
        SyncFlagsFromStrategies();
        ClearForcedTarget();
        if (_bot->GetVictim())
            _bot->AttackStop();
        _chaseGuid = 0;
        if (!_clientControlled)
        {
            _bot->GetMotionMaster()->Clear();
            if (_stay)
                _bot->GetMotionMaster()->MoveIdle();
        }
        ack("Passive.");
        return true;
    }

    if (cmd == "aggressive" || cmd == "aggro")
    {
        _strategies.ApplyAggressivePack();
        SyncFlagsFromStrategies();
        _holdAssist = false;
        ack("Aggressive.");
        return true;
    }

    if (cmd == "attack")
    {
        Unit* target = from->GetSelectedUnit();
        if (!IsSafeAttackTarget(target))
        {
            ack("No valid target.");
            return true;
        }
        _strategies.ApplyAggressivePack();
        _strategies.Remove("grind", BotState::Combat);
        _strategies.Remove("grind", BotState::NonCombat);
        // Ordered attack: don't delay this pull.
        _strategies.Remove("wait for attack", BotState::Combat);
        SyncFlagsFromStrategies();
        _holdAssist = false;
        _combatStartTime = 0;
        SetForcedTarget(target);
        StopResting();
        ack("Attacking.");
        return true;
    }

    if (cmd == "tank attack")
    {
        if (GetCombatRole() != CombatRole::Tank)
            return true;
        Unit* target = from->GetSelectedUnit();
        if (!IsSafeAttackTarget(target))
        {
            ack("No valid target.");
            return true;
        }
        // AC TankAttackChatShortcut: -passive on both engines, then pull.
        _strategies.ChangeStrategy("-passive", BotState::NonCombat);
        _strategies.ChangeStrategy("-passive", BotState::Combat);
        _strategies.Remove("wait for attack", BotState::Combat);
        SyncFlagsFromStrategies();
        _holdAssist = false;
        _combatStartTime = 0;
        SetForcedTarget(target);
        StopResting();
        ack("Tank attacking.");
        return true;
    }

    // AC pull: tanks reach the master's target (or RTI), cast an opener, then fight.
    if (cmd == "pull")
    {
        if (GetCombatRole() != CombatRole::Tank)
            return true;
        Unit* target = from->GetSelectedUnit();
        if (!IsSafeAttackTarget(target))
            target = _targets.GetRtiTarget(this);
        if (!IsSafeAttackTarget(target))
        {
            ack("No valid pull target.");
            return true;
        }
        _strategies.ChangeStrategy("-passive", BotState::NonCombat);
        _strategies.ChangeStrategy("-passive", BotState::Combat);
        _strategies.Remove("wait for attack", BotState::Combat);
        SyncFlagsFromStrategies();
        BeginPullSequence(target);
        StopResting();
        ack("Pulling.");
        return true;
    }

    // AC rti: set which raid icon bots prefer for focus fire (default skull).
    if (cmd == "rti" || cmd == "rti ?" || cmd.rfind("rti ", 0) == 0)
    {
        if (cmd == "rti" || cmd == "rti ?")
        {
            std::string reply = "rti: " + _targets.GetRti();
            ack(reply.c_str());
            return true;
        }
        std::string icon = cmd.substr(4);
        while (!icon.empty() && icon.front() == ' ')
            icon.erase(icon.begin());
        if (icon == "?" || icon.empty())
        {
            std::string reply = "rti: " + _targets.GetRti();
            ack(reply.c_str());
            return true;
        }
        if (BotTargetValues::RtiIndexFromName(icon) < 0)
        {
            ack("rti icons: star circle diamond triangle moon square cross skull");
            return true;
        }
        _targets.SetRti(icon);
        std::string reply = "rti: " + _targets.GetRti();
        ack(reply.c_str());
        return true;
    }

    if (cmd == "dps attack")
    {
        if (GetCombatRole() != CombatRole::Damage)
            return true;
        Unit* target = from->GetSelectedUnit();
        if (!IsSafeAttackTarget(target))
        {
            ack("No valid target.");
            return true;
        }
        _strategies.ApplyAggressivePack();
        _strategies.Remove("wait for attack", BotState::Combat);
        SyncFlagsFromStrategies();
        _holdAssist = false;
        _combatStartTime = 0;
        SetForcedTarget(target);
        StopResting();
        ack("DPS attacking.");
        return true;
    }

    return false;
}

void PlayerbotAI::SyncPhaseWithMaster(Unit* master)
{
    if (!_bot || !master || _clientControlled)
        return;

    Player* viewer = master->ToPlayer();

    // Alive-player ghost visibility. If these flags are wrong, CanSeeOrDetect
    // falls into the "need IsGroupVisibleFor" branch — invite appears to "fix"
    // visibility even when phases already match.
    _bot->m_serverSideVisibility.SetValue(SERVERSIDE_VISIBILITY_GHOST,
        GHOST_VISIBILITY_ALIVE | GHOST_VISIBILITY_GHOST);
    _bot->m_serverSideVisibilityDetect.SetValue(SERVERSIDE_VISIBILITY_GHOST,
        GHOST_VISIBILITY_ALIVE);

    // Party members bypass stealth/invis via IsAlwaysDetectableFor; clear those
    // so ungrouped bots are not stuck invisible for the same reason.
    _bot->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);
    _bot->RemoveAurasByType(SPELL_AURA_MOD_INVISIBILITY);
    _bot->RemoveAurasByType(SPELL_AURA_MOD_CAMOUFLAGE);

    // Copy the viewer's real phase-id set even if they are currently GM.
    std::set<uint32> const& masterPhases = master->GetPhases();
    uint32 const masterMask = master->GetPhaseMask();
    uint32 const applyMask = (masterMask == PHASEMASK_ANYWHERE) ? PHASEMASK_NORMAL : masterMask;
    bool const phasesMatch = _bot->GetPhases() == masterPhases && _bot->GetPhaseMask() == applyMask;

    if (!phasesMatch)
    {
        _bot->ClearPhases(false);
        for (uint32 phaseId : masterPhases)
            _bot->SetPhased(phaseId, false, true);

        _bot->SetPhaseMask(applyMask, false);
        _bot->RebuildTerrainSwaps();
        _bot->RebuildWorldMapAreaSwaps();
    }

    if (_bot->IsInWorld())
        _bot->UpdateObjectVisibility(true);

    if (viewer && viewer->IsInWorld() && viewer->GetMap() == _bot->GetMap())
        viewer->UpdateVisibilityOf(_bot);
}

void PlayerbotAI::EnsureVisiblePhases()
{
    if (!_bot || !_bot->IsInWorld() || _clientControlled)
        return;

    Player* source = nullptr;

    if (Group* group = _bot->GetGroup())
    {
        // Prefer a non-GM real member on the same map (accurate phase ids).
        Player* gmFallback = nullptr;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == _bot || !member->IsInWorld())
                continue;
            if (member->GetMap() != _bot->GetMap())
                continue;
            if (!member->GetSession() || member->GetSession()->IsBot())
                continue;

            if (!member->IsGameMaster())
            {
                source = member;
                break;
            }
            if (!gmFallback)
                gmFallback = member;
        }
        if (!source)
            source = gmFallback;
    }

    if (!source)
    {
        // Same-map nearest real player (no short range cap — grouped bots already
        // synced at any distance; ungrouped .summon / world bots need that too).
        float bestDist = std::numeric_limits<float>::max();
        SF_SHARED_GUARD readGuard(*HashMapHolder<Player>::GetLock());
        HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
        for (HashMapHolder<Player>::MapType::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* player = itr->second;
            if (!player || player == _bot || !player->IsInWorld())
                continue;
            if (player->GetMap() != _bot->GetMap())
                continue;
            if (!player->GetSession() || player->GetSession()->IsBot())
                continue;

            float const dist = _bot->GetDistance(player);
            if (dist >= bestDist)
                continue;

            bestDist = dist;
            source = player;
        }
    }

    if (source)
        SyncPhaseWithMaster(source);
}

bool PlayerbotAI::TeleportToPlayer(Player* master)
{
    if (!master || !_bot || _clientControlled || !master->IsInWorld() || !_bot->IsInWorld())
        return false;

    // A taxi spline keeps running after TeleportTo and walks the bot back off the summon point.
    if (_bot->IsInFlight())
    {
        _bot->GetMotionMaster()->MovementExpired();
        _bot->CleanupAfterTaxiFlight();
    }

    // Dead bots cannot change map (no corpse in the destination instance) and the
    // death timer still repops them at the graveyard after a same-map teleport.
    if (!_bot->IsAlive())
    {
        _bot->ResurrectPlayer(1.0f, false);
        _bot->SpawnCorpseBones();
        if (_bot->getStandState() != UNIT_STAND_STATE_STAND)
            _bot->SetStandState(UNIT_STAND_STATE_STAND);
    }

    // Finish a stuck near/far teleport before starting another, or the new dest is dropped.
    if (_bot->IsBeingTeleported())
    {
        if (WorldSession* session = _bot->GetSession())
            session->FinalizeBotTeleport();
    }

    _bot->GetMotionMaster()->Clear();
    _bot->GetMotionMaster()->MoveIdle();

    if (!_bot->TeleportTo(master->GetMapId(), master->GetPositionX(), master->GetPositionY(),
        master->GetPositionZ(), master->GetOrientation()))
        return false;

    if (_bot->GetSession() && _bot->GetSession()->IsBot())
        _bot->GetSession()->FinalizeBotTeleport();
    // FinalizeBotTeleport → UpdateZone → UpdateAreaPhase uses the bot's own
    // quest/aura conditions, which often diverge from the master's MoP phases.
    SyncPhaseWithMaster(master);

    _followGuid = 0;
    _chaseGuid = 0;
    _lootGuid = 0;
    return true;
}

void PlayerbotAI::TeleportToLeader(Player* leader)
{
    TeleportToPlayer(leader);
}
