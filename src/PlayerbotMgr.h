/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* Playerbots module - central configuration/state manager.
*
* Phase 1: socketless bot WorldSessions + character login (async pool path;
*          sync LoginBotCharacter for single-bot add/create).
* Phase 2: random-bot pool sourced from dedicated bot accounts, managed by a
*          throttled update loop (bot AI still arrives in Phase 3, see PORTING.md).
*/

#ifndef _SF_PLAYERBOT_MGR_H
#define _SF_PLAYERBOT_MGR_H

#include "Define.h"
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class WorldSession;
class Player;
class PlayerbotAI;
class Group;

class PlayerbotMgr
{
public:
    static PlayerbotMgr* instance();

    // (Re)reads module settings from the worldserver configuration.
    void LoadConfig();

    // Called every world tick (from the module's WorldScript::OnUpdate).
    void Update(uint32 diff);

    // Ticks a single bot's AI (from PlayerScript::OnUpdate).
    void UpdateBotAI(Player* bot, uint32 diff);
    // Player object is going away — drop self-bot / AI so a relog cannot tick
    // a dangling _bot (GetUInt64Value on a destroyed Object with valuesCount 0).
    void OnPlayerLogout(Player* player);

    // Real player tick: align in-range socket bots to this player's MoP phases
    // and refresh client visibility (ungrouped bots were invisible until invite).
    void SyncNearbyBotVisibility(Player* viewer, uint32 diff);

    // Routes a chat order to one bot (whisper) or every bot in a group (party/raid).
    void HandleBotWhisper(Player* from, Player* bot, std::string const& msg);
    void HandleBotGroupChat(Player* from, Group* group, std::string const& msg);

    bool IsEnabled() const { return _enabled; }
    bool IsRandomBotsEnabled() const { return _randomBotsEnabled; }
    uint32 GetMaxRandomBots() const { return _maxRandomBots; }

    // Logs the given character into the world as a manually-added bot.
    bool AddBot(uint64 characterGuid, std::string* errorOut = nullptr);
    // Logs the given bot character out of the world and frees its session.
    bool RemoveBot(uint64 characterGuid);
    bool IsBot(uint64 characterGuid) const { return _bots.find(characterGuid) != _bots.end(); }
    // True if this GUID has an attached PlayerbotAI (socket bot or self-bot).
    bool HasBotAI(uint64 characterGuid) const { return _ai.find(characterGuid) != _ai.end(); }
    PlayerbotAI* GetBotAI(uint64 characterGuid) const;
    PlayerbotAI* GetBotAI(Player* bot) const;
    bool IsSelfBot(uint64 characterGuid) const { return _selfBots.find(characterGuid) != _selfBots.end(); }

    // Attach/detach AI to a real logged-in player (client keeps movement).
    bool AttachSelfBot(Player* player, std::string* errorOut = nullptr);
    bool DetachSelfBot(Player* player);
    bool ToggleSelfBot(Player* player, std::string* report = nullptr);

    // Logs out and frees every managed bot (used on world shutdown).
    void LogoutAllBots();

    // Forces the candidate bot-character pool to be reloaded on the next tick.
    void ReloadCandidates() { _candidatesLoaded = false; }

    bool IsAutoCreateOnStartup() const { return _autoCreateOnStartup; }
    bool ShouldDeleteRandomBotAccounts() const { return _deleteRandomBotAccounts; }
    std::string const& GetAccountPrefix() const { return _accountPrefix; }
    int GetGearMinQuality() const { return _gearMinQuality; }
    int GetGearMaxQuality() const { return _gearMaxQuality; }

    float GetRestHealthPct() const { return _restHealthPct; }
    float GetRestManaPct() const { return _restManaPct; }
    float GetAlmostFullHealthPct() const { return _almostFullHealthPct; }
    float GetMediumManaPct() const { return _mediumManaPct; }
    float GetThreatThrottlePct() const { return _threatThrottlePct; }
    float GetFleeDistance() const { return _fleeDistance; }
    bool IsFleeEnabled() const { return _fleeEnabled; }
    float GetSaveManaThreshold() const { return _saveManaThreshold; }
    uint32 GetWaitForAttackSeconds() const { return _waitForAttackSeconds; }
    bool GetVendorSellGreens() const { return _vendorSellGreens; }
    float GetVendorHubMaxDistance() const { return _vendorHubMaxDistance; }
    bool GetQuestAutoPickReward() const { return _questAutoPickReward; }
    bool IsRandomBot(uint64 characterGuid) const { return _randomBots.count(characterGuid) > 0; }

    // Provisions bot accounts (prefix + n) and fills them with characters using
    // the configured faction/role ratios and level range. Incremental: existing
    // accounts/characters are reused. Returns the number of characters
    // created; a human-readable summary is appended to *report when provided.
    // allowInitOnCreate=false defers gear/teleport to the per-tick init queue
    // even when InitOnCreate is set (startup must not block the world loop).
    uint32 CreateBotPopulation(std::string* report = nullptr, bool allowInitOnCreate = true);

    // Resets saved bot positions that sit on a map other than their homebind
    // while holding coordinates from the homebind's map (or a missing tile).
    // Run before any bot logs in. Returns the number of characters moved home.
    uint32 RepairStrandedBotRows();

    // Fully deletes characters whose account no longer exists in auth
    // (account 0 soft-deletes excluded). Returns the number deleted.
    uint32 DeleteOrphanCharacters();
    bool ShouldDeleteOrphanCharacters() const { return _deleteOrphanCharacters; }

    // Logs out prefix bots and deletes all accounts/characters matching
    // AccountPrefix. Returns accounts deleted. Refuses unsafe prefixes.
    uint32 DeleteBotAccounts(std::string* report = nullptr);
    bool CanDeleteBotAccounts(std::string* errorOut = nullptr) const;

    // Re-applies derived state (specialization/spells, gear) to a single active
    // bot. Safe to call repeatedly.
    // roleOverride: -1 keep current role mapping, otherwise 0 = tank, 1 = healer,
    // 2 = damage (default DPS tab for the class).
    // specOverride: if non-zero, force that ChrSpecialization id (takes priority).
    // maxItemQuality: ITEM_QUALITY_* cap for gear rolls; -1 = use conf Gear.MaxQuality.
    // relevel: when true, roll a new level in AutoCreate.MinLevel–MaxLevel first
    // (default false keeps the bot's current level).
    // teleport: after gear (and relevel), place the bot via TeleportForLevel.
    // teleportFromCommand: true for `.playerbots init … tele` (levels 1–5 go
    // homebind); false for AutoCreate.TeleportOnInit (levels 1–5 stay put).
    void InitializeBot(Player* bot, int roleOverride = -1, uint32 specOverride = 0,
        int maxItemQuality = -1, bool relevel = false, bool teleport = false,
        bool teleportFromCommand = false);
    // Queue init for every active socket bot (processed over world ticks).
    // Returns how many jobs were queued. notifyPlayerGuid gets a done message.
    uint32 EnqueueInitializeAllBots(int roleOverride = -1, uint32 specOverride = 0,
        int maxItemQuality = -1, bool relevel = false, bool teleport = false,
        uint64 notifyPlayerGuid = 0);
    // Queue a single online bot (or no-op if offline). Returns true if queued.
    bool EnqueueInitializeBot(uint64 characterGuid, int roleOverride = -1,
        uint32 specOverride = 0, int maxItemQuality = -1, bool relevel = false,
        bool teleport = false, uint64 notifyPlayerGuid = 0);
    uint32 GetInitQueueSize() const { return uint32(_initQueue.size()); }

    // AC `autogear`: re-gears for the bot's current spec, leaving spells and
    // talents alone. reset=false upgrades the worn PvE set slot by slot (a slot
    // changes only for a higher item level; the old piece goes to the bags);
    // reset=true wipes and re-rolls both sets like init. maxItemQuality is an
    // ITEM_QUALITY_* cap, maxItemLevel 0 = none. Returns the slots upgraded
    // (reset: slots equipped).
    uint32 AutoGearBot(Player* bot, int maxItemQuality, uint32 maxItemLevel, bool reset);
    // AC `maintenance`: refreshes spells, proficiencies, talents, glyphs,
    // riding/mounts and hunter pets, then repairs. Keeps spec and gear.
    // Alt bots skip the parts their Playerbots.AltMaintenance.* key disables.
    void MaintainBot(Player* bot);
    // Not a random-pool bot and not on a bot (AccountPrefix) account.
    bool IsAltBot(Player* bot) const;

    bool IsAutoGearCommandEnabled() const { return _autoGearCommand; }
    bool IsAutoGearAltBotsEnabled() const { return _autoGearAltBots; }
    int GetAutoGearQualityLimit() const { return _autoGearQualityLimit; }
    uint32 GetAutoGearItemLevelLimit() const { return _autoGearItemLevelLimit; }
    bool IsMaintenanceCommandEnabled() const { return _maintenanceCommand; }
    uint32 GetInitPerTick() const { return _initPerTick; }

    // Configured AutoCreate level band (also used by init +relevel).
    uint32 GetAutoMinLevel() const { return _autoMinLevel; }
    uint32 GetAutoMaxLevel() const { return _autoMaxLevel; }

    bool IsActivityEnabled() const { return _activityEnabled; }
    float GetActivityActiveRange() const { return _activityActiveRange; }
    uint32 GetActivitySlowInterval() const { return _activitySlowInterval; }
    // Counts socket bots by the activity Map::Update is currently honoring.
    void GetBotActivityCounts(uint32& full, uint32& slow, uint32& dormant) const;

    uint32 GetActiveBotCount() const { return uint32(_bots.size()); }
    uint32 GetRandomBotCount() const { return uint32(_randomBots.size()); }
    uint32 GetCandidateCount() const { return uint32(_candidates.size()); }
    uint32 GetPendingLoginCount() const { return uint32(_pendingLogins.size()); }
    uint32 GetLoginsPerTick() const { return _loginsPerTick; }
    uint32 GetMaxPendingLogins() const { return _maxPendingLogins; }
    void GetBotGuids(std::vector<uint64>& out) const;

private:
    PlayerbotMgr() = default;
    ~PlayerbotMgr() = default;
    PlayerbotMgr(PlayerbotMgr const&) = delete;
    PlayerbotMgr& operator=(PlayerbotMgr const&) = delete;

    struct BotCandidate
    {
        uint64 guid = 0;
        uint32 accountId = 0;
    };

    struct PendingBotLogin
    {
        WorldSession* session = nullptr;
        uint64 characterGuid = 0;
        bool isRandom = false;
        uint32 botRealm = 0;
        uint32 startedMs = 0;
        bool discard = false;
    };

    // Shared bot spawn path. isRandom marks the bot as pool-managed.
    // accountIdOverride skips the GUID→account DB lookup (needed right after
    // async character create, before the INSERT is visible to sync queries).
    bool SpawnBot(uint64 characterGuid, bool isRandom, std::string* errorOut,
        uint32 accountIdOverride = 0);
    // Starts async login for the random pool (no world-thread DB spin-wait).
    bool BeginSpawnBot(uint64 characterGuid, bool isRandom, uint32 accountId,
        std::string* errorOut);
    void ProcessPendingLogins();
    void FinishSpawnBot(PendingBotLogin& pending);
    void AbortPendingLogin(PendingBotLogin& pending);
    uint32 ResolveBotVirtualRealm();
    void LoadCandidates();
    void TrySpawnRandomBot();
    void CleanupDeadBots();
    void DestroyBotAI(uint64 characterGuid);
    // When a real player is solo-queued for LFG, queue level-matched bots too.
    void UpdateLfgAutoJoin(uint32 diff);
    // Full / slow / dormant from real-player positions. Applies next map tick.
    void UpdateBotActivity();
    // Stop movement and drop combat when a bot leaves full speed.
    void QuiesceBot(Player* bot);
    void ProcessInitQueue();
    void UpsertInitQueueJob(uint64 guid, int roleOverride, uint32 specOverride,
        int maxItemQuality, bool relevel, bool teleport, bool teleportFromCommand);
    // Periodically queue online bots that still only have starter whites.
    void EnqueueUngearedOnlineBots(uint32 diff);

    // Auto-creation helpers.
    struct PendingBotInit
    {
        uint64 guid = 0;
        uint32 accountId = 0;
        int role = 2;
        uint32 specId = 0;
    };

    uint32 PopulateAccount(uint32 accountId, std::vector<PendingBotInit>* pending);
    bool CreateOneCharacter(uint32 accountId, std::vector<PendingBotInit>* pending);
    void InitCreatedBot(PendingBotInit const& pending);
    bool BotNeedsGearInit(Player* bot) const;

    bool _enabled = false;
    bool _randomBotsEnabled = false;
    uint32 _maxRandomBots = 0;
    std::string _accountPrefix = "RNDBOT";
    // Delay between *starting* a new async login (not gear init).
    uint32 _loginIntervalMs = 250;
    // Cap in-flight login query holders for the random pool.
    uint32 _maxPendingLogins = 8;
    // Max HandlePlayerLogin (LoadFromDB + map add) completions per world tick.
    uint32 _loginsPerTick = 1;
    // Cached virtual realm for socketless sessions (avoids scanning players).
    uint32 _botVirtualRealm = 0;

    // Auto-join LFG when a real player queues (AC-style fill).
    bool _joinLfg = false;
    uint32 _joinLfgMaxBots = 10;
    uint32 _joinLfgLevelRange = 5;   // |botLevel - playerLevel| must be <= this
    uint32 _lfgJoinTimer = 0;

    // Auto-creation settings.
    bool _autoCreateOnStartup = false;
    bool _deleteRandomBotAccounts = false;
    bool _deleteOrphanCharacters = true;
    uint32 _autoAccountCount = 0;
    std::string _autoPassword = "password";
    uint32 _autoCharsPerAccount = 1;
    uint32 _autoAlliancePct = 50;
    uint32 _autoTankPct = 20;
    uint32 _autoHealerPct = 20;
    uint32 _autoMinLevel = 1;
    uint32 _autoMaxLevel = 1;
    // When false (default), create skips world login/gear — first SpawnBot inits.
    // Set true to gear every new char immediately (much slower for large pools).
    bool _autoInitOnCreate = false;
    // Scatter to a level/faction-safe anchor after first-time gear init
    // (InitOnCreate and deferred first-login). Manual `.playerbots init` still
    // needs an explicit `tele` token.
    bool _autoTeleportOnInit = true;

    // Gear quality band for InitializeBot / create auto-init (ITEM_QUALITY_*).
    int _gearMinQuality = 2; // uncommon
    int _gearMaxQuality = 4; // epic

    // `autogear` / `maintenance` chat commands (AC AutoGear* / *Maintenance*).
    bool _autoGearCommand = true;
    bool _autoGearAltBots = true;
    int _autoGearQualityLimit = 4;    // epic, matches Gear.MaxQuality
    uint32 _autoGearItemLevelLimit = 0; // 0 = no limit
    bool _maintenanceCommand = true;
    bool _altMaintenanceSpells = true;
    bool _altMaintenanceTalents = true;
    bool _altMaintenanceGlyphs = true;
    bool _altMaintenanceMounts = true;
    bool _altMaintenancePet = true;

    // Rest / save-mana numeric thresholds only (enable/disable is co/nc runtime).
    float _restHealthPct = 50.0f;
    float _restManaPct = 50.0f;
    float _almostFullHealthPct = 85.0f; // party holds follow until allies reach this
    float _mediumManaPct = 40.0f;       // mana users must reach this before "ready"
    float _threatThrottlePct = 80.0f;   // DPS pause when threat >= this % of tank/top
    float _fleeDistance = 20.0f;        // combat kite / flee sample radius
    bool _fleeEnabled = true;
    float _saveManaThreshold = 60.0f;
    uint32 _waitForAttackSeconds = 5;
    bool _vendorSellGreens = true;
    float _vendorHubMaxDistance = 500.0f;
    bool _questAutoPickReward = true;

    // Empty maps freeze bots; same map but far away idles on a slow heartbeat.
    bool _activityEnabled = true;
    float _activityActiveRange = 0.0f; // 0 = that map's visibility range
    uint32 _activitySlowInterval = 5000;

    // Background `.playerbots init` work — applied a few bots per world tick.
    struct QueuedBotInit
    {
        uint64 guid = 0;
        int roleOverride = -1;
        uint32 specOverride = 0;
        int maxItemQuality = -1;
        bool relevel = false;
        bool teleport = false;
        bool teleportFromCommand = false;
    };
    std::deque<QueuedBotInit> _initQueue;
    uint32 _initPerTick = 5;
    uint32 _initQueueBatchTotal = 0;
    uint32 _initQueueBatchDone = 0;
    uint64 _initQueueNotifyGuid = 0;
    uint32 _ungearedSweepTimer = 0;

    uint32 _loginTimer = 0;
    bool _candidatesLoaded = false;
    std::vector<BotCandidate> _candidates;
    std::deque<PendingBotLogin> _pendingLogins;
    std::unordered_set<uint64> _pendingLoginGuids;
    // Role/spec remembered at create when InitOnCreate=0; applied on first login.
    std::unordered_map<uint64, PendingBotInit> _deferredInits;

    std::unordered_map<uint64 /*characterGuid*/, WorldSession*> _bots;
    std::unordered_map<uint64 /*characterGuid*/, PlayerbotAI*> _ai;
    std::unordered_set<uint64 /*characterGuid*/> _randomBots; // subset managed by the pool
    std::unordered_set<uint64 /*characterGuid*/> _selfBots;   // real players with cast-only AI
    std::unordered_map<uint64 /*playerGuid*/, uint32> _viewerVisTimers;
};

#define sPlayerbotMgr PlayerbotMgr::instance()

#endif // _SF_PLAYERBOT_MGR_H
