/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/

#include "PlayerbotMgr.h"
#include "PlayerbotAI.h"
#include "engine/BotMovement.h"
#include "rotations/BotRotation.h"
#include "AccountMgr.h"
#include "BotGearSets.h"
#include "BotPreferredMounts.h"
#include "BotTeleportMaps.h"
#include "BotVendorHubs.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupReference.h"
#include "Item.h"
#include "ItemPrototype.h"
#include "LFGMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "RBAC.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

PlayerbotMgr* PlayerbotMgr::instance()
{
    static PlayerbotMgr instance;
    return &instance;
}

namespace
{
    // Module .conf files are not loaded by the core automatically; resolve the
    // playerbots.conf next to the main worldserver.conf and merge it in.
    std::string ResolveModuleConfigPath()
    {
        std::string mainConfig = sConfigMgr->GetFilename();
        std::string::size_type slash = mainConfig.find_last_of("/\\");
        std::string directory = (slash != std::string::npos) ? mainConfig.substr(0, slash + 1) : "";
        return directory + "playerbots.conf";
    }

    uint64 BotMapKey(uint32 mapId, uint32 instanceId)
    {
        return (uint64(mapId) << 32) | uint64(instanceId);
    }

    bool BotGroupedWithRealPlayer(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == bot || !member->IsInWorld())
                continue;
            WorldSession* session = member->GetSession();
            if (session && !session->IsBot())
                return true;
        }
        return false;
    }

    // Dungeon, raid, scenario, battleground, or arena: one real player wakes
    // every bot in that instance, including rooms the player has not reached.
    bool BotInstanceKeepsFullTick(Map const* map)
    {
        return map && (map->IsInstance() || map->IsBattlegroundOrArena());
    }
}

void PlayerbotMgr::LoadConfig()
{
    // Merge the module configuration into the shared config store. LoadMore is
    // additive, so this only adds/overrides the Playerbots.* keys.
    std::string configPath = ResolveModuleConfigPath();
    if (!sConfigMgr->LoadMore(configPath.c_str()))
        SF_LOG_ERROR("modules", "[mod-playerbots] Could not read config file '%s'; using defaults (module disabled).",
            configPath.c_str());

    _enabled = sConfigMgr->GetBoolDefault("Playerbots.Enable", false);
    _randomBotsEnabled = sConfigMgr->GetBoolDefault("Playerbots.RandomBots.Enable", false);
    _maxRandomBots = uint32(sConfigMgr->GetIntDefault("Playerbots.RandomBots.MaxBots", 0));
    _accountPrefix = sConfigMgr->GetStringDefault("Playerbots.RandomBots.AccountPrefix", "RNDBOT");
    _loginIntervalMs = uint32(sConfigMgr->GetIntDefault("Playerbots.RandomBots.LoginInterval", 250));
    if (_loginIntervalMs < 1)
        _loginIntervalMs = 1;
    _maxPendingLogins = uint32(sConfigMgr->GetIntDefault("Playerbots.RandomBots.MaxPendingLogins", 8));
    if (_maxPendingLogins < 1)
        _maxPendingLogins = 1;
    if (_maxPendingLogins > 64)
        _maxPendingLogins = 64;
    _loginsPerTick = uint32(sConfigMgr->GetIntDefault("Playerbots.RandomBots.LoginsPerTick", 1));
    if (_loginsPerTick < 1)
        _loginsPerTick = 1;
    if (_loginsPerTick > 10)
        _loginsPerTick = 10;

    _joinLfg = sConfigMgr->GetBoolDefault("Playerbots.RandomBotJoinLfg", false);
    _joinLfgMaxBots = uint32(sConfigMgr->GetIntDefault("Playerbots.RandomBotJoinLfg.MaxBots", 10));
    _joinLfgLevelRange = uint32(sConfigMgr->GetIntDefault("Playerbots.RandomBotJoinLfg.LevelRange", 5));
    if (_joinLfgMaxBots > 40)
        _joinLfgMaxBots = 40;
    if (_joinLfgLevelRange > 20)
        _joinLfgLevelRange = 20;

    _autoCreateOnStartup = sConfigMgr->GetBoolDefault("Playerbots.AutoCreate.OnStartup", false);
    _deleteRandomBotAccounts = sConfigMgr->GetBoolDefault("Playerbots.DeleteRandomBotAccounts", false);
    _deleteOrphanCharacters = sConfigMgr->GetBoolDefault("Playerbots.DeleteOrphanCharacters", true);
    _autoAccountCount = uint32(sConfigMgr->GetIntDefault("Playerbots.AutoCreate.AccountCount", 0));
    _autoPassword = sConfigMgr->GetStringDefault("Playerbots.AutoCreate.AccountPassword", "password");
    _autoCharsPerAccount = uint32(sConfigMgr->GetIntDefault("Playerbots.AutoCreate.CharactersPerAccount", 1));
    _autoAlliancePct = uint32(sConfigMgr->GetIntDefault("Playerbots.AutoCreate.AlliancePct", 50));
    _autoTankPct = uint32(sConfigMgr->GetIntDefault("Playerbots.AutoCreate.TankPct", 20));
    _autoHealerPct = uint32(sConfigMgr->GetIntDefault("Playerbots.AutoCreate.HealerPct", 20));

    // Legacy AutoCreate.Level seeds Min/Max when those keys are omitted.
    uint32 const legacyLevel = uint32(sConfigMgr->GetIntDefault("Playerbots.AutoCreate.Level", 1));
    _autoMinLevel = uint32(sConfigMgr->GetIntDefault("Playerbots.AutoCreate.MinLevel", int32(legacyLevel)));
    _autoMaxLevel = uint32(sConfigMgr->GetIntDefault("Playerbots.AutoCreate.MaxLevel", int32(legacyLevel)));
    _autoInitOnCreate = sConfigMgr->GetBoolDefault("Playerbots.AutoCreate.InitOnCreate", false);
    _autoTeleportOnInit = sConfigMgr->GetBoolDefault("Playerbots.AutoCreate.TeleportOnInit", true);
    _initPerTick = uint32(sConfigMgr->GetIntDefault("Playerbots.Init.PerTick", 5));
    if (_initPerTick < 1)
        _initPerTick = 1;
    if (_initPerTick > 50)
        _initPerTick = 50;

    _gearMinQuality = sConfigMgr->GetIntDefault("Playerbots.Gear.MinQuality", int(ITEM_QUALITY_UNCOMMON));
    _gearMaxQuality = sConfigMgr->GetIntDefault("Playerbots.Gear.MaxQuality", int(ITEM_QUALITY_EPIC));

    _autoGearCommand = sConfigMgr->GetBoolDefault("Playerbots.AutoGear.Command", true);
    _autoGearAltBots = sConfigMgr->GetBoolDefault("Playerbots.AutoGear.AltBots", true);
    _autoGearQualityLimit = std::max(int(ITEM_QUALITY_NORMAL), std::min(int(ITEM_QUALITY_LEGENDARY),
        sConfigMgr->GetIntDefault("Playerbots.AutoGear.QualityLimit", int(ITEM_QUALITY_EPIC))));
    _autoGearItemLevelLimit = uint32(std::max(0, sConfigMgr->GetIntDefault("Playerbots.AutoGear.ItemLevelLimit", 0)));
    _maintenanceCommand = sConfigMgr->GetBoolDefault("Playerbots.Maintenance.Command", true);
    _altMaintenanceSpells = sConfigMgr->GetBoolDefault("Playerbots.AltMaintenance.Spells", true);
    _altMaintenanceTalents = sConfigMgr->GetBoolDefault("Playerbots.AltMaintenance.Talents", true);
    _altMaintenanceGlyphs = sConfigMgr->GetBoolDefault("Playerbots.AltMaintenance.Glyphs", true);
    _altMaintenanceMounts = sConfigMgr->GetBoolDefault("Playerbots.AltMaintenance.Mounts", true);
    _altMaintenancePet = sConfigMgr->GetBoolDefault("Playerbots.AltMaintenance.Pet", true);

    // Optional numeric thresholds (strategies themselves are runtime co/nc only).
    _restHealthPct = float(sConfigMgr->GetFloatDefault("Playerbots.Rest.HealthPct", 50.0f));
    _restManaPct = float(sConfigMgr->GetFloatDefault("Playerbots.Rest.ManaPct", 50.0f));
    _almostFullHealthPct = float(sConfigMgr->GetFloatDefault("Playerbots.Rest.AlmostFullHealth", 85.0f));
    _mediumManaPct = float(sConfigMgr->GetFloatDefault("Playerbots.Rest.MediumMana", 40.0f));
    _threatThrottlePct = float(sConfigMgr->GetFloatDefault("Playerbots.Threat.ThrottlePct", 80.0f));
    _fleeDistance = float(sConfigMgr->GetFloatDefault("Playerbots.Flee.Distance", 20.0f));
    _fleeEnabled = sConfigMgr->GetBoolDefault("Playerbots.Flee.Enabled", true);
    _saveManaThreshold = float(sConfigMgr->GetFloatDefault("Playerbots.SaveMana.Threshold", 60.0f));
    _waitForAttackSeconds = uint32(sConfigMgr->GetIntDefault("Playerbots.WaitForAttack.Seconds", 5));
    _vendorSellGreens = sConfigMgr->GetBoolDefault("Playerbots.Vendor.SellGreens", true);
    _vendorHubMaxDistance = float(sConfigMgr->GetFloatDefault("Playerbots.Vendor.HubMaxDistance", 500.0f));
    _questAutoPickReward = sConfigMgr->GetBoolDefault("Playerbots.Quest.AutoPickReward", true);

    _activityEnabled = sConfigMgr->GetBoolDefault("Playerbots.Activity.Enable", true);
    _activityActiveRange = sConfigMgr->GetFloatDefault("Playerbots.Activity.ActiveRange", 0.0f);
    _activitySlowInterval = uint32(sConfigMgr->GetIntDefault("Playerbots.Activity.SlowInterval", 5000));
    if (_activityActiveRange < 0.0f)
        _activityActiveRange = 0.0f;
    if (_activityActiveRange > 2000.0f)
        _activityActiveRange = 2000.0f;
    if (_activitySlowInterval < 1)
        _activitySlowInterval = 5000;
    if (_activitySlowInterval > 60000)
        _activitySlowInterval = 60000;

    // Clamp to sane ranges so bad config can't create invalid characters.
    if (_autoAlliancePct > 100) _autoAlliancePct = 100;
    if (_autoTankPct > 100) _autoTankPct = 100;
    if (_autoHealerPct > 100) _autoHealerPct = 100;
    if (_autoTankPct + _autoHealerPct > 100) _autoHealerPct = 100 - _autoTankPct;
    if (_autoCharsPerAccount > 11) _autoCharsPerAccount = 11;   // realm cap
    if (_autoMinLevel < 1) _autoMinLevel = 1;
    if (_autoMaxLevel < 1) _autoMaxLevel = 1;
    if (_autoMaxLevel < _autoMinLevel)
        std::swap(_autoMinLevel, _autoMaxLevel);
    {
        uint32 const maxPlayerLevel = uint32(sWorld->getIntConfig(WorldIntConfigs::CONFIG_MAX_PLAYER_LEVEL));
        if (_autoMinLevel > maxPlayerLevel) _autoMinLevel = maxPlayerLevel;
        if (_autoMaxLevel > maxPlayerLevel) _autoMaxLevel = maxPlayerLevel;
    }
    if (_gearMinQuality < int(ITEM_QUALITY_POOR)) _gearMinQuality = int(ITEM_QUALITY_POOR);
    if (_gearMaxQuality > int(ITEM_QUALITY_LEGENDARY)) _gearMaxQuality = int(ITEM_QUALITY_LEGENDARY);
    if (_gearMinQuality > _gearMaxQuality)
        std::swap(_gearMinQuality, _gearMaxQuality);
    if (_restHealthPct < 1.0f) _restHealthPct = 1.0f;
    if (_restHealthPct > 100.0f) _restHealthPct = 100.0f;
    if (_restManaPct < 1.0f) _restManaPct = 1.0f;
    if (_restManaPct > 100.0f) _restManaPct = 100.0f;
    if (_almostFullHealthPct < 1.0f) _almostFullHealthPct = 1.0f;
    if (_almostFullHealthPct > 100.0f) _almostFullHealthPct = 100.0f;
    if (_mediumManaPct < 1.0f) _mediumManaPct = 1.0f;
    if (_mediumManaPct > 100.0f) _mediumManaPct = 100.0f;
    if (_threatThrottlePct < 10.0f) _threatThrottlePct = 10.0f;
    if (_threatThrottlePct > 100.0f) _threatThrottlePct = 100.0f;
    if (_fleeDistance < 5.0f) _fleeDistance = 5.0f;
    if (_fleeDistance > 40.0f) _fleeDistance = 40.0f;
    if (_saveManaThreshold < 1.0f) _saveManaThreshold = 1.0f;
    if (_saveManaThreshold > 100.0f) _saveManaThreshold = 100.0f;
    if (_waitForAttackSeconds > 30) _waitForAttackSeconds = 30;

    // Force a candidate reload so prefix changes take effect on .reload config.
    _candidatesLoaded = false;

    if (_enabled)
        SF_LOG_INFO("modules", "[mod-playerbots] Enabled (random bots: %s, max: %u, account prefix: '%s', LFG join: %s, levels %u-%u, gear quality %d-%d, activity: %s).",
            _randomBotsEnabled ? "on" : "off", _maxRandomBots, _accountPrefix.c_str(),
            _joinLfg ? "on" : "off", _autoMinLevel, _autoMaxLevel, _gearMinQuality, _gearMaxQuality,
            _activityEnabled ? "on" : "off");
    else
        SF_LOG_INFO("modules", "[mod-playerbots] Disabled via configuration.");
}

void PlayerbotMgr::Update(uint32 diff)
{
    if (!_enabled)
        return;

    // Drop bots whose player has left the world for any reason.
    CleanupDeadBots();

    // Finish ready async logins (at most LoginsPerTick LoadFromDB per tick).
    ProcessPendingLogins();

    // Mass gear init must run on the world thread, a few bots per tick.
    // This is NOT the random-bot login rate — see RandomBots.LoginsPerTick.
    ProcessInitQueue();

    // Catch bots that logged in before deferred init existed (or after a
    // restart that lost in-memory deferred role/spec) — queue ungeared ones.
    EnqueueUngearedOnlineBots(diff);

    // LFG role checks + proposal accepts must run on the world thread. Calling
    // UpdateProposal from Map::Update (via PlayerbotAI::UpdateAI) races the LFG
    // mgr and can crash in MakeNewGroup / RemoveFromQueue (premade party enter).
    for (auto const& pair : _ai)
    {
        PlayerbotAI* ai = pair.second;
        if (!ai || ai->IsClientControlled())
            continue;
        Player* bot = ai->GetBot();
        if (!bot || bot->GetTypeId() != TypeID::TYPEID_PLAYER || !bot->IsInWorld())
            continue;
        // Dormant and slow bots do not get Player::Update, so party invites
        // would sit until a player walked onto their map.
        if (WorldSession* session = bot->GetSession())
            if (session->IsBot() && session->GetBotActivity() != WorldSession::BotActivity::Full)
                ai->AcceptPendingInvite();
        ai->HandleLfg();
    }

    // AC-style: when a real player solo-queues LFG, fill with level-matched bots.
    UpdateLfgAutoJoin(diff);

    // Runs after map updates, so the flags apply on the next tick. Also runs
    // when the random pool is off — manually added bots need the same gate.
    UpdateBotActivity();

    if (!_randomBotsEnabled)
        return;

    if (!_candidatesLoaded)
        LoadCandidates();

    // Trim excess random bots if the cap was lowered.
    while (_randomBots.size() > _maxRandomBots)
        RemoveBot(*_randomBots.begin());

    // Start at most one new async login per interval while under caps.
    _loginTimer += diff;
    if (_loginTimer < _loginIntervalMs)
        return;
    _loginTimer = 0;

    uint32 const onlineOrPending = uint32(_randomBots.size() + _pendingLogins.size());
    if (onlineOrPending < _maxRandomBots && _pendingLogins.size() < _maxPendingLogins)
        TrySpawnRandomBot();
}

void PlayerbotMgr::QuiesceBot(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return;

    std::vector<Unit*> attackers(bot->getAttackers().begin(), bot->getAttackers().end());
    for (Unit* attacker : attackers)
    {
        if (!attacker || !attacker->IsInWorld())
            continue;
        bot->getHostileRefManager().deleteReference(attacker);
        if (attacker->CanHaveThreatList() && attacker->getThreatManager().isThreatListEmpty())
            attacker->CombatStop(true);
    }

    bot->CombatStopWithPets(true);
    BotMovement::StopAndIdle(bot);
}

void PlayerbotMgr::GetBotActivityCounts(uint32& full, uint32& slow, uint32& dormant) const
{
    full = 0;
    slow = 0;
    dormant = 0;
    for (auto const& pair : _bots)
    {
        WorldSession* session = pair.second;
        if (!session)
            continue;
        switch (session->GetBotActivity())
        {
            case WorldSession::BotActivity::Slow:
                ++slow;
                break;
            case WorldSession::BotActivity::Dormant:
                ++dormant;
                break;
            default:
                ++full;
                break;
        }
    }
}

void PlayerbotMgr::UpdateBotActivity()
{
    if (!_activityEnabled)
    {
        for (auto const& pair : _bots)
            if (pair.second)
                pair.second->SetBotActivity(WorldSession::BotActivity::Full);
        return;
    }

    struct RealPlayerPos
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    std::unordered_map<uint64, std::vector<RealPlayerPos>> playersByMap;
    SessionMap const& sessions = sWorld->GetAllSessions();
    for (SessionMap::const_iterator itr = sessions.begin(); itr != sessions.end(); ++itr)
    {
        WorldSession* session = itr->second;
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !player->IsInWorld() || !player->GetMap())
            continue;
        if (session->IsBot())
            continue;

        RealPlayerPos pos;
        pos.x = player->GetPositionX();
        pos.y = player->GetPositionY();
        pos.z = player->GetPositionZ();
        playersByMap[BotMapKey(player->GetMapId(), player->GetInstanceId())].push_back(pos);
    }

    std::unordered_set<uint64> initializing;
    for (QueuedBotInit const& job : _initQueue)
        initializing.insert(job.guid);

    for (auto const& pair : _bots)
    {
        WorldSession* session = pair.second;
        if (!session)
            continue;

        session->SetBotSlowInterval(_activitySlowInterval);

        Player* bot = session->GetPlayer();
        WorldSession::BotActivity next = WorldSession::BotActivity::Full;
        if (bot && bot->IsInWorld() && bot->GetMap() && !IsSelfBot(bot->GetGUID()))
        {
            bool const porting = bot->IsBeingTeleported() || bot->HasDelayedTeleport();
            if (!porting && !initializing.count(bot->GetGUID()))
            {
                if (BotGroupedWithRealPlayer(bot))
                    next = WorldSession::BotActivity::Full;
                else
                {
                    auto onMap = playersByMap.find(BotMapKey(bot->GetMapId(), bot->GetInstanceId()));
                    if (onMap == playersByMap.end())
                        next = WorldSession::BotActivity::Dormant;
                    else if (BotInstanceKeepsFullTick(bot->GetMap()))
                        next = WorldSession::BotActivity::Full;
                    else
                    {
                        float range = _activityActiveRange;
                        if (range <= 0.0f)
                            range = bot->GetMap()->GetVisibilityRange();
                        if (range < 1.0f)
                            range = 100.0f;
                        float const rangeSq = range * range;
                        bool inRange = false;
                        for (RealPlayerPos const& pos : onMap->second)
                        {
                            float const dx = pos.x - bot->GetPositionX();
                            float const dy = pos.y - bot->GetPositionY();
                            float const dz = pos.z - bot->GetPositionZ();
                            if (dx * dx + dy * dy + dz * dz <= rangeSq)
                            {
                                inRange = true;
                                break;
                            }
                        }
                        next = inRange ? WorldSession::BotActivity::Full
                                       : WorldSession::BotActivity::Slow;
                    }
                }
            }
        }

        if (session->GetBotActivity() == WorldSession::BotActivity::Full
            && next != WorldSession::BotActivity::Full)
            QuiesceBot(bot);
        session->SetBotActivity(next);
    }
}

void PlayerbotMgr::LoadCandidates()
{
    _candidates.clear();
    _candidatesLoaded = true;

    if (_accountPrefix.empty())
    {
        SF_LOG_ERROR("modules", "[mod-playerbots] RandomBots.AccountPrefix is empty; no candidates loaded.");
        return;
    }

    // account is stored in the auth database, characters in the character database,
    // so the lookup is done in two steps rather than a cross-database join.
    QueryResult accounts = LoginDatabase.PQuery(
        "SELECT id FROM account WHERE username LIKE '%s%%'", _accountPrefix.c_str());
    if (!accounts)
    {
        SF_LOG_INFO("modules", "[mod-playerbots] No bot accounts found for prefix '%s'.", _accountPrefix.c_str());
        return;
    }

    std::ostringstream accountIds;
    bool first = true;
    do
    {
        if (!first)
            accountIds << ',';
        accountIds << (*accounts)[0].GetUInt32();
        first = false;
    } while (accounts->NextRow());

    // Cache accountId with guid so BeginSpawnBot skips GetPlayerAccountIdByGUID.
    QueryResult characters = CharacterDatabase.PQuery(
        "SELECT guid, account FROM characters WHERE account IN (%s)", accountIds.str().c_str());
    if (!characters)
    {
        SF_LOG_INFO("modules", "[mod-playerbots] Bot accounts have no characters (prefix '%s').", _accountPrefix.c_str());
        return;
    }

    do
    {
        Field* fields = characters->Fetch();
        BotCandidate c;
        c.guid = MAKE_NEW_GUID(fields[0].GetUInt32(), 0, HIGHGUID_PLAYER);
        c.accountId = fields[1].GetUInt32();
        _candidates.push_back(c);
    } while (characters->NextRow());

    SF_LOG_INFO("modules", "[mod-playerbots] Loaded %u candidate bot character(s) from prefix '%s'.",
        uint32(_candidates.size()), _accountPrefix.c_str());
}

uint32 PlayerbotMgr::DeleteOrphanCharacters()
{
    // auth and characters may live on different servers, so diff the two id
    // sets here instead of a cross-database join.
    std::unordered_set<uint32> accountIds;
    if (QueryResult accounts = LoginDatabase.Query("SELECT id FROM account"))
    {
        do
            accountIds.insert((*accounts)[0].GetUInt32());
        while (accounts->NextRow());
    }
    else
        return 0; // never treat an unreadable/empty auth table as "everything orphaned"

    QueryResult chars = CharacterDatabase.Query("SELECT guid, account FROM characters WHERE account <> 0");
    if (!chars)
        return 0;

    std::vector<uint32> orphans;
    do
    {
        Field* f = chars->Fetch();
        if (!accountIds.count(f[1].GetUInt32()))
            orphans.push_back(f[0].GetUInt32());
    } while (chars->NextRow());

    for (uint32 guid : orphans)
        Player::DeleteFromDB(MAKE_NEW_GUID(guid, 0, HIGHGUID_PLAYER), 0, false, true);

    if (!orphans.empty())
        SF_LOG_ERROR("modules",
            "[mod-playerbots] Deleted %u character(s) whose account no longer exists.", uint32(orphans.size()));
    return uint32(orphans.size());
}

uint32 PlayerbotMgr::RepairStrandedBotRows()
{
    if (_accountPrefix.empty())
        return 0;

    QueryResult accounts = LoginDatabase.PQuery(
        "SELECT id FROM account WHERE username LIKE '%s%%'", _accountPrefix.c_str());
    if (!accounts)
        return 0;

    std::ostringstream accountIds;
    bool first = true;
    do
    {
        if (!first)
            accountIds << ',';
        accountIds << (*accounts)[0].GetUInt32();
        first = false;
    } while (accounts->NextRow());

    QueryResult rows = CharacterDatabase.PQuery(
        "SELECT c.guid, c.map, c.position_x, c.position_y, h.mapId, h.zoneId, h.posX, h.posY, h.posZ "
        "FROM characters c JOIN character_homebind h ON h.guid = c.guid "
        "WHERE c.account IN (%s) AND c.map <> h.mapId", accountIds.str().c_str());
    if (!rows)
        return 0;

    // Init scatter never lands within this distance of the race start on a
    // different continent, so such rows are leftovers of a broken teleport.
    float const nearHome = 1500.0f;
    uint32 repaired = 0;
    do
    {
        Field* f = rows->Fetch();
        uint32 const guid = f[0].GetUInt32();
        uint32 const map = f[1].GetUInt16();
        float const x = f[2].GetFloat();
        float const y = f[3].GetFloat();
        uint32 const homeMap = f[4].GetUInt16();
        uint32 const homeZone = f[5].GetUInt16();
        float const homeX = f[6].GetFloat();
        float const homeY = f[7].GetFloat();
        float const homeZ = f[8].GetFloat();

        bool const nearHomeCoords = std::fabs(x - homeX) < nearHome && std::fabs(y - homeY) < nearHome;
        if (!nearHomeCoords && BotTeleportMaps::HasMapTile(map, x, y))
            continue;

        CharacterDatabase.DirectPExecute(
            "UPDATE characters SET map = %u, position_x = %f, position_y = %f, position_z = %f, zone = %u, "
            "instance_id = 0, trans_x = 0, trans_y = 0, trans_z = 0, transguid = 0, taxi_path = '' WHERE guid = %u",
            homeMap, homeX, homeY, homeZ, homeZone, guid);
        ++repaired;
    } while (rows->NextRow());

    if (repaired)
        SF_LOG_ERROR("modules",
            "[mod-playerbots] Moved %u bot character(s) with mismatched map/coordinates back to homebind.", repaired);
    return repaired;
}

void PlayerbotMgr::TrySpawnRandomBot()
{
    for (BotCandidate const& candidate : _candidates)
    {
        uint64 const guid = candidate.guid;
        if (_bots.find(guid) != _bots.end())
            continue;
        if (_pendingLoginGuids.find(guid) != _pendingLoginGuids.end())
            continue;
        if (ObjectAccessor::FindPlayer(guid))
            continue;

        std::string error;
        if (BeginSpawnBot(guid, true, candidate.accountId, &error))
        {
            SF_LOG_INFO("modules", "[mod-playerbots] Random bot login started (pending %u, online %u/%u).",
                uint32(_pendingLogins.size()), uint32(_randomBots.size()), _maxRandomBots);
            return;
        }

        SF_LOG_DEBUG("modules", "[mod-playerbots] Skipping candidate GUID %u: %s", GUID_LOPART(guid), error.c_str());
    }
}

void PlayerbotMgr::CleanupDeadBots()
{
    for (auto it = _bots.begin(); it != _bots.end();)
    {
        WorldSession* session = it->second;
        Player* player = session ? session->GetPlayer() : nullptr;
        // Only drop bots whose Player is gone. !IsInWorld() alone is wrong —
        // LFG/group teleports briefly take the player out of the world, and deleting
        // the session there permanently despawns the bot (who-list empty, "vanished
        // after uninvite").
        if (!session || !player)
        {
            uint64 guid = it->first;
            it = _bots.erase(it);
            _randomBots.erase(guid);
            DestroyBotAI(guid);
            delete session;
            continue;
        }

        // Socketless bots never ACK worldport / near-teleport. Finish those so
        // they re-enter the world and show up in /who again.
        if (session->IsBot() && player->IsBeingTeleported())
            session->FinalizeBotTeleport();

        ++it;
    }
}

// Party LFG: grouped bots answer role checks via PlayerbotAI::HandleLfg.
// When RandomBotJoinLfg is on, solo bots fill complementary roles for real
// players already in queue. Bot-only LFG groups are always ejected.
void PlayerbotMgr::UpdateLfgAutoJoin(uint32 diff)
{
    if (_bots.empty())
        return;

    _lfgJoinTimer += diff;
    if (_lfgJoinTimer < 2000)
        return;
    _lfgJoinTimer = 0;

    auto isRealPlayer = [](Player* p) -> bool
    {
        return p && p->GetSession() && !p->GetSession()->IsBot();
    };

    auto lfgRoleName = [](uint8 roles) -> char const*
    {
        if (roles & lfg::PLAYER_ROLE_TANK)
            return "TANK";
        if (roles & lfg::PLAYER_ROLE_HEALER)
            return "HEALER";
        if (roles & lfg::PLAYER_ROLE_DAMAGE)
            return "DPS";
        return "NONE";
    };

    // Always eject bots stuck in bot-only LFG dungeon groups.
    for (auto const& pair : _bots)
    {
        WorldSession* session = pair.second;
        Player* bot = session ? session->GetPlayer() : nullptr;
        if (!bot || !bot->IsInWorld())
            continue;

        Group* group = bot->GetGroup();
        if (!group || !group->isLFGGroup())
            continue;

        // Do not tear down mid-proposal / mid-dungeon — the real player may be
        // out-of-world during teleport so GetSource() looks empty (false bot-only).
        lfg::LfgState const gstate = sLFGMgr->GetState(group->GetGUID());
        if (gstate == lfg::LFG_STATE_PROPOSAL || gstate == lfg::LFG_STATE_DUNGEON
            || gstate == lfg::LFG_STATE_ROLECHECK)
            continue;

        bool hasRealPlayer = false;
        for (Group::MemberSlot const& slot : group->GetMemberSlots())
        {
            if (Player* member = ObjectAccessor::FindPlayerInOrOutOfWorld(slot.guid))
            {
                if (isRealPlayer(member))
                {
                    hasRealPlayer = true;
                    break;
                }
            }
            else if (!IsBot(slot.guid))
            {
                // Offline non-bot GUID — treat as a real player who logged out.
                hasRealPlayer = true;
                break;
            }
        }
        if (hasRealPlayer)
            continue;

        sLFGMgr->LeaveLfg(bot->GetGUID());
        if (bot->GetMap() && bot->GetMap()->IsDungeon())
        {
            sLFGMgr->TeleportPlayer(bot, true);
            if (bot->GetSession() && bot->GetSession()->IsBot())
                bot->GetSession()->FinalizeBotTeleport();
        }
        bot->RemoveFromGroup(GROUP_REMOVEMETHOD_LEAVE);
        SF_LOG_INFO("modules", "[mod-playerbots] Ejected bot '%s' from bot-only LFG group.",
            bot->GetName().c_str());
    }

    if (!_joinLfg)
    {
        // Fill disabled: clear solo bot queues so they stay in the open world.
        for (auto const& pair : _bots)
        {
            Player* bot = pair.second ? pair.second->GetPlayer() : nullptr;
            if (!bot || !bot->IsInWorld() || bot->GetGroup())
                continue;
            lfg::LfgState const botState = sLFGMgr->GetState(bot->GetGUID());
            if (botState == lfg::LFG_STATE_NONE || botState == lfg::LFG_STATE_DUNGEON
                || botState == lfg::LFG_STATE_FINISHED_DUNGEON || botState == lfg::LFG_STATE_BOOT)
                continue;
            sLFGMgr->LeaveLfg(bot->GetGUID());
        }
        return;
    }

    struct MasterQueue
    {
        Player* player = nullptr;
        lfg::LfgDungeonSet dungeons;
        uint8 roles = 0;
        uint8 level = 0;
        uint32 team = 0;
    };
    std::vector<MasterQueue> masters;

    {
        SF_SHARED_GUARD readGuard(*HashMapHolder<Player>::GetLock());
        for (auto const& kv : sObjectAccessor->GetPlayers())
        {
            Player* player = kv.second;
            if (!isRealPlayer(player) || !player->IsInWorld())
                continue;

            Group* group = player->GetGroup();
            if (group && !group->isLFGGroup() && !group->IsLeader(player->GetGUID()))
                continue;

            uint64 const stateGuid = group ? group->GetGUID() : player->GetGUID();
            lfg::LfgState state = sLFGMgr->GetState(stateGuid);
            if (state != lfg::LFG_STATE_QUEUED && state != lfg::LFG_STATE_ROLECHECK
                && state != lfg::LFG_STATE_PROPOSAL)
                state = sLFGMgr->GetState(player->GetGUID());
            if (state != lfg::LFG_STATE_QUEUED && state != lfg::LFG_STATE_ROLECHECK
                && state != lfg::LFG_STATE_PROPOSAL)
                continue;

            lfg::LfgDungeonSet const& dList = sLFGMgr->GetSelectedDungeons(player->GetGUID());
            if (dList.empty())
                continue;

            MasterQueue mq;
            mq.player = player;
            mq.dungeons = dList;
            mq.roles = sLFGMgr->GetRoles(player->GetGUID());
            if (!mq.roles && group)
                mq.roles = uint8(group->GetMemberRole(player->GetGUID()));
            mq.level = player->getLevel();
            mq.team = player->GetTeam();
            masters.push_back(mq);
        }
    }

    if (masters.empty())
    {
        // No real players queuing — do not leave bots sitting in solo LFG.
        for (auto const& pair : _bots)
        {
            Player* bot = pair.second ? pair.second->GetPlayer() : nullptr;
            if (!bot || !bot->IsInWorld() || bot->GetGroup())
                continue;
            lfg::LfgState const botState = sLFGMgr->GetState(bot->GetGUID());
            if (botState == lfg::LFG_STATE_NONE || botState == lfg::LFG_STATE_DUNGEON
                || botState == lfg::LFG_STATE_FINISHED_DUNGEON || botState == lfg::LFG_STATE_BOOT)
                continue;
            sLFGMgr->LeaveLfg(bot->GetGUID());
            SF_LOG_DEBUG("modules", "[mod-playerbots] Left solo LFG for bot '%s' (no players queued).",
                bot->GetName().c_str());
        }
        return;
    }

    // 5-man fill by role: 1 tank, 1 healer, 3 dps. Real party members and bots
    // already queued use up slots of their role; bots only join for an open
    // role. Filling by head count alone let surplus roles (two tanks) take every
    // slot, leaving the group one role short with nobody allowed to join.
    // Flooding MaxBots into one queue spawned bot-only proposals that raced
    // eject/teleport and crashed worldserver, so the total stays capped.
    uint32 needTank = 1, needHeal = 1, needDps = 3;
    auto takeSlot = [&](uint8 roles) -> bool
    {
        uint32* slot = nullptr;
        if ((roles & lfg::PLAYER_ROLE_TANK) && needTank)
            slot = &needTank;
        else if ((roles & lfg::PLAYER_ROLE_HEALER) && needHeal)
            slot = &needHeal;
        else if ((roles & lfg::PLAYER_ROLE_DAMAGE) && needDps)
            slot = &needDps;
        if (!slot)
            return false;
        --*slot;
        return true;
    };

    {
        std::unordered_set<uint64> counted;
        auto countReal = [&](Player* member, Group* group)
        {
            uint8 memberRoles = sLFGMgr->GetRoles(member->GetGUID());
            if (!(memberRoles & (lfg::PLAYER_ROLE_TANK | lfg::PLAYER_ROLE_HEALER | lfg::PLAYER_ROLE_DAMAGE)) && group)
                memberRoles = uint8(group->GetMemberRole(member->GetGUID()));
            if (!takeSlot(memberRoles))
                takeSlot(lfg::PLAYER_ROLE_DAMAGE);
        };

        for (MasterQueue const& mq : masters)
        {
            if (Group* group = mq.player->GetGroup())
            {
                for (Group::MemberSlot const& slot : group->GetMemberSlots())
                {
                    if (!counted.insert(slot.guid).second)
                        continue;
                    // Party bots fill their slot the same way real members do
                    Player* member = ObjectAccessor::FindPlayerInOrOutOfWorld(slot.guid);
                    if (member)
                        countReal(member, group);
                    else
                        takeSlot(uint8(group->GetMemberRole(slot.guid)));
                }
            }
            else if (counted.insert(mq.player->GetGUID()).second)
                countReal(mq.player, nullptr);
        }
    }

    // Solo bots already queued keep their slot; surplus ones (role already
    // covered) leave so they can't hold up or split the group.
    uint32 botsAlreadyQueued = 0;
    for (auto const& pair : _bots)
    {
        Player* bot = pair.second ? pair.second->GetPlayer() : nullptr;
        if (!bot || !bot->IsInWorld() || bot->GetGroup())
            continue;
        lfg::LfgState const st = sLFGMgr->GetState(bot->GetGUID());
        if (st != lfg::LFG_STATE_QUEUED && st != lfg::LFG_STATE_ROLECHECK && st != lfg::LFG_STATE_PROPOSAL)
            continue;
        uint8 const queuedRoles = sLFGMgr->GetRoles(bot->GetGUID());
        if (takeSlot(queuedRoles))
        {
            ++botsAlreadyQueued;
            continue;
        }
        if (st == lfg::LFG_STATE_QUEUED)
        {
            sLFGMgr->LeaveLfg(bot->GetGUID());
            SF_LOG_INFO("modules", "[mod-playerbots] Bot '%s' left LFG: %s role already covered.",
                bot->GetName().c_str(), lfgRoleName(queuedRoles));
        }
    }

    uint32 const openSlots = needTank + needHeal + needDps;
    uint32 const capLeft = (_joinLfgMaxBots > botsAlreadyQueued) ? (_joinLfgMaxBots - botsAlreadyQueued) : 0u;
    uint32 const slotsLeft = std::min(openSlots, capLeft);
    uint32 botsJoined = 0;
    if (!slotsLeft)
        return;

    for (auto const& pair : _bots)
    {
        if (botsJoined >= slotsLeft)
            break;

        Player* bot = pair.second ? pair.second->GetPlayer() : nullptr;
        if (!bot || !bot->IsInWorld() || !bot->GetSession())
            continue;
        if (bot->GetGroup() || bot->isDead() || bot->IsBeingTeleported())
            continue;
        if (bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue())
            continue;
        if (bot->getLevel() < 15)
            continue;

        lfg::LfgState const botState = sLFGMgr->GetState(bot->GetGUID());
        if (botState != lfg::LFG_STATE_NONE)
            continue;

        PlayerbotAI* ai = GetBotAI(bot);
        if (!ai)
            continue;

        uint8 const botRole = ai->ComputeLfgRole();
        if ((botRole & lfg::PLAYER_ROLE_TANK) && !needTank)
            continue;
        if ((botRole & lfg::PLAYER_ROLE_HEALER) && !needHeal)
            continue;
        if ((botRole & lfg::PLAYER_ROLE_DAMAGE) && !needDps)
            continue;

        MasterQueue const* matched = nullptr;
        lfg::LfgDungeonSet botDungeons;
        for (MasterQueue const& mq : masters)
        {
            if (bot->GetTeam() != mq.team)
                continue;
            if (std::abs(int(bot->getLevel()) - int(mq.level)) > int(_joinLfgLevelRange))
                continue;

            for (uint32 dungeonId : mq.dungeons)
            {
                lfg::LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(dungeonId);
                if (!dungeon)
                    dungeon = sLFGMgr->GetLFGDungeon(dungeonId & 0x00FFFFFFu);
                if (!dungeon)
                    continue;
                if (bot->getLevel() < dungeon->minlevel || bot->getLevel() > dungeon->maxlevel)
                    continue;
                botDungeons.insert(dungeonId);
            }
            if (!botDungeons.empty())
            {
                matched = &mq;
                break;
            }
        }
        if (!matched || botDungeons.empty())
            continue;

        if (!bot->GetSession()->HasPermission(rbac::RBAC_PERM_JOIN_DUNGEON_FINDER))
        {
            SF_LOG_DEBUG("modules",
                "[mod-playerbots] Bot '%s' skipped LFG join (missing dungeon finder permission).",
                bot->GetName().c_str());
            continue;
        }

        std::string const comment = "playerbot";
        sLFGMgr->JoinLfg(bot, botRole, botDungeons, comment);
        takeSlot(botRole);
        ++botsJoined;

        char const* dungeonName = "dungeon";
        if (lfg::LFGDungeonData const* first = sLFGMgr->GetLFGDungeon(*botDungeons.begin()))
            dungeonName = first->name.c_str();
        else if (lfg::LFGDungeonData const* first = sLFGMgr->GetLFGDungeon(*botDungeons.begin() & 0x00FFFFFFu))
            dungeonName = first->name.c_str();

        SF_LOG_INFO("modules",
            "[mod-playerbots] Bot '%s' (lvl %u) joining LFG as %s for %s (%u selected) — filling %s (%s).",
            bot->GetName().c_str(), bot->getLevel(), lfgRoleName(botRole),
            dungeonName, uint32(botDungeons.size()),
            matched->player->GetName().c_str(), lfgRoleName(matched->roles));
    }

    if (botsJoined < slotsLeft)
    {
        MasterQueue const& mq = masters.front();
        SF_LOG_INFO("modules",
            "[mod-playerbots] LFG fill: %u player(s) queued (e.g. %s as %s, lvl %u); no eligible bot for "
            "%u tank, %u healer, %u dps slot(s) (need level±%u and a matching dungeon).",
            uint32(masters.size()), mq.player->GetName().c_str(), lfgRoleName(mq.roles), mq.level,
            needTank, needHeal, needDps, _joinLfgLevelRange);
    }
}

bool PlayerbotMgr::AddBot(uint64 characterGuid, std::string* errorOut)
{
    return SpawnBot(characterGuid, false, errorOut);
}

bool PlayerbotMgr::SpawnBot(uint64 characterGuid, bool isRandom, std::string* errorOut,
    uint32 accountIdOverride)
{
    auto fail = [errorOut](char const* reason) -> bool
    {
        if (errorOut)
            *errorOut = reason;
        return false;
    };

    if (!_enabled)
        return fail("Playerbots module is disabled (set Playerbots.Enable = 1).");

    if (!characterGuid)
        return fail("Invalid character.");

    if (IsBot(characterGuid))
        return fail("That character is already an active bot.");

    if (_pendingLoginGuids.find(characterGuid) != _pendingLoginGuids.end())
        return fail("That character is already logging in.");

    // Refuse if the character is already in the world (real player or otherwise);
    // a second Player with the same GUID would collide in the object accessor.
    if (ObjectAccessor::FindPlayer(characterGuid))
        return fail("That character is already online.");

    uint32 accountId = accountIdOverride;
    if (!accountId)
        accountId = sObjectMgr->GetPlayerAccountIdByGUID(characterGuid);
    if (!accountId)
        return fail("Could not resolve the character's account.");

    uint8 expansion = uint8(sWorld->getIntConfig(WorldIntConfigs::CONFIG_EXPANSION));

    // Bots always run at player security regardless of the owning account's level.
    WorldSession* botSession = new WorldSession(accountId, nullptr, AccountTypes::SEC_PLAYER, expansion,
        0, LOCALE_enUS, 0, false, false, false, false, std::string(), SessionKey{});
    botSession->SetBot(true);
    uint32 const botRealm = ResolveBotVirtualRealm();
    botSession->SetVirtualRealmID(botRealm);

    if (!botSession->LoginBotCharacter(characterGuid))
    {
        delete botSession;
        return fail("Failed to load the character into the world.");
    }

    PendingBotLogin pending;
    pending.session = botSession;
    pending.characterGuid = characterGuid;
    pending.isRandom = isRandom;
    pending.botRealm = botRealm;
    FinishSpawnBot(pending);
    if (_bots.find(characterGuid) == _bots.end())
        return fail("Failed to load the character into the world.");

    return true;
}

uint32 PlayerbotMgr::ResolveBotVirtualRealm()
{
    if (_botVirtualRealm && _botVirtualRealm != uint32(-1))
        return _botVirtualRealm;

    {
        SF_SHARED_GUARD readGuard(*HashMapHolder<Player>::GetLock());
        for (auto const& kv : sObjectAccessor->GetPlayers())
        {
            Player* p = kv.second;
            if (!p || !p->GetSession() || p->GetSession()->IsBot())
                continue;
            uint32 r = p->GetSession()->GetVirtualRealmID();
            if (r && r != uint32(-1))
            {
                _botVirtualRealm = r;
                return _botVirtualRealm;
            }
        }
    }

    _botVirtualRealm = (realmID && realmID != uint32(-1)) ? realmID : 1;
    return _botVirtualRealm;
}

bool PlayerbotMgr::BeginSpawnBot(uint64 characterGuid, bool isRandom, uint32 accountId,
    std::string* errorOut)
{
    auto fail = [errorOut](char const* reason) -> bool
    {
        if (errorOut)
            *errorOut = reason;
        return false;
    };

    if (!_enabled)
        return fail("Playerbots module is disabled (set Playerbots.Enable = 1).");
    if (!characterGuid)
        return fail("Invalid character.");
    if (IsBot(characterGuid))
        return fail("That character is already an active bot.");
    if (_pendingLoginGuids.find(characterGuid) != _pendingLoginGuids.end())
        return fail("That character is already logging in.");
    if (ObjectAccessor::FindPlayer(characterGuid))
        return fail("That character is already online.");
    if (!accountId)
        accountId = sObjectMgr->GetPlayerAccountIdByGUID(characterGuid);
    if (!accountId)
        return fail("Could not resolve the character's account.");

    uint8 expansion = uint8(sWorld->getIntConfig(WorldIntConfigs::CONFIG_EXPANSION));
    WorldSession* botSession = new WorldSession(accountId, nullptr, AccountTypes::SEC_PLAYER, expansion,
        0, LOCALE_enUS, 0, false, false, false, false, std::string(), SessionKey{});
    botSession->SetBot(true);
    uint32 const botRealm = ResolveBotVirtualRealm();
    botSession->SetVirtualRealmID(botRealm);

    if (!botSession->BeginBotCharacterLogin(characterGuid))
    {
        delete botSession;
        return fail("Failed to start character login.");
    }

    PendingBotLogin pending;
    pending.session = botSession;
    pending.characterGuid = characterGuid;
    pending.isRandom = isRandom;
    pending.botRealm = botRealm;
    pending.startedMs = getMSTime();
    _pendingLogins.push_back(pending);
    _pendingLoginGuids.insert(characterGuid);
    return true;
}

void PlayerbotMgr::ProcessPendingLogins()
{
    if (_pendingLogins.empty())
        return;

    uint32 completed = 0;
    for (auto it = _pendingLogins.begin(); it != _pendingLogins.end();)
    {
        PendingBotLogin& pending = *it;
        if (!pending.session)
        {
            _pendingLoginGuids.erase(pending.characterGuid);
            it = _pendingLogins.erase(it);
            continue;
        }

        // Abandon stalled logins so a wedged DB query cannot fill the pipeline.
        uint32 const ageMs = GetMSTimeDiffToNow(pending.startedMs);
        if (!pending.discard && ageMs > 30000)
        {
            SF_LOG_ERROR("modules",
                "[mod-playerbots] Async bot login timed out (GUID %u) after %u ms.",
                GUID_LOPART(pending.characterGuid), ageMs);
            pending.discard = true;
        }

        // Limit world-thread LoadFromDB work per tick (discard polls are cheap).
        if (!pending.discard && completed >= _loginsPerTick)
        {
            ++it;
            continue;
        }

        WorldSession::BotLoginPollStatus const status =
            pending.session->PollBotCharacterLogin(pending.discard);

        if (status == WorldSession::BotLoginPollStatus::Pending)
        {
            ++it;
            continue;
        }

        if (status == WorldSession::BotLoginPollStatus::Success && !pending.discard)
        {
            FinishSpawnBot(pending);
            if (_bots.find(pending.characterGuid) != _bots.end())
            {
                ++completed;
                SF_LOG_INFO("modules", "[mod-playerbots] Random bot pool: %u/%u online (%u login pending).",
                    uint32(_randomBots.size()), _maxRandomBots,
                    uint32(_pendingLogins.size() > 0 ? _pendingLogins.size() - 1 : 0));
            }
        }
        else
        {
            AbortPendingLogin(pending);
        }

        _pendingLoginGuids.erase(pending.characterGuid);
        it = _pendingLogins.erase(it);
    }
}

void PlayerbotMgr::AbortPendingLogin(PendingBotLogin& pending)
{
    if (!pending.session)
        return;
    delete pending.session;
    pending.session = nullptr;
}

void PlayerbotMgr::FinishSpawnBot(PendingBotLogin& pending)
{
    WorldSession* botSession = pending.session;
    uint64 const characterGuid = pending.characterGuid;
    bool const isRandom = pending.isRandom;
    uint32 const botRealm = pending.botRealm;
    if (!botSession)
        return;

    // Bots have no client to ACK login teleports (instance eject / homebind).
    if (Player* loggedIn = botSession->GetPlayer())
    {
        if (loggedIn->IsBeingTeleported())
            botSession->FinalizeBotTeleport();
        botSession->SetVirtualRealmID(botRealm);
        loggedIn->SetUInt32Value(PLAYER_FIELD_VIRTUAL_PLAYER_REALM, botRealm);
    }

    if (!botSession->GetPlayer() || !botSession->GetPlayer()->IsInWorld())
    {
        AbortPendingLogin(pending);
        return;
    }

    _bots[characterGuid] = botSession;
    if (isRandom)
        _randomBots.insert(characterGuid);

    if (Player* bot = botSession->GetPlayer())
    {
        // Create AI before gear init so AfterInitRelocate / role strategies apply.
        _ai[characterGuid] = new PlayerbotAI(bot);
        if (isRandom)
        {
            if (PlayerbotAI* ai = _ai[characterGuid])
            {
                ai->GetStrategyEngine().ChangeStrategy("+quests,+grind,-follow", BotState::NonCombat);
                ai->GetStrategyEngine().Add("grind", BotState::Combat);
                ai->SyncFlagsFromStrategies();
            }
        }

        // Deferred create (InitOnCreate=0): queue gear/spells/teleport so mass
        // login stays fast (Init.PerTick paces the heavy work).
        auto deferred = _deferredInits.find(characterGuid);
        if (deferred != _deferredInits.end())
        {
            PendingBotInit const& d = deferred->second;
            UpsertInitQueueJob(characterGuid, d.role, d.specId, -1, false,
                _autoTeleportOnInit, false);
            _deferredInits.erase(deferred);
            SF_LOG_INFO("modules",
                "[mod-playerbots] Queued first-login init+teleport for '%s' (GUID %u).",
                bot->GetName().c_str(), GUID_LOPART(characterGuid));
        }
        else if (BotNeedsGearInit(bot))
        {
            UpsertInitQueueJob(characterGuid, -1, 0, -1, false, _autoTeleportOnInit, false);
            SF_LOG_INFO("modules",
                "[mod-playerbots] Queued ungeared init+teleport for '%s' (GUID %u).",
                bot->GetName().c_str(), GUID_LOPART(characterGuid));
        }

        char const* raceName = GetRaceName(bot->getRace(), LOCALE_enUS);
        char const* className = GetClassName(bot->getClass(), LOCALE_enUS);
        SF_LOG_INFO("modules",
            "[mod-playerbots] Bot '%s' entered the world (GUID %u, level %u %s %s%s).",
            bot->GetName().c_str(), GUID_LOPART(characterGuid), bot->getLevel(),
            raceName ? raceName : "?", className ? className : "?",
            isRandom ? ", random" : "");
    }

    // Ownership transferred to _bots; clear so Abort won't delete.
    pending.session = nullptr;
}

void PlayerbotMgr::UpdateBotAI(Player* bot, uint32 diff)
{
    if (!bot)
        return;

    auto it = _ai.find(bot->GetGUID());
    if (it == _ai.end() || !it->second)
        return;

    it->second->Rebind(bot);
    it->second->UpdateAI(diff);
}

void PlayerbotMgr::OnPlayerLogout(Player* player)
{
    if (!player)
        return;
    uint64 const guid = player->GetGUID();
    _selfBots.erase(guid);
    DestroyBotAI(guid);
}

void PlayerbotMgr::SyncNearbyBotVisibility(Player* viewer, uint32 diff)
{
    if (!_enabled || !viewer || !viewer->IsInWorld() || !viewer->GetSession())
        return;
    if (viewer->GetSession()->IsBot())
        return;

    uint32& timer = _viewerVisTimers[viewer->GetGUID()];
    timer += diff;
    if (timer < 500)
        return;
    timer = 0;

    Map* map = viewer->GetMap();
    if (!map)
        return;

    float const range = map->GetVisibilityRange();
    uint32 const viewerRealm = viewer->GetSession()->GetVirtualRealmID();
    for (auto const& pair : _bots)
    {
        Player* bot = pair.second ? pair.second->GetPlayer() : nullptr;
        if (!bot || !bot->IsInWorld() || bot->GetMap() != map)
            continue;

        // Align bot session VR with real players (name query / who). Never write
        // PLAYER_FIELD_VIRTUAL_PLAYER_REALM here — update-field spam blanked /who.
        if (viewerRealm && viewerRealm != uint32(-1)
            && pair.second->GetVirtualRealmID() != viewerRealm)
            pair.second->SetVirtualRealmID(viewerRealm);

        if (viewer->GetDistance(bot) > range)
            continue;

        if (PlayerbotAI* ai = GetBotAI(bot->GetGUID()))
            ai->SyncPhaseWithMaster(viewer);
    }
}

void PlayerbotMgr::HandleBotWhisper(Player* from, Player* bot, std::string const& msg)
{
    if (!_enabled || !from || !bot)
        return;
    if (from->GetSession() && from->GetSession()->IsBot())
        return;

    auto it = _ai.find(bot->GetGUID());
    if (it == _ai.end() || !it->second)
        return;

    it->second->HandleChatCommand(from, msg);
}

void PlayerbotMgr::HandleBotGroupChat(Player* from, Group* group, std::string const& msg)
{
    if (!_enabled || !from || !group)
        return;
    if (from->GetSession() && from->GetSession()->IsBot())
        return;

    // Optional "@tank "/ "@dps "/ "@heal "/ "@ranged " prefix filters who hears the order.
    std::string text = msg;
    while (!text.empty() && text[0] == ' ')
        text.erase(text.begin());

    std::string filter;
    if (!text.empty() && text[0] == '@')
    {
        std::string::size_type space = text.find(' ');
        if (space != std::string::npos)
        {
            filter = text.substr(1, space - 1);
            text = text.substr(space + 1);
            while (!text.empty() && text[0] == ' ')
                text.erase(text.begin());
            std::transform(filter.begin(), filter.end(), filter.begin(),
                [](unsigned char c) { return char(std::tolower(c)); });
            if (filter == "healer")
                filter = "heal";
        }
    }

    if (text.empty())
        return;

    // Strategy queries/changes always want a whisper reply from each bot.
    bool const strategyCmd = (text.size() >= 2
        && (text.compare(0, 2, "co") == 0 || text.compare(0, 2, "nc") == 0)
        && (text.size() == 2 || text[2] == ' ' || text[2] == '?'));

    // "@tank attack" strips to filter=tank + text=attack. Map that to tank-only
    // engage and make everyone else hold assist so melee DPS don't pile in.
    bool const tankOnlyPull = (filter == "tank" && (text == "attack" || text == "tank attack"))
        || (filter.empty() && text == "tank attack");
    bool const dpsOnlyPull = (filter == "dps" && (text == "attack" || text == "dps attack"))
        || (filter.empty() && text == "dps attack");

    auto deliver = [&](Player* member)
    {
        if (!member)
            return;
        auto it = _ai.find(member->GetGUID());
        if (it == _ai.end() || !it->second)
            return;

        if (tankOnlyPull)
        {
            if (it->second->MatchesRoleFilter("tank"))
                it->second->HandleChatCommand(from, "tank attack", false);
            else
                it->second->SetHoldAssist(true);
            return;
        }
        if (dpsOnlyPull)
        {
            if (it->second->MatchesRoleFilter("dps"))
                it->second->HandleChatCommand(from, "dps attack", false);
            return;
        }

        if (!filter.empty() && !it->second->MatchesRoleFilter(filter))
            return;
        // Party orders are silent except co/nc (bots whisper their strategy state).
        it->second->HandleChatCommand(from, text, strategyCmd);
    };

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        // Socket bots and self-bots (including the issuer for co/nc on yourself).
        if (!member || !HasBotAI(member->GetGUID()))
            continue;
        deliver(member);
    }
}

bool PlayerbotMgr::AttachSelfBot(Player* player, std::string* errorOut)
{
    auto fail = [&](char const* msg) -> bool
    {
        if (errorOut)
            *errorOut = msg;
        return false;
    };

    if (!_enabled)
        return fail("Playerbots module is disabled.");
    if (!player || !player->IsInWorld())
        return fail("You must be in the world.");
    if (IsBot(player->GetGUID()))
        return fail("This character is already a full bot session.");
    if (IsSelfBot(player->GetGUID()))
        return fail("Self-bot AI is already attached.");

    uint64 guid = player->GetGUID();
    DestroyBotAI(guid); // safety if a stale AI pointer lingered
    _ai[guid] = new PlayerbotAI(player, true);
    _selfBots.insert(guid);
    return true;
}

bool PlayerbotMgr::DetachSelfBot(Player* player)
{
    if (!player)
        return false;

    uint64 guid = player->GetGUID();
    if (!IsSelfBot(guid))
        return false;

    _selfBots.erase(guid);
    DestroyBotAI(guid);
    return true;
}

bool PlayerbotMgr::ToggleSelfBot(Player* player, std::string* report)
{
    if (!player)
        return false;

    if (IsSelfBot(player->GetGUID()))
    {
        DetachSelfBot(player);
        if (report)
            *report = "Self-bot AI detached. You control combat again.";
        return true;
    }

    std::string error;
    if (!AttachSelfBot(player, &error))
    {
        if (report)
            *report = error;
        return false;
    }

    if (report)
        *report = "Self-bot AI attached. You move; the AI casts in combat. Use .playerbots self again to detach.";
    return true;
}

bool PlayerbotMgr::RemoveBot(uint64 characterGuid)
{
    auto it = _bots.find(characterGuid);
    if (it == _bots.end())
        return false;

    WorldSession* botSession = it->second;
    _bots.erase(it);
    _randomBots.erase(characterGuid);
    DestroyBotAI(characterGuid);

    // ~WorldSession logs the player out (save) if still attached and cleans up.
    delete botSession;
    return true;
}

void PlayerbotMgr::LogoutAllBots()
{
    for (PendingBotLogin& pending : _pendingLogins)
    {
        if (pending.session)
            pending.session->PollBotCharacterLogin(true);
        AbortPendingLogin(pending);
    }
    _pendingLogins.clear();
    _pendingLoginGuids.clear();
    // Keep _deferredInits — those chars may still need first-login gear after
    // a logout/restart cycle within the same world session. Wipe clears them.

    for (auto& pair : _ai)
        delete pair.second;
    _ai.clear();

    for (auto& pair : _bots)
        delete pair.second;

    _bots.clear();
    _randomBots.clear();
    _selfBots.clear();
}

bool PlayerbotMgr::CanDeleteBotAccounts(std::string* errorOut) const
{
    auto fail = [&](char const* msg) -> bool
    {
        if (errorOut)
            *errorOut = msg;
        return false;
    };

    if (_accountPrefix.empty())
        return fail("Playerbots.RandomBots.AccountPrefix is empty; refusing wipe.");
    if (_accountPrefix.size() < 3)
        return fail("AccountPrefix is shorter than 3 characters; refusing wipe.");

    // Guard against accidentally wiping the whole account table.
    std::string upper = _accountPrefix;
    std::transform(upper.begin(), upper.end(), upper.begin(),
        [](unsigned char c) { return char(std::toupper(c)); });
    if (upper == "ADMIN" || upper == "GM" || upper == "PLAYER" || upper == "ACCOUNT")
        return fail("AccountPrefix looks unsafe; refusing wipe.");

    return true;
}

uint32 PlayerbotMgr::DeleteBotAccounts(std::string* report)
{
    std::string error;
    if (!CanDeleteBotAccounts(&error))
    {
        SF_LOG_ERROR("modules", "[mod-playerbots] %s", error.c_str());
        if (report)
            *report = error;
        return 0;
    }

    // Drop live bot sessions first so DeleteAccount does not race logouts.
    LogoutAllBots();

    QueryResult accounts = LoginDatabase.PQuery(
        "SELECT id, username FROM account WHERE username LIKE '%s%%'", _accountPrefix.c_str());
    if (!accounts)
    {
        std::string msg = "No bot accounts found for prefix '" + _accountPrefix + "'.";
        SF_LOG_INFO("modules", "[mod-playerbots] %s", msg.c_str());
        if (report)
            *report = msg;
        _candidates.clear();
        _candidatesLoaded = true;
        return 0;
    }

    uint32 deleted = 0;
    do
    {
        uint32 const accountId = (*accounts)[0].GetUInt32();
        std::string const username = (*accounts)[1].GetString();
        AccountOpResult res = AccountMgr::DeleteAccount(accountId);
        if (res == AccountOpResult::AOR_OK)
        {
            ++deleted;
            SF_LOG_INFO("modules", "[mod-playerbots] Deleted bot account '%s' (id %u).",
                username.c_str(), accountId);
        }
        else
        {
            SF_LOG_ERROR("modules", "[mod-playerbots] Failed to delete bot account '%s' (id %u, error %u).",
                username.c_str(), accountId, uint32(res));
        }
    } while (accounts->NextRow());

    // Account and character deletes are queued asynchronously. Reloading
    // candidates on the next tick would still see those rows and start logins
    // for characters that are about to disappear.
    _candidates.clear();
    _candidatesLoaded = true;
    _deferredInits.clear();
    _initQueue.clear();
    _initQueueBatchTotal = 0;
    _initQueueBatchDone = 0;

    std::ostringstream ss;
    ss << "Deleted " << deleted << " bot account(s) with prefix '" << _accountPrefix
       << "'. Set Playerbots.DeleteRandomBotAccounts = 0 before the next restart, "
       << "then run .playerbots create (or enable AutoCreate.OnStartup).";
    SF_LOG_INFO("modules", "[mod-playerbots] %s", ss.str().c_str());
    if (report)
        *report = ss.str();
    return deleted;
}

void PlayerbotMgr::DestroyBotAI(uint64 characterGuid)
{
    auto it = _ai.find(characterGuid);
    if (it == _ai.end())
        return;

    delete it->second;
    _ai.erase(it);
}

PlayerbotAI* PlayerbotMgr::GetBotAI(uint64 characterGuid) const
{
    auto it = _ai.find(characterGuid);
    return it != _ai.end() ? it->second : nullptr;
}

PlayerbotAI* PlayerbotMgr::GetBotAI(Player* bot) const
{
    return bot ? GetBotAI(bot->GetGUID()) : nullptr;
}

void PlayerbotMgr::GetBotGuids(std::vector<uint64>& out) const
{
    out.reserve(_bots.size());
    for (auto const& pair : _bots)
        out.push_back(pair.first);
}

// ---------------------------------------------------------------------------
// Auto-creation
// ---------------------------------------------------------------------------

namespace
{
    enum BotRole { BOT_ROLE_TANK, BOT_ROLE_HEALER, BOT_ROLE_DPS };

    char const* RoleName(BotRole role)
    {
        switch (role)
        {
            case BOT_ROLE_TANK:   return "tank";
            case BOT_ROLE_HEALER: return "healer";
            default:              return "dps";
        }
    }

    char const* SpecName(uint32 specId)
    {
        switch (specId)
        {
            case SPEC_MAGE_ARCANE:            return "Arcane";
            case SPEC_MAGE_FIRE:              return "Fire";
            case SPEC_MAGE_FROST:             return "Frost";
            case SPEC_PALADIN_HOLY:           return "Holy";
            case SPEC_PALADIN_PROTECTION:     return "Protection";
            case SPEC_PALADIN_RETRIBUTION:    return "Retribution";
            case SPEC_WARRIOR_ARMS:           return "Arms";
            case SPEC_WARRIOR_FURY:           return "Fury";
            case SPEC_WARRIOR_PROTECTION:     return "Protection";
            case SPEC_DRUID_BALANCE:          return "Balance";
            case SPEC_DRUID_FERAL:            return "Feral";
            case SPEC_DRUID_GUARDIAN:         return "Guardian";
            case SPEC_DRUID_RESTORATION:      return "Restoration";
            case SPEC_DEATH_KNIGHT_BLOOD:     return "Blood";
            case SPEC_DEATH_KNIGHT_FROST:     return "Frost";
            case SPEC_DEATH_KNIGHT_UNHOLY:    return "Unholy";
            case SPEC_HUNTER_BEAST_MASTERY:   return "Beast Mastery";
            case SPEC_HUNTER_MARKSMANSHIP:    return "Marksmanship";
            case SPEC_HUNTER_SURVIVAL:        return "Survival";
            case SPEC_PRIEST_DISCIPLINE:      return "Discipline";
            case SPEC_PRIEST_HOLY:            return "Holy";
            case SPEC_PRIEST_SHADOW:          return "Shadow";
            case SPEC_ROGUE_ASSASSINATION:    return "Assassination";
            case SPEC_ROGUE_COMBAT:           return "Combat";
            case SPEC_ROGUE_SUBTLETY:         return "Subtlety";
            case SPEC_SHAMAN_ELEMENTAL:       return "Elemental";
            case SPEC_SHAMAN_ENHANCEMENT:     return "Enhancement";
            case SPEC_SHAMAN_RESTORATION:     return "Restoration";
            case SPEC_WARLOCK_AFFLICTION:     return "Affliction";
            case SPEC_WARLOCK_DEMONOLOGY:     return "Demonology";
            case SPEC_WARLOCK_DESTRUCTION:    return "Destruction";
            case SPEC_MONK_BREWMASTER:        return "Brewmaster";
            case SPEC_MONK_WINDWALKER:        return "Windwalker";
            case SPEC_MONK_MISTWEAVER:        return "Mistweaver";
            default:                         return "None";
        }
    }

    char const* QualityName(int quality)
    {
        switch (quality)
        {
            case ITEM_QUALITY_POOR:      return "poor";
            case ITEM_QUALITY_NORMAL:    return "common";
            case ITEM_QUALITY_UNCOMMON:  return "uncommon";
            case ITEM_QUALITY_RARE:      return "rare";
            case ITEM_QUALITY_EPIC:      return "epic";
            case ITEM_QUALITY_LEGENDARY: return "legendary";
            default:                     return "unknown";
        }
    }

    char const* SafeRaceName(uint8 race)
    {
        char const* name = GetRaceName(race, LOCALE_enUS);
        return name ? name : "?";
    }

    char const* SafeClassName(uint8 cls)
    {
        char const* name = GetClassName(cls, LOCALE_enUS);
        return name ? name : "?";
    }

    // Playable races per faction. Neutral Pandaren are excluded so offline
    // creation never has to resolve the neutral starting faction.
    uint8 const AllianceRaces[] = { RACE_HUMAN, RACE_DWARF, RACE_NIGHTELF, RACE_GNOME, RACE_DRAENEI, RACE_WORGEN };
    uint8 const HordeRaces[]    = { RACE_ORC, RACE_UNDEAD_PLAYER, RACE_TAUREN, RACE_TROLL, RACE_BLOODELF, RACE_GOBLIN };

    // Classes able to fill each role (Death Knight excluded: it has a special
    // starting experience and level requirement that offline creation skips).
    uint8 const TankClasses[]   = { CLASS_WARRIOR, CLASS_PALADIN, CLASS_DRUID, CLASS_MONK };
    uint8 const HealerClasses[] = { CLASS_PALADIN, CLASS_PRIEST, CLASS_SHAMAN, CLASS_DRUID, CLASS_MONK };
    uint8 const DpsClasses[]    = { CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST,
                                    CLASS_SHAMAN, CLASS_MAGE, CLASS_WARLOCK, CLASS_MONK, CLASS_DRUID };

    uint32 RollPct() { return uint32(std::rand() % 100); }   // 0..99

    template <size_t N>
    uint8 PickRandom(uint8 const (&arr)[N]) { return arr[std::rand() % N]; }

    uint8 PickClassForRole(BotRole role)
    {
        switch (role)
        {
            case BOT_ROLE_TANK:   return PickRandom(TankClasses);
            case BOT_ROLE_HEALER: return PickRandom(HealerClasses);
            default:              return PickRandom(DpsClasses);
        }
    }

    // Specialization tab index (matches the in-game spec order) for a class in a
    // given role. Classes that cannot fill the role fall back to a damage spec.
    uint8 SpecTabForRole(uint8 cls, BotRole role)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return role == BOT_ROLE_TANK ? 2 : 0;                       // Prot / Arms
            case CLASS_PALADIN:      return role == BOT_ROLE_TANK ? 1 : (role == BOT_ROLE_HEALER ? 0 : 2); // Prot / Holy / Ret
            case CLASS_DEATH_KNIGHT: return role == BOT_ROLE_TANK ? 0 : 1;                       // Blood / Frost
            case CLASS_PRIEST:       return role == BOT_ROLE_HEALER ? 0 : 2;                     // Disc / Shadow
            case CLASS_SHAMAN:       return role == BOT_ROLE_HEALER ? 2 : 0;                     // Resto / Elemental
            case CLASS_MONK:         return role == BOT_ROLE_TANK ? 0 : (role == BOT_ROLE_HEALER ? 1 : 2); // Brewmaster / Mistweaver / Windwalker
            case CLASS_DRUID:        return role == BOT_ROLE_TANK ? 2 : (role == BOT_ROLE_HEALER ? 3 : 0); // Guardian / Resto / Balance
            case CLASS_HUNTER:       return 0;                                                   // Beast Mastery
            case CLASS_ROGUE:        return 0;                                                   // Assassination
            case CLASS_MAGE:         return 2;                                                   // Frost
            case CLASS_WARLOCK:      return 0;                                                   // Affliction
            default:                 return 0;
        }
    }

    // ChrSpecialization id for a class/role pair (0 if unavailable).
    uint32 SpecIdForRole(uint8 cls, BotRole role)
    {
        uint32 const* specs = GetClassSpecializations(cls);
        if (!specs)
            return 0;
        return specs[SpecTabForRole(cls, role)];
    }

    // Returns a race that forms a valid race/class pair for the faction, or 0.
    uint8 PickRaceForClassFaction(uint8 cls, bool alliance)
    {
        std::vector<uint8> races(alliance ? std::begin(AllianceRaces) : std::begin(HordeRaces),
                                 alliance ? std::end(AllianceRaces) : std::end(HordeRaces));

        // Fisher-Yates shuffle so the chosen race is not biased by list order.
        for (size_t i = races.size(); i > 1; --i)
            std::swap(races[i - 1], races[std::rand() % i]);

        for (uint8 r : races)
            if (sObjectMgr->GetPlayerInfo(r, cls))
                return r;
        return 0;
    }

    std::string GenerateBotName()
    {
        static char const consonants[] = "bcdfghjklmnprstvw";
        static char const vowels[]     = "aeiou";

        uint32 len = 4 + (std::rand() % 5);                  // 4..8 characters
        std::string name;
        bool cons = true;
        for (uint32 i = 0; i < len; ++i)
        {
            if (cons)
                name += consonants[std::rand() % (sizeof(consonants) - 1)];
            else
                name += vowels[std::rand() % (sizeof(vowels) - 1)];
            cons = !cons;
        }
        name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
        return name;
    }

    // -----------------------------------------------------------------------
    // Gear selection
    // -----------------------------------------------------------------------

    // Armor type the class wears in MoP (Cata+ taught this from level 1).
    uint8 ArmorTypeForClass(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
            case CLASS_DEATH_KNIGHT: return ITEM_SUBCLASS_ARMOR_PLATE;
            case CLASS_HUNTER:
            case CLASS_SHAMAN:       return ITEM_SUBCLASS_ARMOR_MAIL;
            case CLASS_ROGUE:
            case CLASS_DRUID:
            case CLASS_MONK:         return ITEM_SUBCLASS_ARMOR_LEATHER;
            default:                 return ITEM_SUBCLASS_ARMOR_CLOTH; // priest/mage/warlock
        }
    }

    // Primary stat the spec wants, used to bias item picks toward useful gear.
    uint32 PrimaryStatForClassRole(uint8 cls, BotRole role, uint32 specId = 0)
    {
        // Spec-aware overrides for hybrids with multiple DPS trees.
        switch (specId)
        {
            case SPEC_SHAMAN_ENHANCEMENT:
            case SPEC_DRUID_FERAL:
            case SPEC_DRUID_GUARDIAN:
            case SPEC_HUNTER_BEAST_MASTERY:
            case SPEC_HUNTER_MARKSMANSHIP:
            case SPEC_HUNTER_SURVIVAL:
            case SPEC_ROGUE_ASSASSINATION:
            case SPEC_ROGUE_COMBAT:
            case SPEC_ROGUE_SUBTLETY:
            case SPEC_MONK_WINDWALKER:
            case SPEC_MONK_BREWMASTER:
                return ITEM_MOD_AGILITY;
            case SPEC_SHAMAN_ELEMENTAL:
            case SPEC_SHAMAN_RESTORATION:
            case SPEC_DRUID_BALANCE:
            case SPEC_DRUID_RESTORATION:
            case SPEC_MONK_MISTWEAVER:
                return ITEM_MOD_INTELLECT;
            default:
                break;
        }

        switch (cls)
        {
            case CLASS_WARRIOR:
            case CLASS_DEATH_KNIGHT: return ITEM_MOD_STRENGTH;
            case CLASS_PALADIN:      return role == BOT_ROLE_HEALER ? ITEM_MOD_INTELLECT : ITEM_MOD_STRENGTH;
            case CLASS_HUNTER:
            case CLASS_ROGUE:        return ITEM_MOD_AGILITY;
            case CLASS_MONK:         return role == BOT_ROLE_HEALER ? ITEM_MOD_INTELLECT : ITEM_MOD_AGILITY;
            case CLASS_DRUID:        return role == BOT_ROLE_TANK ? ITEM_MOD_AGILITY
                                         : (role == BOT_ROLE_DPS ? ITEM_MOD_INTELLECT : ITEM_MOD_INTELLECT);
            case CLASS_SHAMAN:       return role == BOT_ROLE_DPS ? ITEM_MOD_INTELLECT : ITEM_MOD_INTELLECT;
            default:                 return ITEM_MOD_INTELLECT; // priest/mage/warlock
        }
    }

    // Maps a specialization back to a coarse role, but only for classes that can
    // actually fill the tank/healer role; everything else counts as damage.
    BotRole RoleFromSpec(uint8 cls, uint32 specId)
    {
        if (specId)
        {
            for (uint8 c : TankClasses)
                if (c == cls && specId == SpecIdForRole(cls, BOT_ROLE_TANK))
                    return BOT_ROLE_TANK;
            for (uint8 c : HealerClasses)
                if (c == cls && specId == SpecIdForRole(cls, BOT_ROLE_HEALER))
                    return BOT_ROLE_HEALER;
        }
        return BOT_ROLE_DPS;
    }

    // Returns candidate item entries (highest item level first) matching the
    // WHERE clause for this bot. When primaryStat is set the search is
    // restricted to items carrying that stat. More than one row is returned so
    // callers can skip items the bot cannot actually equip yet (e.g. a low-level
    // plate class that only has armor proficiency for mail/leather).
    //
    // Filter by RequiredLevel near the bot's level (not ItemLevel <= level+25):
    // MoP ilvl is hundreds at 90, so an ItemLevel cap wrongly forces ~TBC gear.
    // minQuality/maxQuality are ITEM_QUALITY_* bounds from conf or init override.
    // Results are cached — mass init of similar level/class bots hits memory.
    std::unordered_map<std::string, std::vector<uint32>> g_itemQueryCache;

    // Which half of the item pool gear rolls draw from (PvP = resilience or PvP power).
    enum class GearPool : uint8 { Any, PveOnly, PvpOnly };
    GearPool g_gearPool = GearPool::Any;

    struct ScopedGearPool
    {
        explicit ScopedGearPool(GearPool pool) : _previous(g_gearPool) { g_gearPool = pool; }
        ~ScopedGearPool() { g_gearPool = _previous; }
        GearPool _previous;
    };

    // Autogear knobs for the current gear roll (see ScopedGearRoll).
    struct GearRoll
    {
        uint32 maxItemLevel = 0; // 0 = only the level-based ceiling
        bool incremental = false; // a slot changes only when the pick beats the worn piece
        uint32 changed = 0;       // slots an incremental roll upgraded
    };
    GearRoll g_gearRoll;

    struct ScopedGearRoll
    {
        ScopedGearRoll(uint32 maxItemLevel, bool incremental) : _previous(g_gearRoll)
        {
            g_gearRoll = GearRoll();
            g_gearRoll.maxItemLevel = maxItemLevel;
            g_gearRoll.incremental = incremental;
        }
        ~ScopedGearRoll() { g_gearRoll = _previous; }
        GearRoll _previous;
    };

    std::string PvpStatClause()
    {
        std::ostringstream c;
        c << '(';
        for (uint32 i = 1; i <= MAX_ITEM_PROTO_STATS; ++i)
            c << (i > 1 ? " OR " : "") << "stat_type" << i << " IN ("
              << uint32(ITEM_MOD_RESILIENCE_RATING) << ',' << uint32(ITEM_MOD_PVP_POWER) << ')';
        c << ')';
        return c.str();
    }

    std::vector<uint32> QueryItemEntries(Player* bot, std::string const& where, uint32 primaryStat,
        uint32 exclude, int minQuality, int maxQuality)
    {
        uint32 const level    = bot->getLevel();
        uint32 const classBit = 1u << (bot->getClass() - 1);
        uint32 const raceBit  = 1u << (bot->getRace() - 1);
        uint32 const minReq   = level > 10 ? (level - 10) : 1;
        int qualityCap = std::max(0, std::min(maxQuality, int(ITEM_QUALITY_LEGENDARY)));
        int qualityMin = std::max(0, std::min(minQuality, qualityCap));

        std::ostringstream key;
        // v3: broader QA/template bans + rare-low-ilvl reject + min ItemLevel floor.
        key << "v4|" << where << '|' << level << '|' << classBit << '|' << raceBit << '|'
            << minReq << '|' << qualityMin << '|' << qualityCap << '|' << primaryStat
            << '|' << exclude << '|' << uint32(g_gearPool) << '|' << g_gearRoll.maxItemLevel;
        std::string const cacheKey = key.str();
        if (auto it = g_itemQueryCache.find(cacheKey); it != g_itemQueryCache.end())
            return it->second;

        std::ostringstream q;
        q << "SELECT entry FROM item_template WHERE " << where
          << " AND RequiredLevel <= " << level
          << " AND RequiredLevel >= " << minReq
          << " AND Quality BETWEEN " << qualityMin << " AND " << qualityCap
          << " AND duration = 0 AND startquest = 0"
          // Designer / QA / art / monster templates (RequiredLevel 1, junk names).
          << " AND name NOT LIKE 'QA%'"
          << " AND name NOT LIKE '%Test%'"
          << " AND name NOT LIKE 'ZG %'"
          << " AND name NOT LIKE '%Raid D0%'"
          << " AND name NOT LIKE '% D0%'"
          << " AND name NOT LIKE 'Monster -%'"
          << " AND name NOT LIKE 'Art Template%'"
          << " AND name NOT LIKE 'Artwork %'"
          << " AND (Flags & " << uint32(ITEM_PROTO_FLAG_DEPRECATED) << ") = 0"
          // Rare+ with tiny ilvl is almost always a leftover template (ZG/Icecrown stubs).
          << " AND NOT (Quality >= " << int(ITEM_QUALITY_RARE)
          << " AND ItemLevel < 10 AND RequiredLevel <= 10)"
          // Soft ilvl ceiling so low-req raid gear cannot dominate the pool.
          << " AND ItemLevel <= " << (level * 8u + 40u)
          // Prefer real level-scaled gear over ilvl-1 stubs.
          << " AND ItemLevel >= " << (level > 5 ? (level > 10 ? level - 5 : 3) : 1)
          << " AND (AllowableClass = -1 OR (AllowableClass & " << classBit << "))"
          << " AND (AllowableRace = -1 OR (AllowableRace & " << raceBit << "))";

        if (g_gearRoll.maxItemLevel)
            q << " AND ItemLevel <= " << g_gearRoll.maxItemLevel;

        if (exclude)
            q << " AND entry <> " << exclude;

        if (g_gearPool == GearPool::PveOnly)
            q << " AND NOT " << PvpStatClause();
        else if (g_gearPool == GearPool::PvpOnly)
            q << " AND " << PvpStatClause();

        if (primaryStat)
            q << " AND (stat_type1=" << primaryStat << " OR stat_type2=" << primaryStat
              << " OR stat_type3=" << primaryStat << " OR stat_type4=" << primaryStat
              << " OR stat_type5=" << primaryStat << " OR stat_type6=" << primaryStat
              << " OR stat_type7=" << primaryStat << " OR stat_type8=" << primaryStat
              << " OR stat_type9=" << primaryStat << " OR stat_type10=" << primaryStat << ")";

        // Prefer items the bot actually leveled into; only then pick the strongest.
        q << " ORDER BY RequiredLevel DESC, ItemLevel DESC LIMIT 25";

        std::vector<uint32> entries;
        if (QueryResult result = WorldDatabase.Query(q.str().c_str()))
        {
            do
            {
                entries.push_back(result->Fetch()[0].GetUInt32());
            } while (result->NextRow());
        }
        g_itemQueryCache.emplace(cacheKey, entries);
        return entries;
    }

    // Class trainer spell lists — built once per class (full creature scan is costly).
    std::unordered_map<uint8, std::vector<TrainerSpell const*>> g_trainerSpellCache;

    std::vector<TrainerSpell const*> const& GetClassTrainerSpells(uint8 cls)
    {
        auto it = g_trainerSpellCache.find(cls);
        if (it != g_trainerSpellCache.end())
            return it->second;

        std::vector<TrainerSpell const*> trainerSpells;
        std::unordered_set<uint32> seen;
        CreatureTemplateContainer const* templates = sObjectMgr->GetCreatureTemplates();
        if (templates)
        {
            for (CreatureTemplateContainer::const_iterator itr = templates->begin();
                itr != templates->end(); ++itr)
            {
                CreatureTemplate const& creature = itr->second;
                if (creature.trainer_type != TRAINER_TYPE_CLASS)
                    continue;
                if (creature.trainer_class != cls)
                    continue;
                if (!(creature.npcflag & UNIT_NPC_FLAG_TRAINER))
                    continue;

                TrainerSpellData const* data = sObjectMgr->GetNpcTrainerSpells(creature.Entry);
                if (!data)
                    continue;

                for (TrainerSpellMap::const_iterator sp = data->spellList.begin();
                    sp != data->spellList.end(); ++sp)
                {
                    if (!seen.insert(sp->first).second)
                        continue;
                    trainerSpells.push_back(&sp->second);
                }
            }
        }
        auto inserted = g_trainerSpellCache.emplace(cls, std::move(trainerSpells));
        return inserted.first->second;
    }

    // Level gate used by SkillLineAbility / SpecializationSpells. Prefer
    // SpellLevel, fall back to BaseLevel (some MoP rows store the gate there).
    uint32 SpellLearnLevel(SpellInfo const* info)
    {
        if (!info)
            return 0;
        if (info->SpellLevel)
            return info->SpellLevel;
        return info->BaseLevel;
    }

    // Hardcoded class skill lines — GetSkillIdByClass() can fail when ChrClasses
    // locale names do not match SkillLine names.
    uint32 ClassSkillId(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return SKILL_GENERAL_WARRIOR;
            case CLASS_PALADIN:      return SKILL_GENERAL_PALADIN;
            case CLASS_HUNTER:       return SKILL_GENERAL_HUNTER;
            case CLASS_ROGUE:        return SKILL_GENERAL_ROGUE;
            case CLASS_PRIEST:       return SKILL_GENERAL_PRIEST;
            case CLASS_DEATH_KNIGHT: return SKILL_GENERAL_DEATH_KNIGHT;
            case CLASS_SHAMAN:       return SKILL_GENERAL_SHAMAN;
            case CLASS_MAGE:         return SKILL_GENERAL_MAGE;
            case CLASS_WARLOCK:      return SKILL_GENERAL_WARLOCK;
            case CLASS_MONK:         return SKILL_GENERAL_MONK;
            case CLASS_DRUID:        return SKILL_GENERAL_DRUID;
            default:                 return GetSkillIdByClass(cls);
        }
    }

    // Independent learn so spells persist in character_spell (dependent=true is
    // skipped on SaveToDB — that left bots able to cast for one session only).
    void TryLearnSpell(Player* bot, uint32 spellId)
    {
        if (!bot || !spellId)
            return;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || !SpellMgr::IsSpellValid(info, bot, false))
            return;
        if (GetTalentSpellCost(spellId) > 0)
            return;

        uint32 const needLevel = SpellLearnLevel(info);
        if (needLevel && needLevel > bot->getLevel())
            return;

        // Already known as a saved independent spell — nothing to do.
        // If only known as dependent, remove and re-add independent so it saves.
        if (bot->HasSpell(spellId))
        {
            PlayerSpellMap::const_iterator itr = bot->GetSpellMap().find(spellId);
            if (itr != bot->GetSpellMap().end() && itr->second && !itr->second->dependent
                && itr->second->state != PLAYERSPELL_REMOVED)
                return;
            bot->removeSpell(spellId, false, false);
        }

        if (!bot->IsSpellFitByClassAndRace(spellId))
            return;

        bot->learnSpell(spellId, false);
    }

    // Teach every class-trainer spell the bot qualifies for at its current level
    // (multi-pass so spell-chain prerequisites can unlock in order).
    void LearnClassTrainerSpells(Player* bot)
    {
        if (!bot)
            return;

        std::vector<TrainerSpell const*> const& trainerSpells = GetClassTrainerSpells(bot->getClass());

        for (int pass = 0; pass < 12; ++pass)
        {
            bool learnedAny = false;
            for (TrainerSpell const* ts : trainerSpells)
            {
                if (!ts)
                    continue;

                // If learnedSpell[] was left empty by a LEARN_SPELL edge case,
                // GetTrainerSpellState reports GRAY (already known) incorrectly.
                // Fall back to teaching ts->spell when the bot lacks it and meets
                // the level requirement.
                TrainerSpellState const state = bot->GetTrainerSpellState(ts);
                if (state == TRAINER_SPELL_GREEN)
                {
                    if (ts->IsCastable())
                        bot->CastSpell(bot, ts->spell, true);
                    else
                        bot->learnSpell(ts->spell, false);
                    learnedAny = true;
                    continue;
                }

                if (state != TRAINER_SPELL_GRAY)
                    continue;

                bool anyLearnedSlot = false;
                for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                {
                    if (ts->learnedSpell[i])
                    {
                        anyLearnedSlot = true;
                        break;
                    }
                }
                if (anyLearnedSlot)
                    continue;
                if (!sSpellMgr->GetSpellInfo(ts->spell))
                    continue;
                if (bot->HasSpell(ts->spell))
                    continue;
                if (ts->reqLevel && bot->getLevel() < ts->reqLevel)
                    continue;
                if (!bot->IsSpellFitByClassAndRace(ts->spell))
                    continue;

                bot->learnSpell(ts->spell, false);
                learnedAny = true;
            }
            if (!learnedAny)
                break;
        }

        // Convert trainer spells known only as dependent (unsaved) into
        // independent spells, and pick up anything the GREEN pass missed.
        for (TrainerSpell const* ts : trainerSpells)
        {
            if (!ts || !ts->spell)
                continue;
            if (ts->reqLevel && bot->getLevel() < ts->reqLevel)
                continue;
            uint32 spellId = ts->spell;
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                if (ts->learnedSpell[i])
                {
                    spellId = ts->learnedSpell[i];
                    break;
                }
            }
            TryLearnSpell(bot, spellId);
        }
    }

    // Teach class/spec spells the core GetSpellsForLevels path often misses.
    void LearnLevelClassSpells(Player* bot, uint32 specId)
    {
        if (!bot)
            return;

        uint8 const level = bot->getLevel();
        uint32 const raceMask = bot->getRaceMask();
        uint32 const classSkill = ClassSkillId(bot->getClass());

        // 1) Stock helper (class skill + specialization when mapping works).
        std::list<uint32> const stock = GetSpellsForLevels(
            bot->getClass(), raceMask, specId, 0, level);
        for (uint32 spellId : stock)
            TryLearnSpell(bot, spellId);

        // 2) Direct scan of THIS class's skill line only (e.g. skill 804 = Priest).
        if (classSkill)
        {
            for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
            {
                SkillLineAbilityEntry const* ability = sSkillLineAbilityStore.LookupEntry(i);
                if (!ability || !ability->spellId)
                    continue;
                if (ability->skillId != classSkill)
                    continue;
                if (ability->learnOnGetSkill != ABILITY_LEARNED_ON_GET_RACE_OR_CLASS_SKILL)
                    continue;
                if (ability->racemask && !(ability->racemask & raceMask))
                    continue;
                if (!ability->classmask || !(ability->classmask & bot->getClassMask()))
                    continue;

                TryLearnSpell(bot, ability->spellId);
            }
        }

        // 3) Specialization spells (Mind Flay, Shadowform, etc.) by level.
        if (specId)
        {
            if (std::vector<uint32> const* specSpells = GetSpecializationSpells(specId))
            {
                for (uint32 spellId : *specSpells)
                    TryLearnSpell(bot, spellId);
            }
        }

        // 4) Race+class create spells (Smite, racial languages for this combo).
        bot->learnDefaultSpells();

        // 5) Critical baseline casts that create/trainer paths still miss on some
        // races (e.g. Smite 585) — without these, shadow bots only land SW:P.
        if (bot->getClass() == CLASS_PRIEST)
        {
            TryLearnSpell(bot, 585);   // Smite
            TryLearnSpell(bot, 589);   // Shadow Word: Pain
            TryLearnSpell(bot, 588);   // Inner Fire
            TryLearnSpell(bot, 8092);  // Mind Blast
            if (specId == SPEC_PRIEST_SHADOW)
                TryLearnSpell(bot, 15407); // Mind Flay
        }
        if (bot->getClass() == CLASS_HUNTER)
        {
            TryLearnSpell(bot, 75);    // Auto Shot
            TryLearnSpell(bot, 56641); // Steady Shot
            TryLearnSpell(bot, 3044);  // Arcane Shot
            TryLearnSpell(bot, 1978);  // Serpent Sting
            TryLearnSpell(bot, 13165); // Aspect of the Hawk
            TryLearnSpell(bot, 883);   // Call Pet 1
            TryLearnSpell(bot, 982);   // Revive Pet
        }
        if (bot->getClass() == CLASS_PALADIN)
        {
            TryLearnSpell(bot, 20154); // Seal of Righteousness
            TryLearnSpell(bot, 20271); // Judgment
            TryLearnSpell(bot, 35395); // Crusader Strike
            TryLearnSpell(bot, 879);   // Exorcism
            if (level >= 32)
                TryLearnSpell(bot, 20165); // Seal of Insight
            if (level >= 44)
                TryLearnSpell(bot, 31801); // Seal of Truth
        }
        if (bot->getClass() == CLASS_WARRIOR)
        {
            TryLearnSpell(bot, 78);    // Heroic Strike
            TryLearnSpell(bot, 71);    // Defensive Stance
            TryLearnSpell(bot, 355);   // Taunt
            TryLearnSpell(bot, 6343);  // Thunder Clap
            TryLearnSpell(bot, 7386);  // Sunder Armor
            TryLearnSpell(bot, 23922); // Shield Slam
            if (level >= 20)
                TryLearnSpell(bot, 6673); // Battle Shout
            if (level >= 28)
                TryLearnSpell(bot, 2565); // Shield Block
            if (level >= 26)
                TryLearnSpell(bot, 20243); // Devastate
            if (level >= 40)
                TryLearnSpell(bot, 6572); // Revenge
        }
        if (bot->getClass() == CLASS_WARLOCK)
        {
            TryLearnSpell(bot, 686);  // Shadow Bolt
            TryLearnSpell(bot, 172);  // Corruption
            TryLearnSpell(bot, 980);  // Agony
            TryLearnSpell(bot, 689);  // Drain Life
            TryLearnSpell(bot, 1454); // Life Tap
            TryLearnSpell(bot, 688);  // Summon Imp
        }
    }

    uint32 PickRandomSpell(uint32 const* list, size_t count)
    {
        if (!list || !count)
            return 0;
        return list[static_cast<size_t>(std::rand()) % count];
    }

    void LearnMountIfMissing(Player* bot, uint32 const* primary, size_t primaryCount,
        uint32 const* fallback, size_t fallbackCount)
    {
        if (!bot)
            return;

        auto hasAny = [&](uint32 const* list, size_t count) -> bool
        {
            for (size_t i = 0; i < count; ++i)
                if (list[i] && bot->HasSpell(list[i]))
                    return true;
            return false;
        };

        if (hasAny(primary, primaryCount) || hasAny(fallback, fallbackCount))
            return;

        auto tryLearn = [&](uint32 const* list, size_t count) -> bool
        {
            if (!list || !count)
                return false;
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                uint32 const mount = PickRandomSpell(list, count);
                if (mount && bot->IsSpellFitByClassAndRace(mount))
                {
                    bot->learnSpell(mount, false);
                    return true;
                }
            }
            for (size_t i = 0; i < count; ++i)
            {
                if (list[i] && bot->IsSpellFitByClassAndRace(list[i]))
                {
                    bot->learnSpell(list[i], false);
                    return true;
                }
            }
            return false;
        };

        if (!tryLearn(primary, primaryCount))
            tryLearn(fallback, fallbackCount);
    }

    // Riding skills + one random mount per unlocked tier (level-gated).
    // Tiers: normal ground (20), swift ground (40), normal flying (60), epic flying (70).
    void LearnRidingAndMounts(Player* bot)
    {
        if (!bot)
            return;

        uint8 const level = bot->getLevel();
        auto learnRiding = [&](uint32 spellId)
        {
            if (spellId && !bot->HasSpell(spellId))
                bot->learnSpell(spellId, false);
        };

        // Faction mount pools (spell IDs taught by the mount items).
        static uint32 const allianceGround[] = { 458, 470, 472, 6648, 6777, 6898, 6899 };
        static uint32 const hordeGround[]    = { 580, 6653, 6654, 8395, 10796, 17462, 18989, 34795 };
        static uint32 const neutralGround[]  = { 43899, 49379 };

        static uint32 const allianceSwiftGround[] = { 23227, 23228, 23229, 23238, 23239, 23240 };
        static uint32 const hordeSwiftGround[]    = { 23250, 23251, 23252, 23241, 23242, 23243, 35025, 35027 };
        static uint32 const neutralSwiftGround[]  = { 42777 };

        static uint32 const allianceFlying[] = { 32235, 32239, 32240 };
        static uint32 const hordeFlying[]    = { 32243, 32244, 32245 };
        static uint32 const neutralFlying[]  = { 59569, 59570 };

        static uint32 const allianceEpicFlying[] = { 32242, 32289, 32290, 32292 };
        static uint32 const hordeEpicFlying[]    = { 32246, 32295, 32296, 32297 };
        static uint32 const neutralEpicFlying[]  = { 60025 };

        bool const alliance = bot->GetTeam() == ALLIANCE;

        if (level >= 20)
        {
            learnRiding(33388); // Apprentice Riding
            if (alliance)
                LearnMountIfMissing(bot, allianceGround, sizeof(allianceGround) / sizeof(allianceGround[0]),
                    neutralGround, sizeof(neutralGround) / sizeof(neutralGround[0]));
            else
                LearnMountIfMissing(bot, hordeGround, sizeof(hordeGround) / sizeof(hordeGround[0]),
                    neutralGround, sizeof(neutralGround) / sizeof(neutralGround[0]));
        }

        if (level >= 40)
        {
            learnRiding(33391); // Journeyman Riding
            if (alliance)
                LearnMountIfMissing(bot, allianceSwiftGround, sizeof(allianceSwiftGround) / sizeof(allianceSwiftGround[0]),
                    neutralSwiftGround, sizeof(neutralSwiftGround) / sizeof(neutralSwiftGround[0]));
            else
                LearnMountIfMissing(bot, hordeSwiftGround, sizeof(hordeSwiftGround) / sizeof(hordeSwiftGround[0]),
                    neutralSwiftGround, sizeof(neutralSwiftGround) / sizeof(neutralSwiftGround[0]));
        }

        if (level >= 60)
        {
            learnRiding(34090); // Expert Riding
            learnRiding(90267); // Flight Master's License
            if (alliance)
                LearnMountIfMissing(bot, allianceFlying, sizeof(allianceFlying) / sizeof(allianceFlying[0]),
                    neutralFlying, sizeof(neutralFlying) / sizeof(neutralFlying[0]));
            else
                LearnMountIfMissing(bot, hordeFlying, sizeof(hordeFlying) / sizeof(hordeFlying[0]),
                    neutralFlying, sizeof(neutralFlying) / sizeof(neutralFlying[0]));
        }

        if (level >= 68)
            learnRiding(54197); // Cold Weather Flying

        if (level >= 70)
        {
            learnRiding(34091); // Artisan Riding
            if (alliance)
                LearnMountIfMissing(bot, allianceEpicFlying, sizeof(allianceEpicFlying) / sizeof(allianceEpicFlying[0]),
                    neutralEpicFlying, sizeof(neutralEpicFlying) / sizeof(neutralEpicFlying[0]));
            else
                LearnMountIfMissing(bot, hordeEpicFlying, sizeof(hordeEpicFlying) / sizeof(hordeEpicFlying[0]),
                    neutralEpicFlying, sizeof(neutralEpicFlying) / sizeof(neutralEpicFlying[0]));
        }

        if (level >= 80)
            learnRiding(90265); // Master Riding
    }

    // Learn the armor proficiency the class actually uses in MoP. Cata removed
    // the old level-40 plate/mail trainer gates, so warriors/paladins start in
    // plate and hunters/shamans start in mail.
    void LearnArmorProficiencies(Player* bot)
    {
        if (!bot)
            return;

        uint8 const cls = bot->getClass();

        auto learn = [&](uint32 spellId)
        {
            if (spellId && !bot->HasSpell(spellId))
                bot->learnSpell(spellId, true);
        };

        // Cloth (9078) is baseline for cloth wearers; leather classes also use it
        // under the armor. Learning again is a no-op when already known.
        switch (cls)
        {
            case CLASS_PRIEST:
            case CLASS_MAGE:
            case CLASS_WARLOCK:
                learn(9078); // Cloth
                break;
            case CLASS_ROGUE:
            case CLASS_DRUID:
            case CLASS_MONK:
                learn(9078);
                learn(9077); // Leather
                break;
            case CLASS_HUNTER:
            case CLASS_SHAMAN:
                learn(9078);
                learn(9077);
                learn(8737); // Mail
                break;
            case CLASS_WARRIOR:
            case CLASS_PALADIN:
                learn(9078);
                learn(9077);
                learn(119811); // Mail
                learn(750);    // Plate Mail
                break;
            case CLASS_DEATH_KNIGHT:
                // DKs begin at 55 with plate available.
                learn(9078);
                learn(9077);
                learn(119811);
                learn(750);
                break;
            default:
                break;
        }

        // Keep weapon skills usable for the equipped level band.
        bot->UpdateSkillsToMaxSkillsForLevel();
    }

    // Equips a fresh copy of the item into whichever slot the core picks.
    bool EquipItemEntry(Player* bot, uint32 entry)
    {
        if (!entry)
            return false;

        uint16 dest = 0;
        if (bot->CanEquipNewItem(NULL_SLOT, dest, entry, false) != EQUIP_ERR_OK)
            return false;

        return bot->EquipNewItem(dest, entry, true) != nullptr;
    }

    // Autogear's upgrade yardstick: item level, halved for a piece that carries
    // another primary stat but not the one the spec wants (an earlier spec's
    // gear). Items the bot cannot use score below an empty slot.
    float GearScore(Player* bot, ItemTemplate const* proto, uint32 primaryStat)
    {
        if (!proto || bot->CanUseItem(proto) != EQUIP_ERR_OK)
            return -1.0f;

        float const score = float(proto->ItemLevel);
        if (!primaryStat)
            return score;

        bool wanted = false;
        bool other = false;
        for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
        {
            uint32 const stat = proto->ItemStat[i].ItemStatType;
            if (stat == primaryStat)
                wanted = true;
            else if (stat == ITEM_MOD_STRENGTH || stat == ITEM_MOD_AGILITY || stat == ITEM_MOD_INTELLECT)
                other = true;
        }
        return other && !wanted ? score * 0.5f : score;
    }

    // Moves a worn piece into the backpack or a regular bag. The PvP gear bag is
    // skipped so it keeps one piece per equipment-slot position.
    bool StoreWornItem(Player* bot, uint8 slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || bot->CanUnequipItem(uint16(INVENTORY_SLOT_BAG_0) << 8 | slot, false) != EQUIP_ERR_OK)
            return false;

        Bag* const gearBag = BotGearSets::FindGearBag(bot);
        std::vector<uint8> bags = { INVENTORY_SLOT_BAG_0 };
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* container = bot->GetBagByPos(bag))
                if (container != gearBag)
                    bags.push_back(bag);

        for (uint8 bag : bags)
        {
            ItemPosCountVec dest;
            if (bot->CanStoreItem(bag, NULL_SLOT, dest, item, false) != EQUIP_ERR_OK)
                continue;
            bot->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
            bot->StoreItem(dest, item, true);
            return true;
        }
        return false;
    }

    // Incremental autogear for one slot: equips the strongest candidate only if
    // it beats the worn piece, which goes to the bags (AC keeps it the same way;
    // a full bag keeps the worn piece). Returns the entry left in the slot so
    // callers' fallbacks (2H -> 1H + shield, plate -> mail) only run when empty.
    uint32 UpgradeSlot(Player* bot, uint8 slot, std::string const& where, uint32 primaryStat,
        uint32 exclude, int minQuality, int maxQuality)
    {
        Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        uint32 const wornEntry = worn ? worn->GetEntry() : 0;
        float const wornScore = worn ? GearScore(bot, worn->GetTemplate(), primaryStat) : -1.0f;

        uint32 const passStats[2] = { primaryStat, 0 };
        int const passes = primaryStat && g_gearPool != GearPool::PvpOnly ? 2 : 1;

        for (int p = 0; p < passes; ++p)
        {
            std::vector<uint32> const entries = QueryItemEntries(bot, where, passStats[p], exclude,
                minQuality, maxQuality);
            if (entries.empty())
                continue;

            uint32 best = 0;
            float bestScore = wornScore;
            for (uint32 entry : entries)
            {
                if (entry == wornEntry)
                    continue;
                float const score = GearScore(bot, sObjectMgr->GetItemTemplate(entry), primaryStat);
                if (score <= bestScore)
                    continue;
                uint16 dest = 0;
                if (bot->CanEquipNewItem(slot, dest, entry, true) != EQUIP_ERR_OK)
                    continue;
                best = entry;
                bestScore = score;
            }

            if (!best)
            {
                if (worn)
                    return wornEntry;
                continue;
            }

            if (worn && !StoreWornItem(bot, slot))
                return wornEntry;

            // A two-hander needs the off hand free; keep that piece too.
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(best);
            if (proto && proto->InventoryType == INVTYPE_2HWEAPON && !bot->CanTitanGrip()
                && bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
                StoreWornItem(bot, EQUIPMENT_SLOT_OFFHAND);

            uint16 dest = 0;
            if (bot->CanEquipNewItem(slot, dest, best, false) != EQUIP_ERR_OK
                || !bot->EquipNewItem(dest, best, true))
                return 0;

            bot->AutoUnequipOffhandIfNeed();
            ++g_gearRoll.changed;
            return best;
        }
        return wornEntry;
    }

    // Equips a strong item matching the WHERE clause that the bot can actually
    // use. Among the top ilvl candidates, pick randomly so bots do not all wear
    // the identical BiS-looking template. Prefers primary-stat items, then any.
    // The exclude entry keeps paired slots (rings/trinkets) distinct.
    // slot is the equipment slot the clause fills; incremental rolls need it.
    uint32 EquipBestForSlot(Player* bot, std::string const& where, uint32 primaryStat, uint32 exclude,
        int minQuality, int maxQuality, uint8 slot = NULL_SLOT)
    {
        if (g_gearRoll.incremental && slot != NULL_SLOT)
            return UpgradeSlot(bot, slot, where, primaryStat, exclude, minQuality, maxQuality);

        uint32 const passStats[2] = { primaryStat, 0 };
        // A PvP piece with the wrong primary stat is worse than keeping the PvE piece.
        int const passes = primaryStat && g_gearPool != GearPool::PvpOnly ? 2 : 1;
        constexpr size_t kRandomPool = 8;

        for (int p = 0; p < passes; ++p)
        {
            std::vector<uint32> entries = QueryItemEntries(bot, where, passStats[p], exclude,
                minQuality, maxQuality);
            if (entries.empty())
                continue;

            size_t const pool = std::min(entries.size(), kRandomPool);
            for (size_t i = 0; i + 1 < pool; ++i)
            {
                size_t const j = i + size_t(std::rand() % int(pool - i));
                std::swap(entries[i], entries[j]);
            }

            for (uint32 entry : entries)
                if (EquipItemEntry(bot, entry))
                    return entry;
        }
        return 0;
    }

    // Removes everything currently equipped except the cosmetic shirt/tabard so
    // a re-gear starts from a clean set of slots.
    void ClearEquipment(Player* bot)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
                continue;
            if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                bot->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        }
    }

    // Picks class/role-appropriate weapons (and shield/offhand/ranged).
    void GearWeapons(Player* bot, BotRole role, uint32 specId, int minQuality, int maxQuality)
    {
        uint8 const cls = bot->getClass();
        uint32 const primary = PrimaryStatForClassRole(cls, role, specId);

        uint8 const mainHand = EQUIPMENT_SLOT_MAINHAND;
        uint8 const offHand = EQUIPMENT_SLOT_OFFHAND;
        auto equip = [&](std::string const& where, uint8 slot) -> bool
        {
            return EquipBestForSlot(bot, where, primary, 0, minQuality, maxQuality, slot) != 0;
        };

        std::string const oneH   = "class=2 AND InventoryType IN (13,21)";
        std::string const twoH   = "class=2 AND InventoryType=17";
        std::string const shield = "class=4 AND subclass=6 AND InventoryType=14";
        std::string const held   = "class=4 AND InventoryType=23";
        std::string const ranged = "class=2 AND InventoryType IN (15,26) AND subclass IN (2,3,18)";

        switch (cls)
        {
            case CLASS_HUNTER:
                equip(ranged, mainHand);
                break;
            case CLASS_WARRIOR:
                if (role == BOT_ROLE_TANK)
                {
                    if (!equip(oneH + " AND subclass IN (0,4,7)", mainHand))
                        equip(twoH + " AND subclass IN (1,5,6,8)", mainHand);
                    equip(shield, offHand);
                }
                else if (!equip(twoH + " AND subclass IN (1,5,6,8)", mainHand))
                {
                    equip(oneH + " AND subclass IN (0,4,7)", mainHand);
                    equip(shield, offHand);
                }
                break;
            case CLASS_PALADIN:
                if (role == BOT_ROLE_DPS)
                {
                    if (!equip(twoH + " AND subclass IN (1,5,6,8)", mainHand))
                    {
                        equip(oneH + " AND subclass IN (0,4,7)", mainHand);
                        equip(shield, offHand);
                    }
                }
                else
                {
                    equip(oneH + " AND subclass IN (0,4,7)", mainHand);
                    equip(shield, offHand);
                }
                break;
            case CLASS_DEATH_KNIGHT:
                if (!equip(twoH + " AND subclass IN (1,5,6,8)", mainHand))
                    equip(oneH + " AND subclass IN (0,4,7)", mainHand);
                break;
            case CLASS_ROGUE:
                // Assassination (Mutilate) requires Dual Wield + daggers in both hands.
                if (specId == SPEC_ROGUE_ASSASSINATION)
                {
                    if (!bot->HasSpell(674))
                        bot->learnSpell(674, true); // Dual Wield
                    bot->SetCanDualWield(true);

                    std::string const dagger =
                        "class=2 AND subclass=15 AND InventoryType IN (13,21,22)";
                    uint32 const mh = EquipBestForSlot(bot, dagger, primary, 0, minQuality, maxQuality,
                        mainHand);
                    uint32 oh = 0;
                    if (mh)
                        oh = EquipBestForSlot(bot, dagger, primary, mh, minQuality, maxQuality, offHand);

                    // Last resort: any 1H if the dagger pool is empty, but still
                    // force Dual Wield so an off-hand slot can fill.
                    if (!mh)
                        EquipBestForSlot(bot, oneH + " AND subclass IN (0,4,7,13,15)", primary, 0,
                            minQuality, maxQuality, mainHand);
                    if (!oh)
                        EquipBestForSlot(bot, oneH + " AND subclass IN (0,4,7,13,15)", primary,
                            bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND)
                                ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND)->GetEntry()
                                : 0,
                            minQuality, maxQuality, offHand);
                }
                else
                {
                    if (!bot->HasSpell(674))
                        bot->learnSpell(674, true);
                    bot->SetCanDualWield(true);
                    equip(oneH + " AND subclass IN (0,4,7,13,15)", mainHand);
                    equip(oneH + " AND subclass IN (0,4,7,13,15)", offHand);
                }
                break;
            case CLASS_SHAMAN:
                if (specId == SPEC_SHAMAN_ENHANCEMENT)
                {
                    // Enhancement: dual-wield 1H.
                    equip(oneH + " AND subclass IN (0,4,13,15)", mainHand);
                    equip(oneH + " AND subclass IN (0,4,13,15)", offHand);
                }
                else
                {
                    equip(oneH + " AND subclass IN (0,4,13,15)", mainHand);
                    equip(shield, offHand);
                }
                break;
            case CLASS_MONK:
                if (!equip(twoH + " AND subclass IN (6,10)", mainHand))
                {
                    equip(oneH + " AND subclass IN (0,4,7,13)", mainHand);
                    equip(held, offHand);
                }
                break;
            case CLASS_DRUID:
                if (!equip(twoH + " AND subclass IN (6,10)", mainHand))
                {
                    equip(oneH + " AND subclass IN (4,13,15)", mainHand);
                    equip(held, offHand);
                }
                break;
            case CLASS_PRIEST:
                if (!equip(twoH + " AND subclass=10", mainHand))
                {
                    equip(oneH + " AND subclass IN (4,15)", mainHand);
                    equip(held, offHand);
                }
                break;
            case CLASS_MAGE:
            case CLASS_WARLOCK:
                if (!equip(twoH + " AND subclass=10", mainHand))
                {
                    equip(oneH + " AND subclass IN (7,15)", mainHand);
                    equip(held, offHand);
                }
                break;
            default:
                break;
        }
    }

    // Fills every gear slot with level/class/spec-appropriate items.
    void GearBot(Player* bot, BotRole role, uint32 specId, int minQuality, int maxQuality)
    {
        uint8 const cls = bot->getClass();
        uint32 const primary = PrimaryStatForClassRole(cls, role, specId);
        // Prefer the class's MoP armor type. Plate often does not exist at low
        // levels, so warriors/paladins/DKs fall back to mail for an empty slot.
        // Do not continue into leather or cloth — that left max-level bots in cloth.
        uint8 const primaryArmor = ArmorTypeForClass(cls);
        uint8 const fallbackArmor = primaryArmor == ITEM_SUBCLASS_ARMOR_PLATE
            ? uint8(ITEM_SUBCLASS_ARMOR_MAIL) : 0;

        if (!g_gearRoll.incremental)
            ClearEquipment(bot);

        // Best equippable item for a slot, optionally excluding an already-picked
        // entry (used to place two distinct rings/trinkets).
        auto equipOne = [&](std::string const& where, uint32 exclude, uint8 slot) -> uint32
        {
            return EquipBestForSlot(bot, where, primary, exclude, minQuality, maxQuality, slot);
        };

        auto equipArmorSlot = [&](char const* slotClause, uint8 equipSlot)
        {
            std::string const slot = std::string(" AND ") + slotClause;
            std::string const plateOrPrimary =
                "class=4 AND subclass=" + std::to_string(uint32(primaryArmor)) + slot;
            if (equipOne(plateOrPrimary, 0, equipSlot))
                return;

            if (!fallbackArmor)
                return;

            std::string const mail =
                "class=4 AND subclass=" + std::to_string(uint32(fallbackArmor)) + slot;
            if (equipOne(mail, 0, equipSlot))
                return;

            // Starter mail/plate is often white. The green+ band leaves those slots empty.
            if (minQuality <= int(ITEM_QUALITY_NORMAL))
                return;

            if (EquipBestForSlot(bot, plateOrPrimary, primary, 0, int(ITEM_QUALITY_NORMAL), maxQuality,
                equipSlot))
                return;
            EquipBestForSlot(bot, mail, primary, 0, int(ITEM_QUALITY_NORMAL), maxQuality, equipSlot);
        };

        equipArmorSlot("InventoryType=1", EQUIPMENT_SLOT_HEAD);
        equipArmorSlot("InventoryType=3", EQUIPMENT_SLOT_SHOULDERS);
        equipArmorSlot("InventoryType IN (5,20)", EQUIPMENT_SLOT_CHEST);
        equipArmorSlot("InventoryType=6", EQUIPMENT_SLOT_WAIST);
        equipArmorSlot("InventoryType=7", EQUIPMENT_SLOT_LEGS);
        equipArmorSlot("InventoryType=8", EQUIPMENT_SLOT_FEET);
        equipArmorSlot("InventoryType=9", EQUIPMENT_SLOT_WRISTS);
        equipArmorSlot("InventoryType=10", EQUIPMENT_SLOT_HANDS);

        equipOne("class=4 AND InventoryType=2", 0, EQUIPMENT_SLOT_NECK);
        equipOne("class=4 AND InventoryType=16", 0, EQUIPMENT_SLOT_BACK);

        uint32 ring1 = equipOne("class=4 AND InventoryType=11", 0, EQUIPMENT_SLOT_FINGER1);
        equipOne("class=4 AND InventoryType=11", ring1, EQUIPMENT_SLOT_FINGER2);
        uint32 trinket1 = equipOne("class=4 AND InventoryType=12", 0, EQUIPMENT_SLOT_TRINKET1);
        equipOne("class=4 AND InventoryType=12", trinket1, EQUIPMENT_SLOT_TRINKET2);

        GearWeapons(bot, role, specId, minQuality, maxQuality);
    }

    // Rolls a PvP set into the gear bag, then a PvE set to wear. Slots with no
    // PvP piece keep the PvE item in battlegrounds too.
    void GearBotSets(Player* bot, BotRole role, uint32 specId, int minQuality, int maxQuality)
    {
        bool const hasGearBag = BotGearSets::EnsureGearBag(bot) != nullptr;
        if (hasGearBag)
        {
            BotGearSets::ClearStoredSet(bot);
            {
                ScopedGearPool pvp(GearPool::PvpOnly);
                GearBot(bot, role, specId, minQuality, maxQuality);
            }
            BotGearSets::StoreEquippedAsPvpSet(bot);
        }

        ScopedGearPool pve(GearPool::PveOnly);
        GearBot(bot, role, specId, minQuality, maxQuality);
        if (hasGearBag)
            BotGearSets::DropMismatchedWeapons(bot);
    }

    // Rest is spell-based (level-based food/drink auras) — no bag food/drink.
    void GiveRestConsumables(Player* /*bot*/)
    {
    }

    // Stampede summons the pets in Call Pet slots 2-5; empty slots only clone the
    // current pet, so give each free slot its own non-exotic beast.
    void FillHunterPetSlots(Player* bot)
    {
        enum : uint32 { TAME_BEAST = 1515 };

        if (bot->getClass() != CLASS_HUNTER || bot->getLevel() < 10)
            return;

        static std::vector<std::pair<uint32, uint32>> const beasts = []
        {
            std::vector<std::pair<uint32, uint32>> list;
            if (QueryResult result = WorldDatabase.PQuery(
                "SELECT entry, family FROM creature_template WHERE (type_flags & %u) <> 0 "
                "AND family IN (1,2,3,4,5,6,7,8,9,11,12,20,21,24,25,26,27,31,34,35,37)",
                uint32(CREATURE_TYPEFLAGS_TAMEABLE)))
            {
                do
                {
                    Field* fields = result->Fetch();
                    list.emplace_back(fields[0].GetUInt32(), fields[1].GetUInt8());
                } while (result->NextRow());
            }
            return list;
        }();
        if (beasts.empty())
            return;

        bool slotUsed[MAX_ACTIVE_PETS] = {};
        std::unordered_set<uint32> usedFamilies;
        if (QueryResult result = CharacterDatabase.PQuery(
            "SELECT slot, entry FROM character_pet WHERE owner = %u AND slot <= %u",
            bot->GetGUIDLow(), uint32(PET_SAVE_LAST_ACTIVE_SLOT)))
        {
            do
            {
                Field* fields = result->Fetch();
                slotUsed[fields[0].GetUInt8()] = true;
                if (CreatureTemplate const* info = sObjectMgr->GetCreatureTemplate(fields[1].GetUInt32()))
                    usedFamilies.insert(info->family);
            } while (result->NextRow());
        }

        for (uint8 slot = PET_SAVE_FIRST_ACTIVE_SLOT + 1; slot <= PET_SAVE_LAST_ACTIVE_SLOT; ++slot)
        {
            if (slotUsed[slot])
                continue;

            uint32 entry = 0;
            for (int tries = 0; tries < 20 && !entry; ++tries)
            {
                std::pair<uint32, uint32> const& beast = beasts[std::rand() % beasts.size()];
                if (usedFamilies.insert(beast.second).second)
                    entry = beast.first;
            }
            if (!entry)
                continue;

            Pet* pet = bot->CreateTamedPetFrom(entry, TAME_BEAST);
            if (!pet)
                continue;

            // Dismiss-mode save keeps the pet in its Call Pet slot without making it current.
            pet->SetOwnerGUID(bot->GetGUID());
            pet->SetSlot(slot);
            pet->SavePetToDB(PET_SAVE_NOT_IN_SLOT);
            delete pet;
        }
    }
}

uint32 PlayerbotMgr::CreateBotPopulation(std::string* report, bool allowInitOnCreate)
{
    if (_autoAccountCount == 0)
    {
        if (report)
            *report = "Playerbots.AutoCreate.AccountCount is 0; nothing to create.";
        return 0;
    }

    uint8 expansion = uint8(sWorld->getIntConfig(WorldIntConfigs::CONFIG_EXPANSION));

    uint32 accountsCreated = 0;
    uint32 charsCreated = 0;
    std::vector<PendingBotInit> pending;

    for (uint32 i = 1; i <= _autoAccountCount; ++i)
    {
        std::string username = _accountPrefix + std::to_string(i);

        uint32 accountId = AccountMgr::GetId(username);
        if (!accountId)
        {
            // CreateAccount rejects an empty email (AOR_EMAIL_INVALID). Each account
            // needs its own address or the next one fails as a duplicate.
            std::string const email = username + "@playerbots.local";
            AccountOpResult res = sAccountMgr->CreateAccount(username, _autoPassword, email);
            if (res != AccountOpResult::AOR_OK)
            {
                SF_LOG_ERROR("modules", "[mod-playerbots] Failed to create bot account '%s' (error %u).",
                    username.c_str(), uint32(res));
                continue;
            }

            accountId = AccountMgr::GetId(username);
            if (!accountId)
                continue;

            ++accountsCreated;

            // Unlock every race/class by matching the realm's expansion.
            PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_UPD_EXPANSION);
            stmt->setUInt8(0, expansion);
            stmt->setUInt32(1, accountId);
            LoginDatabase.Execute(stmt);
        }

        charsCreated += PopulateAccount(accountId, &pending);
    }

    // One flush for the whole create batch (was Wait() per character).
    if (charsCreated)
        CharacterDatabase.Wait();

    bool const initNow = _autoInitOnCreate && allowInitOnCreate;
    uint32 inited = 0;
    if (initNow && !pending.empty())
    {
        SF_LOG_INFO("modules",
            "[mod-playerbots] Auto-init on create: gearing %u new character(s)...",
            uint32(pending.size()));
        for (PendingBotInit const& p : pending)
        {
            InitCreatedBot(p);
            ++inited;
            if ((inited % 25u) == 0u)
                SF_LOG_INFO("modules", "[mod-playerbots] Auto-init progress: %u / %u.",
                    inited, uint32(pending.size()));
        }
    }
    else if (!pending.empty())
    {
        for (PendingBotInit const& p : pending)
            _deferredInits[p.guid] = p;
        SF_LOG_INFO("modules",
            "[mod-playerbots] Deferred first-login init+teleport for %u character(s).",
            uint32(pending.size()));
    }

    // Newly created characters are candidates; refresh the pool on the next tick.
    _candidatesLoaded = false;

    SF_LOG_INFO("modules",
        "[mod-playerbots] Auto-create complete: %u new account(s), %u new character(s)%s.",
        accountsCreated, charsCreated,
        initNow ? "" : " (init deferred to first login)");
    if (initNow && inited)
        SF_LOG_INFO("modules", "[mod-playerbots] Auto-init finished: %u character(s).", inited);

    if (report)
    {
        std::ostringstream ss;
        ss << "Auto-create complete: " << accountsCreated << " new account(s), "
           << charsCreated << " new character(s)";
        if (initNow)
            ss << ", " << inited << " inited";
        else
            ss << " (gear on first login)";
        ss << ".";
        *report = ss.str();
    }

    return charsCreated;
}

void PlayerbotMgr::InitializeBot(Player* bot, int roleOverride, uint32 specOverride,
    int maxItemQuality, bool relevel, bool teleport, bool teleportFromCommand)
{
    if (!bot || !bot->IsInWorld())
        return;

    if (relevel)
    {
        uint32 const maxPlayerLevel = sWorld->getIntConfig(WorldIntConfigs::CONFIG_MAX_PLAYER_LEVEL);
        uint32 minL = std::min(_autoMinLevel, maxPlayerLevel);
        uint32 maxL = std::min(_autoMaxLevel, maxPlayerLevel);
        if (maxL < minL)
            std::swap(minL, maxL);
        uint8 const level = (maxL > minL)
            ? uint8(minL + uint32(std::rand() % int(maxL - minL + 1)))
            : uint8(minL);
        if (bot->getLevel() != level)
        {
            SF_LOG_INFO("modules",
                "[mod-playerbots]   '%s': relevelling %u -> %u (conf %u-%u)...",
                bot->GetName().c_str(), bot->getLevel(), uint32(level), minL, maxL);
            bot->GiveLevel(level);
            bot->SetUInt32Value(PLAYER_FIELD_XP, 0);
        }
    }

    SF_LOG_INFO("modules", "[mod-playerbots] Initializing bot '%s' (level %u %s %s)%s%s...",
        bot->GetName().c_str(), bot->getLevel(),
        SafeRaceName(bot->getRace()), SafeClassName(bot->getClass()),
        relevel ? " [relevel]" : "",
        teleport ? (teleportFromCommand ? " [tele]" : " [tele-auto]") : "");

    // Specialization + spells. Explicit specOverride wins; otherwise a role
    // override picks the default tab for that role; otherwise keep current.
    // Below level 10 specs are unavailable.
    BotRole role = BOT_ROLE_DPS;
    uint32 specId = 0;
    if (bot->getLevel() >= 10)
    {
        if (specOverride)
        {
            if (ChrSpecializationEntry const* entry = sChrSpecializationStore.LookupEntry(specOverride))
            {
                if (entry->classId == bot->getClass())
                    specId = specOverride;
            }
            if (!specId)
            {
                // Unknown or wrong-class spec id; keep existing state.
                specId = bot->GetTalentSpecialization(bot->GetActiveSpec());
                if (!specId)
                    specId = SpecIdForRole(bot->getClass(), BOT_ROLE_DPS);
                role = RoleFromSpec(bot->getClass(), specId);
            }
            else
                role = RoleFromSpec(bot->getClass(), specId);
        }
        else if (roleOverride >= 0)
        {
            role = static_cast<BotRole>(roleOverride);
            specId = SpecIdForRole(bot->getClass(), role);
        }
        else
        {
            specId = bot->GetTalentSpecialization(bot->GetActiveSpec());
            if (!specId)
                specId = SpecIdForRole(bot->getClass(), BOT_ROLE_DPS);
            role = RoleFromSpec(bot->getClass(), specId);
        }

        if (specId)
            bot->LearnSpecialization(specId);

        BotRotation::ApplyRecommendedTalents(bot);
        BotRotation::ApplyRecommendedGlyphs(bot);
    }
    else if (roleOverride >= 0)
    {
        role = static_cast<BotRole>(roleOverride);
    }

    // Armor/weapon skills for this level, then trainer spells, riding/mounts,
    // then gear. Re-running init after a delevel simply skips plate/mail and
    // higher riding tiers the bot is no longer high enough for.
    LearnArmorProficiencies(bot);
    LearnClassTrainerSpells(bot);
    LearnLevelClassSpells(bot, specId);
    LearnRidingAndMounts(bot);

    // Gear the bot for the (possibly newly assigned) role/spec at its level.
    int const qualityCap = maxItemQuality < 0
        ? _gearMaxQuality
        : std::max(0, std::min(maxItemQuality, int(ITEM_QUALITY_LEGENDARY)));
    int qualityFloor = _gearMinQuality;
    if (maxItemQuality >= 0)
        qualityFloor = std::min(qualityFloor, qualityCap);
    if (qualityFloor > qualityCap)
        qualityFloor = qualityCap;

    GearBotSets(bot, role, specId, qualityFloor, qualityCap);
    GiveRestConsumables(bot);
    FillHunterPetSlots(bot);

    // After relevel + gear so the destination matches the bot's final level.
    // Levels 1–5: auto skips; player `tele` resets to homebind/spawn.
    if (teleport)
        BotTeleportMaps::TeleportForLevel(bot, teleportFromCommand);

    bot->SaveToDB();

    if (auto it = _ai.find(bot->GetGUID()); it != _ai.end() && it->second)
    {
        it->second->ResetStrategiesToRoleDefaults();
        it->second->ResetGearSet();
        it->second->AfterInitRelocate(teleport);
    }

    SF_LOG_INFO("modules",
        "[mod-playerbots] Initialized bot '%s': level %u %s %s, spec %s (%s), gear %s-%s%s.",
        bot->GetName().c_str(), bot->getLevel(),
        SafeRaceName(bot->getRace()), SafeClassName(bot->getClass()),
        SpecName(specId), RoleName(role), QualityName(qualityFloor), QualityName(qualityCap),
        teleport ? ", teleported" : "");
}

namespace
{
    // The spec/role init keeps when given no override. Below level 10 there is
    // no spec yet: damage role, spec 0.
    uint32 CurrentSpecAndRole(Player* bot, BotRole& role)
    {
        role = BOT_ROLE_DPS;
        if (bot->getLevel() < 10)
            return 0;

        uint32 specId = bot->GetTalentSpecialization(bot->GetActiveSpec());
        if (!specId)
            specId = SpecIdForRole(bot->getClass(), BOT_ROLE_DPS);
        role = RoleFromSpec(bot->getClass(), specId);
        return specId;
    }

    uint32 CountEquippedGear(Player* bot)
    {
        uint32 count = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD
                && bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                ++count;
        return count;
    }
}

bool PlayerbotMgr::IsAltBot(Player* bot) const
{
    if (!bot || IsRandomBot(bot->GetGUID()))
        return false;

    uint32 const accountId = bot->GetSession() ? bot->GetSession()->GetAccountId() : 0;
    for (BotCandidate const& candidate : _candidates)
        if (candidate.accountId == accountId)
            return false;
    return true;
}

uint32 PlayerbotMgr::AutoGearBot(Player* bot, int maxItemQuality, uint32 maxItemLevel, bool reset)
{
    if (!bot || !bot->IsInWorld())
        return 0;

    BotRole role = BOT_ROLE_DPS;
    uint32 const specId = CurrentSpecAndRole(bot, role);

    int const qualityCap = std::max(0, std::min(maxItemQuality, int(ITEM_QUALITY_LEGENDARY)));
    int const qualityFloor = std::min(_gearMinQuality, qualityCap);

    // Proficiencies only grow with level; a bot that levelled since its last
    // init may now wear plate/mail or a new weapon type.
    LearnArmorProficiencies(bot);

    uint32 result = 0;
    {
        ScopedGearRoll roll(maxItemLevel, !reset);
        if (reset)
        {
            GearBotSets(bot, role, specId, qualityFloor, qualityCap);
            result = CountEquippedGear(bot);
        }
        else
        {
            // Upgrade the PvE set; the stored PvP set is only re-rolled by reset.
            BotGearSets::WearSet(bot, false);
            ScopedGearPool pve(GearPool::PveOnly);
            GearBot(bot, role, specId, qualityFloor, qualityCap);
            result = g_gearRoll.changed;
        }
    }

    bot->SaveToDB();

    if (auto it = _ai.find(bot->GetGUID()); it != _ai.end() && it->second)
        it->second->ResetGearSet();

    SF_LOG_INFO("modules",
        "[mod-playerbots] Autogear '%s' (%s): spec %s, gear %s-%s, item level cap %u, %u slot(s) %s.",
        bot->GetName().c_str(), reset ? "reset" : "upgrade", SpecName(specId),
        QualityName(qualityFloor), QualityName(qualityCap), maxItemLevel, result,
        reset ? "equipped" : "upgraded");
    return result;
}

void PlayerbotMgr::MaintainBot(Player* bot)
{
    if (!bot || !bot->IsInWorld())
        return;

    // Alt bots are real players' characters; each part can be switched off.
    bool const alt = IsAltBot(bot);
    auto allowed = [alt](bool altSetting) { return !alt || altSetting; };

    BotRole role = BOT_ROLE_DPS;
    uint32 const specId = CurrentSpecAndRole(bot, role);
    // Keep the bot's spec; only pick one for a bot that never had one.
    if (specId && !bot->GetTalentSpecialization(bot->GetActiveSpec()))
        bot->LearnSpecialization(specId);

    if (allowed(_altMaintenanceTalents))
        BotRotation::ApplyRecommendedTalents(bot);
    if (allowed(_altMaintenanceGlyphs))
        BotRotation::ApplyRecommendedGlyphs(bot);
    if (allowed(_altMaintenanceSpells))
    {
        LearnArmorProficiencies(bot);
        LearnClassTrainerSpells(bot);
        LearnLevelClassSpells(bot, specId);
    }
    if (allowed(_altMaintenanceMounts))
        LearnRidingAndMounts(bot);
    if (allowed(_altMaintenancePet))
        FillHunterPetSlots(bot);

    bot->DurabilityRepairAll(false, 1.0f, false);
    bot->SendTalentsInfoData();
    bot->SaveToDB();

    SF_LOG_INFO("modules", "[mod-playerbots] Maintained bot '%s' (level %u %s, spec %s%s).",
        bot->GetName().c_str(), bot->getLevel(), SafeClassName(bot->getClass()), SpecName(specId),
        alt ? ", alt bot" : "");
}

uint32 PlayerbotMgr::EnqueueInitializeAllBots(int roleOverride, uint32 specOverride,
    int maxItemQuality, bool relevel, bool teleport, uint64 notifyPlayerGuid)
{
    uint32 added = 0;
    for (auto const& pair : _bots)
    {
        WorldSession* session = pair.second;
        Player* bot = session ? session->GetPlayer() : nullptr;
        if (!bot || !bot->IsInWorld())
            continue;
        if (specOverride)
        {
            ChrSpecializationEntry const* entry = sChrSpecializationStore.LookupEntry(specOverride);
            if (!entry || entry->classId != bot->getClass())
                continue;
        }
        // Queued from `.playerbots init` — tele token is an explicit command.
        UpsertInitQueueJob(bot->GetGUID(), roleOverride, specOverride, maxItemQuality,
            relevel, teleport, teleport);
        ++added;
    }

    if (added)
    {
        if (!_initQueueBatchTotal)
            _initQueueBatchDone = 0;
        _initQueueBatchTotal = uint32(_initQueue.size());
        if (notifyPlayerGuid)
            _initQueueNotifyGuid = notifyPlayerGuid;
        SF_LOG_INFO("modules",
            "[mod-playerbots] Init queue: %u bot(s) queued (%u/tick).",
            added, _initPerTick);
    }
    return added;
}

bool PlayerbotMgr::EnqueueInitializeBot(uint64 characterGuid, int roleOverride,
    uint32 specOverride, int maxItemQuality, bool relevel, bool teleport,
    uint64 notifyPlayerGuid)
{
    Player* bot = ObjectAccessor::FindPlayer(characterGuid);
    if (!bot || !bot->IsInWorld())
        return false;
    if (!IsBot(characterGuid) && !IsSelfBot(characterGuid))
        return false;
    if (specOverride)
    {
        ChrSpecializationEntry const* entry = sChrSpecializationStore.LookupEntry(specOverride);
        if (!entry || entry->classId != bot->getClass())
            return false;
    }

    UpsertInitQueueJob(characterGuid, roleOverride, specOverride, maxItemQuality,
        relevel, teleport, teleport);
    if (!_initQueueBatchTotal)
        _initQueueBatchDone = 0;
    _initQueueBatchTotal = uint32(_initQueue.size());
    if (notifyPlayerGuid)
        _initQueueNotifyGuid = notifyPlayerGuid;
    return true;
}

void PlayerbotMgr::UpsertInitQueueJob(uint64 guid, int roleOverride, uint32 specOverride,
    int maxItemQuality, bool relevel, bool teleport, bool teleportFromCommand)
{
    for (QueuedBotInit& job : _initQueue)
    {
        if (job.guid != guid)
            continue;
        job.roleOverride = roleOverride;
        job.specOverride = specOverride;
        job.maxItemQuality = maxItemQuality;
        job.relevel = relevel;
        job.teleport = teleport;
        job.teleportFromCommand = teleportFromCommand;
        return;
    }

    QueuedBotInit job;
    job.guid = guid;
    job.roleOverride = roleOverride;
    job.specOverride = specOverride;
    job.maxItemQuality = maxItemQuality;
    job.relevel = relevel;
    job.teleport = teleport;
    job.teleportFromCommand = teleportFromCommand;
    _initQueue.push_back(job);
}

void PlayerbotMgr::ProcessInitQueue()
{
    if (_initQueue.empty())
        return;

    uint32 processed = 0;
    while (!_initQueue.empty() && processed < _initPerTick)
    {
        QueuedBotInit job = _initQueue.front();
        _initQueue.pop_front();
        ++processed;
        ++_initQueueBatchDone;

        Player* bot = ObjectAccessor::FindPlayer(job.guid);
        if (!bot || !bot->IsInWorld())
            continue;
        if (!IsBot(job.guid) && !IsSelfBot(job.guid))
            continue;
        if (job.specOverride)
        {
            ChrSpecializationEntry const* entry = sChrSpecializationStore.LookupEntry(job.specOverride);
            if (!entry || entry->classId != bot->getClass())
                continue;
        }

        InitializeBot(bot, job.roleOverride, job.specOverride, job.maxItemQuality,
            job.relevel, job.teleport, job.teleportFromCommand);
    }

    if ((_initQueueBatchDone % 25u) == 0u || _initQueue.empty())
        SF_LOG_INFO("modules", "[mod-playerbots] Init queue progress: %u done, %u remaining.",
            _initQueueBatchDone, uint32(_initQueue.size()));

    if (!_initQueue.empty())
        return;

    uint32 const done = _initQueueBatchDone;
    uint64 const notify = _initQueueNotifyGuid;
    _initQueueBatchTotal = 0;
    _initQueueBatchDone = 0;
    _initQueueNotifyGuid = 0;

    SF_LOG_INFO("modules", "[mod-playerbots] Init queue finished (%u bot(s)).", done);
    if (notify)
    {
        if (Player* player = ObjectAccessor::FindPlayer(notify))
            if (WorldSession* session = player->GetSession())
                ChatHandler(session).PSendSysMessage(
                    "Playerbots init queue finished (%u bot(s)).", done);
    }
}

void PlayerbotMgr::EnqueueUngearedOnlineBots(uint32 diff)
{
    if (_bots.empty())
        return;

    _ungearedSweepTimer += diff;
    if (_ungearedSweepTimer < 5000)
        return;
    _ungearedSweepTimer = 0;

    auto alreadyQueued = [this](uint64 guid) -> bool
    {
        for (QueuedBotInit const& job : _initQueue)
            if (job.guid == guid)
                return true;
        return false;
    };

    uint32 queued = 0;
    for (auto const& pair : _bots)
    {
        Player* bot = pair.second ? pair.second->GetPlayer() : nullptr;
        if (!bot || !bot->IsInWorld())
            continue;
        if (alreadyQueued(pair.first))
            continue;

        auto deferred = _deferredInits.find(pair.first);
        if (deferred != _deferredInits.end())
        {
            PendingBotInit const& d = deferred->second;
            UpsertInitQueueJob(pair.first, d.role, d.specId, -1, false,
                _autoTeleportOnInit, false);
            _deferredInits.erase(deferred);
            ++queued;
            continue;
        }

        if (!BotNeedsGearInit(bot))
            continue;

        UpsertInitQueueJob(pair.first, -1, 0, -1, false, _autoTeleportOnInit, false);
        ++queued;
    }

    if (queued)
        SF_LOG_INFO("modules",
            "[mod-playerbots] Queued %u online ungeared bot(s) for init+teleport.", queued);
}

uint32 PlayerbotMgr::PopulateAccount(uint32 accountId, std::vector<PendingBotInit>* pending)
{
    uint32 existing = AccountMgr::GetCharactersCount(accountId);
    uint32 created = 0;

    for (uint32 n = existing; n < _autoCharsPerAccount; ++n)
        if (CreateOneCharacter(accountId, pending))
            ++created;

    return created;
}

bool PlayerbotMgr::BotNeedsGearInit(Player* bot) const
{
    if (!bot)
        return false;
    if (_deferredInits.find(bot->GetGUID()) != _deferredInits.end())
        return true;

    // Ungeared after deferred create — chest and main-hand both empty.
    if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_CHEST)
        && !bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
        return true;

    // Starter whites only — still needs a real gear/spec init pass.
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;
        ItemTemplate const* proto = item->GetTemplate();
        if (proto && proto->Quality >= ITEM_QUALITY_UNCOMMON)
            return false;
    }
    return true;
}

void PlayerbotMgr::InitCreatedBot(PendingBotInit const& pending)
{
    std::string spawnError;
    if (!SpawnBot(pending.guid, false, &spawnError, pending.accountId))
    {
        SF_LOG_ERROR("modules", "[mod-playerbots] Created GUID %u but could not auto-init: %s",
            GUID_LOPART(pending.guid), spawnError.c_str());
        return;
    }
    if (auto it = _bots.find(pending.guid); it != _bots.end())
    {
        if (Player* bot = it->second ? it->second->GetPlayer() : nullptr)
            InitializeBot(bot, pending.role, pending.specId, -1, false, _autoTeleportOnInit);
    }
    RemoveBot(pending.guid);
}

bool PlayerbotMgr::CreateOneCharacter(uint32 accountId, std::vector<PendingBotInit>* pending)
{
    bool alliance = RollPct() < _autoAlliancePct;

    uint32 roll = RollPct();
    BotRole role;
    if (roll < _autoTankPct)
        role = BOT_ROLE_TANK;
    else if (roll < _autoTankPct + _autoHealerPct)
        role = BOT_ROLE_HEALER;
    else
        role = BOT_ROLE_DPS;

    uint8 cls = 0;
    uint8 race = 0;
    for (int attempt = 0; attempt < 20 && !race; ++attempt)
    {
        cls = PickClassForRole(role);
        race = PickRaceForClassFaction(cls, alliance);
    }

    if (!race || !cls)
    {
        SF_LOG_ERROR("modules", "[mod-playerbots] No valid race/class combo for account %u.", accountId);
        return false;
    }

    uint8 gender = uint8(std::rand() % 2);

    std::string name;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        std::string candidate = GenerateBotName();
        if (ObjectMgr::CheckPlayerName(candidate, true) != ResponseCodes::CHAR_NAME_SUCCESS)
            continue;
        if (sObjectMgr->GetPlayerGUIDByName(candidate))
            continue;
        name = candidate;
        break;
    }

    if (name.empty())
    {
        SF_LOG_ERROR("modules", "[mod-playerbots] Could not generate a unique name for account %u.", accountId);
        return false;
    }

    uint8 expansion = uint8(sWorld->getIntConfig(WorldIntConfigs::CONFIG_EXPANSION));
    uint32 maxLevel = sWorld->getIntConfig(WorldIntConfigs::CONFIG_MAX_PLAYER_LEVEL);
    uint32 minL = std::min(_autoMinLevel, maxLevel);
    uint32 maxL = std::min(_autoMaxLevel, maxLevel);
    if (maxL < minL)
        std::swap(minL, maxL);
    uint8 level = uint8(minL);
    if (maxL > minL)
        level = uint8(minL + uint32(std::rand() % int(maxL - minL + 1)));

    WorldSession* sess = new WorldSession(accountId, nullptr, AccountTypes::SEC_PLAYER, expansion,
        0, LOCALE_enUS, 0, false, false, false, false, std::string(), SessionKey{});
    sess->SetBot(true);
    // Match real characters (global realmID is set at world startup from RealmID
    // conf or auth.realmlist). Never persist the unset sentinel 0xFFFFFFFF.
    uint32 botRealm = realmID;
    if (!botRealm || botRealm == uint32(-1))
        botRealm = 1;
    sess->SetVirtualRealmID(botRealm);

    uint32 specId = SpecIdForRole(cls, role);
    uint32 guid = sess->CreateBotCharacter(name, race, cls, gender, 0, 0, 0, 0, 0, level, specId);

    delete sess;

    if (!guid)
    {
        SF_LOG_ERROR("modules", "[mod-playerbots] CreateBotCharacter failed (account %u, race %u, class %u).",
            accountId, race, cls);
        return false;
    }

    SF_LOG_INFO("modules", "[mod-playerbots] Created bot '%s' (GUID %u, %s, race %u, class %u, level %u) on account %u.",
        name.c_str(), guid, alliance ? "Alliance" : "Horde", race, cls, level, accountId);

    if (pending)
    {
        PendingBotInit p;
        p.guid = MAKE_NEW_GUID(guid, 0, HIGHGUID_PLAYER);
        p.accountId = accountId;
        p.role = int(role);
        p.specId = specId;
        pending->push_back(p);
    }

    return true;
}
