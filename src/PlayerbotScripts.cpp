/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* Playerbots module - core script hooks and admin commands.
*/

#include "PlayerbotMgr.h"
#include "BotPreferredMounts.h"
#include "BotVendorHubs.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupReference.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

// Routes per-tick AI updates and chat orders to the PlayerbotMgr for
// bot-controlled players.
class playerbot_player_script : public PlayerScript
{
public:
    playerbot_player_script() : PlayerScript("playerbot_player_script") { }

    void OnUpdate(Player* player, uint32 diff) override
    {
        if (!player || !player->GetSession())
            return;

        // Socket bots and self-bots both have an attached PlayerbotAI.
        if (player->GetSession()->IsBot() || sPlayerbotMgr->HasBotAI(player->GetGUID()))
            sPlayerbotMgr->UpdateBotAI(player, diff);

        // Real players: force nearby socket bots onto our phase set. Invite was
        // the only path that did this before, so ungrouped bots stayed invisible.
        if (!player->GetSession()->IsBot() && player->IsInWorld())
            sPlayerbotMgr->SyncNearbyBotVisibility(player, diff);
    }

    void OnLogout(Player* player) override
    {
        sPlayerbotMgr->OnPlayerLogout(player);
    }

    // Whisper directed at a bot or self-bot (including whispering yourself).
    void OnChat(Player* player, ChatMsg /*type*/, Language /*lang*/, std::string& msg, Player* receiver) override
    {
        if (!sPlayerbotMgr->IsEnabled() || !player || !receiver)
            return;
        if (player->GetSession() && player->GetSession()->IsBot())
            return;
        // Socket bots and self-bots both have AI. Allow from == receiver so a
        // self-bot can /w Themselves co ? and get a reply.
        if (!sPlayerbotMgr->HasBotAI(receiver->GetGUID()))
            return;
        // Ignore status-style replies ("co: ..." / "nc: ...") so a self-whisper
        // ack can never re-enter strategy handling.
        if (msg.size() >= 3 && (msg.compare(0, 3, "co:") == 0 || msg.compare(0, 3, "nc:") == 0))
            return;

        sPlayerbotMgr->HandleBotWhisper(player, receiver, msg);
    }

    // Party / raid chat: every bot in the group hears the order.
    void OnChat(Player* player, ChatMsg /*type*/, Language /*lang*/, std::string& msg, Group* group) override
    {
        if (!sPlayerbotMgr->IsEnabled() || !player || !group)
            return;
        if (player->GetSession() && player->GetSession()->IsBot())
            return;

        sPlayerbotMgr->HandleBotGroupChat(player, group, msg);
    }

    // When a real player (or self-bot) accepts a quest, mirror it into grouped
    // bots' logs so they share kill credit while staying on follow (no travel).
    void OnQuestAdd(Player* player, Quest const* quest) override
    {
        if (!sPlayerbotMgr->IsEnabled() || !player || !quest)
            return;
        // Socket-bot AddQuest echoes this hook — skip to avoid loops.
        if (!player->GetSession() || player->GetSession()->IsBot())
            return;

        Group* group = player->GetGroup();
        if (!group)
            return;

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == player || !member->IsInWorld())
                continue;
            if (member->GetMapId() != player->GetMapId())
                continue;
            if (PlayerbotAI* ai = sPlayerbotMgr->GetBotAI(member))
                ai->MirrorQuestAccept(player, quest);
        }
    }

    // When a real player (or self-bot) turns a quest in, grouped bots on the same
    // map that have it complete turn it in too, so the next quest of a chain is
    // open to them and their log does not fill up with finished quests.
    void OnQuestReward(Player* player, Quest const* quest, Object* questGiver) override
    {
        if (!sPlayerbotMgr->IsEnabled() || !player || !quest)
            return;
        // A bot's own RewardQuest echoes this hook: skip to avoid loops.
        if (!player->GetSession() || player->GetSession()->IsBot())
            return;

        Group* group = player->GetGroup();
        if (!group)
            return;

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == player || !member->IsInWorld())
                continue;
            if (member->GetMapId() != player->GetMapId())
                continue;
            if (PlayerbotAI* ai = sPlayerbotMgr->GetBotAI(member))
                ai->MirrorQuestReward(player, quest, questGiver);
        }
    }
};

// Loads/refreshes the module configuration together with the world config and
// tears down bot sessions cleanly on shutdown.
class playerbot_world_script : public WorldScript
{
public:
    playerbot_world_script() : WorldScript("playerbot_world_script") { }

    void OnConfigLoad(bool /*reload*/) override
    {
        sPlayerbotMgr->LoadConfig();
    }

    void OnStartup() override
    {
        if (!sPlayerbotMgr->IsEnabled())
            return;

        // AC-style wipe: set DeleteRandomBotAccounts=1, start worldserver once,
        // then set it back to 0 and recreate.
        if (sPlayerbotMgr->ShouldDeleteRandomBotAccounts())
        {
            std::string report;
            sPlayerbotMgr->DeleteBotAccounts(&report);
            SF_LOG_INFO("modules", "[mod-playerbots] %s", report.c_str());
        }
        else
        {
            if (sPlayerbotMgr->ShouldDeleteOrphanCharacters())
                sPlayerbotMgr->DeleteOrphanCharacters();
            sPlayerbotMgr->RepairStrandedBotRows();
        }

        if (!sPlayerbotMgr->ShouldDeleteRandomBotAccounts() && sPlayerbotMgr->IsAutoCreateOnStartup())
        {
            // Optionally provision bot accounts/characters once the world is fully
            // loaded (all DBC/db data available for valid race/class/name checks).
            // OnStartup runs before the world loop, so sessions are not
            // accepted until it returns. Gear and teleport go to the per-tick
            // init queue instead of logging every new bot in here.
            std::string report;
            sPlayerbotMgr->CreateBotPopulation(&report, false);
            SF_LOG_INFO("modules", "[mod-playerbots] %s", report.c_str());
        }

        sBotVendorHubs->Load();
        sBotPreferredMounts->Load();
    }

    void OnUpdate(uint32 diff) override
    {
        sPlayerbotMgr->Update(diff);
    }

    void OnShutdown() override
    {
        sPlayerbotMgr->LogoutAllBots();
    }
};

// ".playerbots ..." administration commands.
class playerbot_commandscript : public CommandScript
{
public:
    playerbot_commandscript() : CommandScript("playerbot_commandscript") { }

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> playerbotCommandTable =
        {
            { "status", rbac::RBAC_PERM_COMMAND_GM, true, &HandlePlayerbotStatusCommand, "", },
            { "add",    rbac::RBAC_PERM_COMMAND_GM, true, &HandlePlayerbotAddCommand,    "", },
            { "remove", rbac::RBAC_PERM_COMMAND_GM, true, &HandlePlayerbotRemoveCommand, "", },
            { "summon", rbac::RBAC_PERM_COMMAND_GM, false, &HandlePlayerbotSummonCommand, "", },
            { "list",   rbac::RBAC_PERM_COMMAND_GM, true, &HandlePlayerbotListCommand,   "", },
            { "reload", rbac::RBAC_PERM_COMMAND_GM, true, &HandlePlayerbotReloadCommand, "", },
            { "create", rbac::RBAC_PERM_COMMAND_GM, true, &HandlePlayerbotCreateCommand, "", },
            { "wipe",   rbac::RBAC_PERM_COMMAND_GM, true, &HandlePlayerbotWipeCommand,   "", },
            { "init",   rbac::RBAC_PERM_COMMAND_GM, true, &HandlePlayerbotInitCommand,   "", },
            { "self",   rbac::RBAC_PERM_COMMAND_GM, false, &HandlePlayerbotSelfCommand,  "", },
        };

        static std::vector<ChatCommand> commandTable =
        {
            { "playerbots", rbac::RBAC_PERM_COMMAND_GM, true, NULL, "", playerbotCommandTable },
        };
        return commandTable;
    }

    static bool HandlePlayerbotStatusCommand(ChatHandler* handler, char const* /*args*/)
    {
        handler->PSendSysMessage("Playerbots module: %s.",
            sPlayerbotMgr->IsEnabled() ? "enabled" : "disabled");
        handler->PSendSysMessage("Random bots: %s (%u/%u, %u candidate(s), %u login pending).",
            sPlayerbotMgr->IsRandomBotsEnabled() ? "enabled" : "disabled",
            sPlayerbotMgr->GetRandomBotCount(), sPlayerbotMgr->GetMaxRandomBots(),
            sPlayerbotMgr->GetCandidateCount(), sPlayerbotMgr->GetPendingLoginCount());
        handler->PSendSysMessage("Login pacing: %u per tick, max %u pending (Init.PerTick=%u is gear only).",
            sPlayerbotMgr->GetLoginsPerTick(), sPlayerbotMgr->GetMaxPendingLogins(),
            sPlayerbotMgr->GetInitPerTick());
        handler->PSendSysMessage("Active bots: %u.", sPlayerbotMgr->GetActiveBotCount());
        uint32 full = 0;
        uint32 slow = 0;
        uint32 dormant = 0;
        sPlayerbotMgr->GetBotActivityCounts(full, slow, dormant);
        if (sPlayerbotMgr->IsActivityEnabled())
        {
            if (sPlayerbotMgr->GetActivityActiveRange() <= 0.0f)
                handler->PSendSysMessage("Activity throttle: on (full %u, slow %u, dormant %u, range visibility, slow every %u ms).",
                    full, slow, dormant, sPlayerbotMgr->GetActivitySlowInterval());
            else
                handler->PSendSysMessage("Activity throttle: on (full %u, slow %u, dormant %u, range %.0f yd, slow every %u ms).",
                    full, slow, dormant, sPlayerbotMgr->GetActivityActiveRange(), sPlayerbotMgr->GetActivitySlowInterval());
        }
        else
            handler->PSendSysMessage("Activity throttle: off (full %u, slow %u, dormant %u).",
                full, slow, dormant);
        return true;
    }

    static bool HandlePlayerbotAddCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        std::string name = args;
        if (!normalizePlayerName(name))
        {
            handler->SendSysMessage("Invalid character name.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint64 guid = sObjectMgr->GetPlayerGUIDByName(name);
        if (!guid)
        {
            handler->PSendSysMessage("Character '%s' not found.", name.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        // The login runs in the background; "Bot 'X' logged in." follows once the
        // character is loaded (the console gets nothing more).
        std::string error;
        Player* issuer = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!sPlayerbotMgr->AddBot(guid, &error, issuer ? issuer->GetGUID() : 0))
        {
            handler->PSendSysMessage("Could not add bot '%s': %s", name.c_str(), error.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage("Logging in bot '%s'...", name.c_str());
        return true;
    }

    static bool HandlePlayerbotRemoveCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        std::string name = args;
        if (!normalizePlayerName(name))
        {
            handler->SendSysMessage("Invalid character name.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint64 guid = sObjectMgr->GetPlayerGUIDByName(name);
        if (!guid || !sPlayerbotMgr->RemoveBot(guid))
        {
            handler->PSendSysMessage("'%s' is not an active bot.", name.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage("Bot '%s' logged out.", name.c_str());
        return true;
    }

    static bool HandlePlayerbotSummonCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* master = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!master)
        {
            handler->SendSysMessage("This command must be used in-game.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        Group* group = master->GetGroup();
        if (!group)
        {
            handler->SendSysMessage("You are not in a group with any bots.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 count = 0;
        for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == master)
                continue;
            if (!sPlayerbotMgr->IsBot(member->GetGUID()))
                continue;

            // TeleportToPlayer revives corpses (a dead bot's teleport is rejected
            // on map change, and the death timer sends them to the graveyard).
            if (PlayerbotAI* ai = sPlayerbotMgr->GetBotAI(member->GetGUID()))
            {
                if (ai->TeleportToPlayer(master))
                    ++count;
            }
        }

        handler->PSendSysMessage("Summoned %u bot(s) to your position.", count);
        return true;
    }

    static bool HandlePlayerbotReloadCommand(ChatHandler* handler, char const* /*args*/)
    {
        sPlayerbotMgr->LoadConfig();
        sPlayerbotMgr->ReloadCandidates();
        handler->SendSysMessage("Playerbots configuration and candidate pool reloaded.");
        return true;
    }

    static bool HandlePlayerbotCreateCommand(ChatHandler* handler, char const* /*args*/)
    {
        if (!sPlayerbotMgr->IsEnabled())
        {
            handler->SendSysMessage("Playerbots module is disabled (set Playerbots.Enable = 1).");
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->SendSysMessage("Provisioning bot accounts and characters; this may take a moment...");

        // Gear/teleport goes to the per-tick init queue: initing every new
        // character inline holds the world thread for minutes and the next
        // tick's diff idles out every real player's session.
        std::string report;
        sPlayerbotMgr->CreateBotPopulation(&report, false);
        handler->PSendSysMessage("%s", report.c_str());
        return true;
    }

    // .playerbots wipe confirm — deletes all AccountPrefix bot accounts/characters.
    static bool HandlePlayerbotWipeCommand(ChatHandler* handler, char const* args)
    {
        if (!sPlayerbotMgr->IsEnabled())
        {
            handler->SendSysMessage("Playerbots module is disabled (set Playerbots.Enable = 1).");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string arg = args ? args : "";
        while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t'))
            arg.erase(arg.begin());
        std::transform(arg.begin(), arg.end(), arg.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });

        if (arg != "confirm")
        {
            handler->PSendSysMessage(
                "This deletes ALL accounts whose username starts with '%s' (and their characters).",
                sPlayerbotMgr->GetAccountPrefix().c_str());
            handler->SendSysMessage("Usage: .playerbots wipe confirm");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string error;
        if (!sPlayerbotMgr->CanDeleteBotAccounts(&error))
        {
            handler->SendSysMessage(error.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->SendSysMessage("Wiping bot accounts; this may take a moment...");
        std::string report;
        sPlayerbotMgr->DeleteBotAccounts(&report);
        handler->PSendSysMessage("%s", report.c_str());
        return true;
    }

    // .playerbots self [on|off]
    // Attach cast-only AI to your logged-in character (you keep WASD movement).
    static bool HandlePlayerbotSelfCommand(ChatHandler* handler, char const* args)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
        {
            handler->SendSysMessage("This command must be used in-game.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!sPlayerbotMgr->IsEnabled())
        {
            handler->SendSysMessage("Playerbots module is disabled (set Playerbots.Enable = 1).");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string arg = args ? args : "";
        std::transform(arg.begin(), arg.end(), arg.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });

        std::string report;
        if (arg == "off" || arg == "detach" || arg == "disable")
        {
            if (!sPlayerbotMgr->DetachSelfBot(player))
            {
                handler->SendSysMessage("Self-bot AI is not attached.");
                handler->SetSentErrorMessage(true);
                return false;
            }
            handler->SendSysMessage("Self-bot AI detached. You control combat again.");
            return true;
        }

        if (arg == "on" || arg == "attach" || arg == "enable")
        {
            if (!sPlayerbotMgr->AttachSelfBot(player, &report))
            {
                handler->PSendSysMessage("%s", report.c_str());
                handler->SetSentErrorMessage(true);
                return false;
            }
            handler->SendSysMessage("Self-bot AI attached. You move; the AI casts in combat.");
            return true;
        }

        if (!sPlayerbotMgr->ToggleSelfBot(player, &report))
        {
            handler->PSendSysMessage("%s", report.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }
        handler->PSendSysMessage("%s", report.c_str());
        return true;
    }

    // Parses a role or specialization token for .playerbots init.
    // Roles: tank / healer / dps. Specs: elemental, enhancement, feral, moonkin, etc.
    static bool ParseRoleOrSpecToken(std::string token, int& roleOut, uint32& specOut)
    {
        std::transform(token.begin(), token.end(), token.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });

        roleOut = -1;
        specOut = 0;

        if (token == "tank")
        {
            roleOut = 0;
            return true;
        }
        if (token == "healer" || token == "heal" || token == "heals" || token == "resto" || token == "restoration")
        {
            // "resto" alone is ambiguous across classes - treat as healer role
            // so SpecIdForRole picks the class's resto tab. Explicit class specs
            // below still win when the token is unique (e.g. mistweaver).
            if (token == "resto" || token == "restoration")
            {
                // Prefer role mapping; per-class resto specs use the same word.
                roleOut = 1;
                return true;
            }
            roleOut = 1;
            return true;
        }
        if (token == "dps" || token == "damage" || token == "dd")
        {
            roleOut = 2;
            return true;
        }

        // Spec names (ChrSpecialization ids). Hybrids need these so DPS is not
        // stuck on the default tab (e.g. shaman ele vs enh, druid balance vs feral).
        if (token == "elemental" || token == "ele")
            specOut = SPEC_SHAMAN_ELEMENTAL;
        else if (token == "enhancement" || token == "enh" || token == "enhance")
            specOut = SPEC_SHAMAN_ENHANCEMENT;
        else if (token == "balance" || token == "moonkin" || token == "boomkin" || token == "boomie")
            specOut = SPEC_DRUID_BALANCE;
        else if (token == "feral")
            specOut = SPEC_DRUID_FERAL;
        else if (token == "guardian")
            specOut = SPEC_DRUID_GUARDIAN;
        else if (token == "retribution" || token == "ret")
            specOut = SPEC_PALADIN_RETRIBUTION;
        else if (token == "protection" || token == "prot")
        {
            // Ambiguous across warrior/pala/monk - leave as tank role.
            roleOut = 0;
            return true;
        }
        else if (token == "holy")
        {
            // Ambiguous (pala/priest); use healer role so SpecIdForRole is class-aware.
            roleOut = 1;
            return true;
        }
        else if (token == "shadow")
            specOut = SPEC_PRIEST_SHADOW;
        else if (token == "discipline" || token == "disc")
            specOut = SPEC_PRIEST_DISCIPLINE;
        else if (token == "windwalker" || token == "ww")
            specOut = SPEC_MONK_WINDWALKER;
        else if (token == "brewmaster" || token == "brm")
            specOut = SPEC_MONK_BREWMASTER;
        else if (token == "mistweaver" || token == "mw")
            specOut = SPEC_MONK_MISTWEAVER;
        else if (token == "beastmastery" || token == "beastmaster" || token == "bm")
            specOut = SPEC_HUNTER_BEAST_MASTERY;
        else if (token == "marksmanship" || token == "marks" || token == "mm")
            specOut = SPEC_HUNTER_MARKSMANSHIP;
        else if (token == "survival" || token == "surv")
            specOut = SPEC_HUNTER_SURVIVAL;
        else if (token == "affliction" || token == "aff")
            specOut = SPEC_WARLOCK_AFFLICTION;
        else if (token == "demonology" || token == "demo")
            specOut = SPEC_WARLOCK_DEMONOLOGY;
        else if (token == "destruction" || token == "destro")
            specOut = SPEC_WARLOCK_DESTRUCTION;
        else if (token == "arcane")
            specOut = SPEC_MAGE_ARCANE;
        else if (token == "fire")
            specOut = SPEC_MAGE_FIRE;
        else if (token == "frost")
            specOut = SPEC_MAGE_FROST;
        else if (token == "arms")
            specOut = SPEC_WARRIOR_ARMS;
        else if (token == "fury")
            specOut = SPEC_WARRIOR_FURY;
        else if (token == "assassination" || token == "mut")
            specOut = SPEC_ROGUE_ASSASSINATION;
        else if (token == "combat")
            specOut = SPEC_ROGUE_COMBAT;
        else if (token == "subtlety" || token == "sub")
            specOut = SPEC_ROGUE_SUBTLETY;
        else if (token == "blood")
            specOut = SPEC_DEATH_KNIGHT_BLOOD;
        else if (token == "unholy")
            specOut = SPEC_DEATH_KNIGHT_UNHOLY;
        else
            return false;

        return specOut != 0;
    }

    static bool SpecMatchesClass(uint32 specId, uint8 cls)
    {
        ChrSpecializationEntry const* entry = sChrSpecializationStore.LookupEntry(specId);
        return entry && entry->classId == cls;
    }

    // Parses a gear quality token for .playerbots init (caps ItemQuality).
    // Accepts names, colors, or quality=rare / =epic forms.
    static bool ParseQualityToken(std::string token, int& qualityOut)
    {
        std::transform(token.begin(), token.end(), token.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });

        // Strip optional "quality=" / "q=" / leading '=' (e.g. init=rare style).
        if (token.compare(0, 8, "quality=") == 0)
            token = token.substr(8);
        else if (token.compare(0, 2, "q=") == 0)
            token = token.substr(2);
        else if (!token.empty() && token[0] == '=')
            token = token.substr(1);

        if (token == "poor" || token == "grey" || token == "gray" || token == "trash")
            qualityOut = ITEM_QUALITY_POOR;
        else if (token == "common" || token == "white")
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

    static char const* QualityName(int quality)
    {
        switch (quality)
        {
            case ITEM_QUALITY_POOR: return "poor";
            case ITEM_QUALITY_NORMAL: return "common";
            case ITEM_QUALITY_UNCOMMON: return "uncommon";
            case ITEM_QUALITY_RARE: return "rare";
            case ITEM_QUALITY_EPIC: return "epic";
            case ITEM_QUALITY_LEGENDARY: return "legendary";
            default: return "custom";
        }
    }

    // .playerbots init [<charname>|all|self] [tank|healer|dps|<spec>] [rare|epic|...] [relevel] [tele]
    // Tokens may appear in any order. Quality caps gear rolls (default: conf MaxQuality).
    // Without relevel, level is unchanged. With relevel, rolls AutoCreate.MinLevel–MaxLevel.
    // With tele/teleport, scatter to a level/faction-safe anchor after gear (uses post-relevel level).
    static bool HandlePlayerbotInitCommand(ChatHandler* handler, char const* args)
    {
        std::vector<std::string> tokens;
        if (args)
        {
            std::istringstream stream(args);
            std::string token;
            while (stream >> token)
                tokens.push_back(token);
        }

        std::string target;
        int roleOverride = -1;
        uint32 specOverride = 0;
        bool haveChoice = false;
        int maxItemQuality = -1; // use Playerbots.Gear.MaxQuality
        bool haveQuality = false;
        bool relevel = false;
        bool teleport = false;

        for (std::string const& raw : tokens)
        {
            std::string lower = raw;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                [](unsigned char c) { return char(std::tolower(c)); });
            if (lower == "relevel" || lower == "re-level")
            {
                relevel = true;
                continue;
            }
            if (lower == "tele" || lower == "teleport")
            {
                teleport = true;
                continue;
            }

            int quality = 0;
            if (ParseQualityToken(raw, quality))
            {
                maxItemQuality = quality;
                haveQuality = true;
                continue;
            }

            int role = -1;
            uint32 spec = 0;
            if (ParseRoleOrSpecToken(raw, role, spec))
            {
                roleOverride = role;
                specOverride = spec;
                haveChoice = true;
                continue;
            }

            if (target.empty())
            {
                target = raw;
                continue;
            }

            handler->PSendSysMessage(
                "Unknown init token '%s'. Use self|all|<name>, a role/spec, "
                "a quality (poor…legendary), relevel, and/or tele.",
                raw.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* master = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        std::string choiceNote;
        if (haveChoice || haveQuality || relevel || teleport)
        {
            choiceNote = " (";
            bool firstNote = true;
            if (haveChoice)
            {
                choiceNote += specOverride ? "spec" : "role";
                firstNote = false;
            }
            if (haveQuality)
            {
                if (!firstNote)
                    choiceNote += ", ";
                choiceNote += "max quality ";
                choiceNote += QualityName(maxItemQuality);
                firstNote = false;
            }
            if (relevel)
            {
                if (!firstNote)
                    choiceNote += ", ";
                choiceNote += "relevel ";
                choiceNote += std::to_string(sPlayerbotMgr->GetAutoMinLevel());
                choiceNote += "-";
                choiceNote += std::to_string(sPlayerbotMgr->GetAutoMaxLevel());
                firstNote = false;
            }
            if (teleport)
            {
                if (!firstNote)
                    choiceNote += ", ";
                choiceNote += "tele";
            }
            choiceNote += ")";
        }

        if (target == "all")
        {
            uint64 notify = master ? master->GetGUID() : 0;
            uint32 count = sPlayerbotMgr->EnqueueInitializeAllBots(
                roleOverride, specOverride, maxItemQuality, relevel, teleport, notify);
            if (!count)
            {
                handler->SendSysMessage("No active bots to init.");
                return true;
            }
            handler->PSendSysMessage(
                "Queued background init for %u bot(s)%s (%u per tick). "
                "You will be notified when finished.",
                count, choiceNote.c_str(), sPlayerbotMgr->GetInitPerTick());
            return true;
        }

        if (target == "self" || (!target.empty() && master && target == master->GetName()))
        {
            if (!master)
            {
                handler->SendSysMessage("This command must be used in-game.");
                handler->SetSentErrorMessage(true);
                return false;
            }
            if (specOverride && !SpecMatchesClass(specOverride, master->getClass()))
            {
                handler->SendSysMessage("That specialization does not match your class.");
                handler->SetSentErrorMessage(true);
                return false;
            }
            sPlayerbotMgr->InitializeBot(master, roleOverride, specOverride, maxItemQuality,
                relevel, teleport, teleport);
            handler->PSendSysMessage("Initialized yourself%s.", choiceNote.c_str());
            return true;
        }

        if (!target.empty())
        {
            std::string name = target;
            if (!normalizePlayerName(name))
            {
                handler->SendSysMessage("Invalid character name.");
                handler->SetSentErrorMessage(true);
                return false;
            }

            uint64 guid = sObjectMgr->GetPlayerGUIDByName(name);
            Player* bot = guid ? ObjectAccessor::FindPlayer(guid) : nullptr;
            if (!bot || (!sPlayerbotMgr->IsBot(bot->GetGUID()) && !sPlayerbotMgr->IsSelfBot(bot->GetGUID())))
            {
                handler->PSendSysMessage("'%s' is not an active bot.", name.c_str());
                handler->SetSentErrorMessage(true);
                return false;
            }
            if (specOverride && !SpecMatchesClass(specOverride, bot->getClass()))
            {
                handler->PSendSysMessage("That specialization does not match %s's class.", name.c_str());
                handler->SetSentErrorMessage(true);
                return false;
            }

            sPlayerbotMgr->InitializeBot(bot, roleOverride, specOverride, maxItemQuality,
                relevel, teleport, teleport);
            handler->PSendSysMessage("Initialized bot '%s'%s.", name.c_str(), choiceNote.c_str());
            return true;
        }

        if (!master)
        {
            handler->SendSysMessage(
                "Usage: .playerbots init [<charname>|all|self] [tank|healer|dps|<spec>] "
                "[rare|epic|...] [relevel] [tele].");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (specOverride && !SpecMatchesClass(specOverride, master->getClass()))
        {
            handler->SendSysMessage("That specialization does not match your class.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        sPlayerbotMgr->InitializeBot(master, roleOverride, specOverride, maxItemQuality,
            relevel, teleport, teleport);
        uint32 queued = 0;

        if (Group* group = master->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (member && member != master && sPlayerbotMgr->IsBot(member->GetGUID()))
                {
                    if (specOverride && !SpecMatchesClass(specOverride, member->getClass()))
                        continue;
                    if (sPlayerbotMgr->EnqueueInitializeBot(member->GetGUID(), roleOverride,
                            specOverride, maxItemQuality, relevel, teleport, master->GetGUID()))
                        ++queued;
                }
            }
        }

        if (queued)
            handler->PSendSysMessage(
                "Initialized yourself%s; queued %u grouped bot(s) for background init.",
                choiceNote.c_str(), queued);
        else
            handler->PSendSysMessage("Initialized yourself%s.", choiceNote.c_str());
        return true;
    }

    static bool HandlePlayerbotListCommand(ChatHandler* handler, char const* /*args*/)
    {
        std::vector<uint64> guids;
        sPlayerbotMgr->GetBotGuids(guids);

        handler->PSendSysMessage("Active bots: %u", uint32(guids.size()));
        for (uint64 guid : guids)
        {
            std::string name;
            if (!sObjectMgr->GetPlayerNameByGUID(guid, name))
                name = "<unknown>";
            handler->PSendSysMessage("  %s (GUID %u)", name.c_str(), GUID_LOPART(guid));
        }
        return true;
    }
};

void AddSC_playerbot_scripts()
{
    new playerbot_world_script();
    new playerbot_player_script();
    new playerbot_commandscript();
}
