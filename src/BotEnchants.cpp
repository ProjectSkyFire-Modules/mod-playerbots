/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* Enchants and gems for a bot's equipped gear.
*
* Candidates come from the game data, built once: gems are the gem items of
* the item template store, enchants are the enchanting profession's item
* enchant spells plus the item enhancement consumables (scrolls, armor kits,
* leg armor, shoulder inscriptions, belt buckles). Each is scored with the
* bot's spec weights (BotStatWeights): stats of the enchant, and the auras of
* its equip and proc spells (procs count kProcUptime of their value).
*/

#include "BotEnchants.h"
#include "BotStatWeights.h"

#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "ItemEnchantmentMgr.h"
#include "ItemPrototype.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    // Defined with the gear valuation below.
    void AddEnchantStats(BotStatVector& stats, SpellItemEnchantmentEntry const* enchant, float scale);
    // mustReachBoost: points below a must-reach cap count that many times (maintenance
    // kMustReachBoost, loot rolls 1.15, 1 = no boost).
    // exclude: stats taken out of the bot's totals first (the enchant being replaced).
    float ValueStats(Player* bot, BotStatWeights const& weights, BotStatVector const& stats, uint8 slot,
        float mustReachBoost = 1.0f, BotStatVector const* exclude = nullptr);
    constexpr float kMustReachBoost = 10.0f;

    // Share of a proc's stats counted, a rough average uptime.
    constexpr float kProcUptime = 0.35f;
    constexpr uint32 kNormalGemColors = SOCKET_COLOR_RED | SOCKET_COLOR_YELLOW | SOCKET_COLOR_BLUE;

    struct GemCandidate
    {
        ItemTemplate const* proto;
        SpellItemEnchantmentEntry const* enchant;
        uint32 color;
    };

    struct EnchantCandidate
    {
        SpellInfo const* spell;
        SpellItemEnchantmentEntry const* enchant;
        uint32 level;      // bot level needed
        uint32 skill;      // profession the bot itself needs (item RequiredSkill), 0 = none
        uint32 skillRank;
        bool prismatic;    // adds a socket (belt buckle) instead of a permanent enchant
        ItemTemplate const* source; // scroll / kit / inscription item, nullptr for recipes
        float power = 0.0f;         // stat value in primary-stat points (PowerWeights)
    };

    // Share of the gear's stat budget an enchant may be worth (Playerbots.Maintenance.EnchantBudgetShare).
    float g_enchantBudgetShare = 1.0f;

    bool IsJunkName(char const* raw)
    {
        if (!raw)
            return false;
        std::string name(raw);
        std::transform(name.begin(), name.end(), name.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });
        // "test" as a word only: "Greatest" is a real enchant name.
        for (size_t at = name.find("test"); at != std::string::npos; at = name.find("test", at + 1))
            if (at == 0 || !std::isalpha(static_cast<unsigned char>(name[at - 1])))
                return true;
        return name.rfind("qa", 0) == 0 || name.find("deprecated") != std::string::npos
            || name.find("unused") != std::string::npos || name.rfind("monster -", 0) == 0;
    }

    // Enchanting recipe rank to the level of the content it belongs to.
    uint32 SkillRankToLevel(uint32 rank)
    {
        if (rank <= 300)
            return std::max<uint32>(1, rank / 5);
        if (rank <= 375)
            return 60 + (rank - 300) * 10 / 75;
        if (rank <= 450)
            return 70 + (rank - 375) * 10 / 75;
        if (rank <= 525)
            return 80 + (rank - 450) * 5 / 75;
        return std::min<uint32>(90, 85 + (rank - 525) * 5 / 75);
    }

    std::vector<GemCandidate> const& GemCandidates()
    {
        static std::vector<GemCandidate> gems;
        static bool built = false;
        if (built)
            return gems;
        built = true;

        for (auto const& pair : *sObjectMgr->GetItemTemplateStore())
        {
            ItemTemplate const* proto = &pair.second;
            if (proto->Class != ITEM_CLASS_GEM || !proto->GemProperties || IsJunkName(proto->Name1.c_str()))
                continue;
            GemPropertiesEntry const* props = sGemPropertiesStore.LookupEntry(proto->GemProperties);
            if (!props || !props->spellitemenchantement || !props->color)
                continue;
            // Sha-touched and cogwheel gems only fit their own sockets, which bots skip.
            if (props->color & (SOCKET_COLOR_HYDRAULIC | SOCKET_COLOR_COGWHEEL))
                continue;
            bool const meta = props->color == SOCKET_COLOR_META;
            // Unique gems other than metas are profession or special gems.
            if (!meta && ((proto->Flags & ITEM_PROTO_FLAG_UNIQUE_EQUIPPED) || proto->ItemLimitCategory))
                continue;
            SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(props->spellitemenchantement);
            if (!enchant)
                continue;
            gems.push_back({ proto, enchant, props->color });
        }
        SF_LOG_INFO("modules", "[mod-playerbots] Enchants: %u gem candidate(s).", uint32(gems.size()));
        return gems;
    }

    bool FindEnchantEffect(SpellInfo const* spell, SpellItemEnchantmentEntry const*& enchant, bool& prismatic)
    {
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& effect = spell->Effects[i];
            if (effect.Effect != SPELL_EFFECT_ENCHANT_ITEM && effect.Effect != SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC)
                continue;
            if (effect.MiscValue <= 0)
                continue;
            enchant = sSpellItemEnchantmentStore.LookupEntry(uint32(effect.MiscValue));
            if (!enchant)
                continue;
            prismatic = effect.Effect == SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC;
            if (prismatic)
            {
                bool socket = false;
                for (uint8 j = 0; j < MAX_ITEM_ENCHANTMENT_EFFECTS; ++j)
                    socket = socket || enchant->type[j] == ITEM_ENCHANTMENT_TYPE_PRISMATIC_SOCKET;
                if (!socket)
                    continue;
            }
            return true;
        }
        return false;
    }

    bool TargetsGear(SpellInfo const* spell)
    {
        return spell->EquippedItemClass == ITEM_CLASS_ARMOR || spell->EquippedItemClass == ITEM_CLASS_WEAPON;
    }

    float EnchantPower(SpellItemEnchantmentEntry const* enchant);   // below, after ScoreEnchant

    std::vector<EnchantCandidate> const& EnchantCandidates()
    {
        static std::vector<EnchantCandidate> enchants;
        static bool built = false;
        if (built)
            return enchants;
        built = true;

        // Scrolls, armor kits, leg armor, inscriptions, buckles.
        for (auto const& pair : *sObjectMgr->GetItemTemplateStore())
        {
            ItemTemplate const* proto = &pair.second;
            if (proto->Class != ITEM_CLASS_CONSUMABLE || proto->SubClass != ITEM_SUBCLASS_ITEM_ENHANCEMENT)
                continue;
            if (IsJunkName(proto->Name1.c_str()))
                continue;
            for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            {
                if (proto->Spells[i].SpellId <= 0)
                    continue;
                SpellInfo const* spell = sSpellMgr->GetSpellInfo(uint32(proto->Spells[i].SpellId));
                SpellItemEnchantmentEntry const* enchant = nullptr;
                bool prismatic = false;
                if (!spell || !TargetsGear(spell) || !FindEnchantEffect(spell, enchant, prismatic))
                    continue;
                // Without a required or item level there is nothing to say who it is for.
                if (!proto->RequiredLevel && proto->ItemLevel <= 1)
                    continue;
                uint32 const level = proto->RequiredLevel ? proto->RequiredLevel
                    : std::clamp<uint32>(proto->ItemLevel, 1, 90);
                enchants.push_back({ spell, enchant, level, proto->RequiredSkill, proto->RequiredSkillRank, prismatic, proto });
            }
        }

        // Enchanting recipes: the bot does not need the profession, someone enchanted it.
        for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
        {
            SpellInfo const* spell = sSpellMgr->GetSpellInfo(id);
            if (!spell || !TargetsGear(spell) || IsJunkName(spell->SpellName))
                continue;
            uint32 rank = 0;
            bool enchanting = false;
            SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(id);
            for (auto it = bounds.first; it != bounds.second; ++it)
            {
                if (it->second->skillId != SKILL_ENCHANTING)
                    continue;
                enchanting = true;
                rank = std::max(rank, it->second->req_skill_value ? it->second->req_skill_value : it->second->min_value);
            }
            // A recipe without a skill rank has no level: often an NPC or unused copy.
            if (!enchanting || !rank)
                continue;
            SpellItemEnchantmentEntry const* enchant = nullptr;
            bool prismatic = false;
            if (!FindEnchantEffect(spell, enchant, prismatic))
                continue;
            enchants.push_back({ spell, enchant, SkillRankToLevel(rank), 0, 0, prismatic, nullptr });
        }
        for (EnchantCandidate& candidate : enchants)
            candidate.power = EnchantPower(candidate.enchant);
        SF_LOG_INFO("modules", "[mod-playerbots] Enchants: %u enchant candidate(s).", uint32(enchants.size()));
        return enchants;
    }

    BotStat StatFromCombatRating(uint32 rating)
    {
        switch (CombatRating(rating))
        {
            case CombatRating::CR_DODGE:       return BOT_STAT_DODGE;
            case CombatRating::CR_PARRY:       return BOT_STAT_PARRY;
            case CombatRating::CR_HIT_MELEE:
            case CombatRating::CR_HIT_RANGED:
            case CombatRating::CR_HIT_SPELL:   return BOT_STAT_HIT;
            case CombatRating::CR_CRIT_MELEE:
            case CombatRating::CR_CRIT_RANGED:
            case CombatRating::CR_CRIT_SPELL:  return BOT_STAT_CRIT;
            case CombatRating::CR_HASTE_MELEE:
            case CombatRating::CR_HASTE_RANGED:
            case CombatRating::CR_HASTE_SPELL: return BOT_STAT_HASTE;
            case CombatRating::CR_EXPERTISE:   return BOT_STAT_EXPERTISE;
            case CombatRating::CR_MASTERY:     return BOT_STAT_MASTERY;
            default:             return BOT_STAT_MAX;
        }
    }

    BotStat StatFromUnitStat(int32 stat)
    {
        switch (stat)
        {
            case STAT_STRENGTH:  return BOT_STAT_STRENGTH;
            case STAT_AGILITY:   return BOT_STAT_AGILITY;
            case STAT_STAMINA:   return BOT_STAT_STAMINA;
            case STAT_INTELLECT: return BOT_STAT_INTELLECT;
            case STAT_SPIRIT:    return BOT_STAT_SPIRIT;
            default:             return BOT_STAT_MAX;
        }
    }

    // Stats an aura spell gives, weighted; proc triggers count kProcUptime.
    float ScoreSpell(uint32 spellId, Player* bot, BotStatWeights const& weights, float factor, uint32 depth)
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(spellId);
        if (!spell || depth > 2)
            return 0.0f;

        float direct = 0.0f;
        float procs = 0.0f;
        bool attackPower = false;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            SpellEffectInfo const& effect = spell->Effects[i];
            if (!effect.IsAura())
                continue;
            float const amount = float(effect.CalcValue(bot));
            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_MOD_STAT:
                    if (effect.MiscValue < 0)
                    {
                        for (int32 stat = STAT_STRENGTH; stat <= STAT_SPIRIT; ++stat)
                            direct += weights.Get(StatFromUnitStat(stat)) * amount;
                    }
                    else if (BotStat stat = StatFromUnitStat(effect.MiscValue); stat != BOT_STAT_MAX)
                        direct += weights.Get(stat) * amount;
                    break;
                case SPELL_AURA_MOD_RATING:
                {
                    bool seen[BOT_STAT_MAX] = {};
                    for (uint32 rating = 0; rating < MAX_COMBAT_RATING; ++rating)
                    {
                        if (!(uint32(effect.MiscValue) & (1u << rating)))
                            continue;
                        BotStat stat = StatFromCombatRating(rating);
                        if (stat == BOT_STAT_MAX || seen[stat])
                            continue;
                        seen[stat] = true;
                        direct += weights.Get(stat) * amount;
                    }
                    break;
                }
                case SPELL_AURA_MOD_DAMAGE_DONE:
                    if (effect.MiscValue & SPELL_SCHOOL_MASK_MAGIC)
                        direct += weights.Get(BOT_STAT_SPELL_POWER) * amount;
                    break;
                case SPELL_AURA_MOD_ATTACK_POWER:
                case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
                    if (!attackPower)
                        direct += weights.Get(BOT_STAT_ATTACK_POWER) * amount;
                    attackPower = true;
                    break;
                case SPELL_AURA_PROC_TRIGGER_SPELL:
                    if (effect.TriggerSpell && effect.TriggerSpell != spellId)
                        procs += ScoreSpell(effect.TriggerSpell, bot, weights, factor * kProcUptime, depth + 1);
                    break;
                default:
                    break;
            }
        }
        return direct * factor + procs;
    }

    float ScoreEnchant(SpellItemEnchantmentEntry const* enchant, Player* bot, BotStatWeights const& weights)
    {
        if (!enchant)
            return 0.0f;
        float score = 0.0f;
        for (uint8 i = 0; i < MAX_ITEM_ENCHANTMENT_EFFECTS; ++i)
        {
            switch (enchant->type[i])
            {
                case ITEM_ENCHANTMENT_TYPE_STAT:
                    score += weights.ForItemMod(enchant->spellid[i]) * float(enchant->amount[i]);
                    break;
                case ITEM_ENCHANTMENT_TYPE_EQUIP_SPELL:
                    score += ScoreSpell(enchant->spellid[i], bot, weights, 1.0f, 0);
                    break;
                case ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL:
                    score += ScoreSpell(enchant->spellid[i], bot, weights, kProcUptime, 0);
                    break;
                default:
                    break;
            }
        }
        return score;
    }

    // Spec-neutral yardstick: everything in primary-stat points (MoP gems: 160 primary,
    // 240 stamina, 320 rating), attack power at half a point, procs at their uptime.
    BotStatWeights const& PowerWeights()
    {
        static BotStatWeights weights = []
        {
            BotStatWeights w;
            for (float& v : w.perPoint)
                v = 0.5f;
            w.perPoint[BOT_STAT_STRENGTH] = w.perPoint[BOT_STAT_AGILITY] = w.perPoint[BOT_STAT_INTELLECT] = 1.0f;
            w.perPoint[BOT_STAT_SPELL_POWER] = 1.0f;
            w.perPoint[BOT_STAT_STAMINA] = 0.67f;
            return w;
        }();
        return weights;
    }

    float EnchantPower(SpellItemEnchantmentEntry const* enchant)
    {
        return ScoreEnchant(enchant, nullptr, PowerWeights());
    }

    // An enchant's worth in maintenance: its equip effects, and its stats valued against the
    // spec's caps with must-reach caps filled first.
    float MaintenanceEnchantScore(SpellItemEnchantmentEntry const* enchant, Player* bot,
        BotStatWeights const& weights, BotStatVector const& exclude)
    {
        float const total = ScoreEnchant(enchant, bot, weights);
        if (weights.caps.empty() || !enchant)
            return total;
        BotStatVector stats{};
        AddEnchantStats(stats, enchant, 1.0f);
        float plain = 0.0f;
        for (uint32 s = 0; s < BOT_STAT_MAX; ++s)
            plain += stats[s] * weights.Get(BotStat(s));
        return total - plain + ValueStats(bot, weights, stats, NULL_SLOT, kMustReachBoost, &exclude);
    }

    // Stat budget of an epic chest at the gear's item level (RandPropPoints): the
    // yardstick an enchant is measured against. Heirlooms count at the bot's level, items
    // raised by Destiny Weaver at their raised item level (Item::GetStatTemplate).
    float GearBudget(Player* bot, Item* item)
    {
        ItemTemplate const* proto = item->GetStatTemplate();
        uint32 itemLevel = proto->ItemLevel;
        if (proto->ScalingStatDistribution)
            itemLevel = std::max<uint32>(itemLevel, bot->getLevel());
        RandomPropertiesPointsEntry const* points = sRandomPropertiesPointsStore.LookupEntry(itemLevel);
        return points ? float(points->EpicPropertiesPoints[0]) : 0.0f;
    }

    // Items sold for reputation (shoulder inscriptions, some gems) need it.
    bool HasReputation(Player* bot, ItemTemplate const* proto)
    {
        return !proto || !proto->RequiredReputationFaction
            || bot->GetReputationRank(proto->RequiredReputationFaction) >= ReputationRank(proto->RequiredReputationRank);
    }

    bool HasSkill(Player* bot, uint32 skill, uint32 rank)
    {
        return !skill || (bot->HasSkill(skill) && bot->GetSkillValue(skill) >= rank);
    }

    // Level, profession and target checks shared by every candidate enchant.
    bool EnchantUsable(EnchantCandidate const& c, Player* bot, Item* item)
    {
        uint32 const level = bot->getLevel();
        SpellItemEnchantmentEntry const* enchant = c.enchant;
        if (c.level > level || !HasSkill(bot, c.skill, c.skillRank) || !HasReputation(bot, c.source))
            return false;
        if (enchant->requiredMinLevel > level || (enchant->requiredMaxLevel && enchant->requiredMaxLevel < level))
            return false;
        // Ring enchants and other profession perks.
        if (!HasSkill(bot, enchant->requiredSkill, enchant->requiredSkillValue))
            return false;
        if (item->GetStatTemplate()->ItemLevel < c.spell->BaseLevel)
            return false;
        // An enchant worth more than the gear's own stat budget belongs to far better
        // gear, whatever level its source claims (test copies, recipes without a rank).
        if (c.power > 0.0f)
            if (float const budget = GearBudget(bot, item); budget > 0.0f && c.power > budget * g_enchantBudgetShare)
                return false;
        return item->IsFitToSpellRequirements(c.spell);
    }

    uint32 GemColorOfEnchant(uint32 enchantId)
    {
        SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
        if (!enchant || !enchant->GemID)
            return 0;
        ItemTemplate const* gem = sObjectMgr->GetItemTemplate(enchant->GemID);
        if (!gem)
            return 0;
        GemPropertiesEntry const* props = sGemPropertiesStore.LookupEntry(gem->GemProperties);
        return props ? props->color : 0;
    }

    struct Socket
    {
        uint8 index;
        uint32 color;   // socket color; kNormalGemColors for a buckle's prismatic socket
        bool prismatic;
    };

    struct GemPick
    {
        GemCandidate const* gem = nullptr;
        float score = 0.0f;
    };

    GemPick BestGem(Socket const& socket, bool matchColor, Player* bot, Item* item, std::vector<float> const& gemScores)
    {
        std::vector<GemCandidate> const& gems = GemCandidates();
        uint32 const level = bot->getLevel();
        uint32 const itemLevel = item->GetStatTemplate()->ItemLevel;
        bool const metaSocket = socket.color == SOCKET_COLOR_META;
        GemPick best;
        for (size_t i = 0; i < gems.size(); ++i)
        {
            GemCandidate const& gem = gems[i];
            bool const meta = gem.color == SOCKET_COLOR_META;
            if (meta != metaSocket)
                continue;
            if (!meta && matchColor && !socket.prismatic && !(gem.color & socket.color))
                continue;
            if (gemScores[i] <= best.score)
                continue;
            ItemTemplate const* proto = gem.proto;
            if (proto->RequiredLevel > level || proto->ItemLevel > itemLevel)
                continue;
            if (!HasSkill(bot, proto->RequiredSkill, proto->RequiredSkillRank) || !HasReputation(bot, proto))
                continue;
            if (meta && bot->CanEquipUniqueItem(proto, item->GetSlot(), 1) != EQUIP_ERR_OK)
                continue;
            best.gem = &gem;
            best.score = gemScores[i];
        }
        return best;
    }

    // Same order as WorldSession::HandleSocketOpcode.
    void SocketGems(Player* bot, Item* item, std::vector<std::pair<uint8, uint32>> const& picks)
    {
        uint8 const slot = item->GetSlot();
        bool const bonusBefore = item->GemsFitSockets();
        bot->ToggleMetaGemsActive(slot, false);

        for (uint32 enchantSlot = SOCK_ENCHANTMENT_SLOT; enchantSlot < SOCK_ENCHANTMENT_SLOT + MAX_GEM_SOCKETS; ++enchantSlot)
            bot->ApplyEnchantment(item, EnchantmentSlot(enchantSlot), false);
        for (auto const& pick : picks)
            item->SetEnchantment(EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + pick.first), pick.second, 0, 0, bot->GetGUID());
        for (uint32 enchantSlot = SOCK_ENCHANTMENT_SLOT; enchantSlot < SOCK_ENCHANTMENT_SLOT + MAX_GEM_SOCKETS; ++enchantSlot)
            bot->ApplyEnchantment(item, EnchantmentSlot(enchantSlot), true);

        bool const bonusAfter = item->GemsFitSockets();
        if (bonusBefore != bonusAfter)
        {
            bot->ApplyEnchantment(item, BONUS_ENCHANTMENT_SLOT, false);
            item->SetEnchantment(BONUS_ENCHANTMENT_SLOT, bonusAfter ? item->GetTemplate()->socketBonus : 0, 0, 0, bot->GetGUID());
            bot->ApplyEnchantment(item, BONUS_ENCHANTMENT_SLOT, true);
        }

        bot->ToggleMetaGemsActive(slot, true);
        item->ClearSoulboundTradeable(bot);
    }

    uint32 GemItem(Player* bot, Item* item, BotStatWeights const& weights, std::vector<float> const& gemScores)
    {
        ItemTemplate const* proto = item->GetTemplate();
        std::vector<Socket> empty;
        bool bonusReachable = proto->socketBonus != 0;
        bool prismaticTaken = !item->GetEnchantmentId(PRISMATIC_ENCHANTMENT_SLOT);
        for (uint8 i = 0; i < MAX_GEM_SOCKETS; ++i)
        {
            uint32 color = proto->Socket[i].Color;
            bool prismatic = false;
            if (!color)
            {
                // A buckle's socket is the first one the item does not have.
                if (prismaticTaken)
                    break;
                prismaticTaken = true;
                prismatic = true;
                color = kNormalGemColors;
            }
            if (color & (SOCKET_COLOR_HYDRAULIC | SOCKET_COLOR_COGWHEEL))
                continue;

            if (uint32 current = item->GetEnchantmentId(EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + i)))
            {
                if (!prismatic && !(GemColorOfEnchant(current) & color))
                    bonusReachable = false;
                continue;
            }
            empty.push_back({ i, color, prismatic });
        }
        if (empty.empty())
            return 0;

        // Best gems whatever their color, or matching colors plus the socket bonus.
        std::vector<std::pair<uint8, uint32>> anyColor, matched;
        float anyScore = 0.0f, matchedScore = 0.0f;
        bool matchedComplete = bonusReachable;
        for (Socket const& socket : empty)
        {
            GemPick pick = BestGem(socket, false, bot, item, gemScores);
            if (pick.gem)
            {
                anyColor.emplace_back(socket.index, pick.gem->enchant->ID);
                anyScore += pick.score;
            }
            if (!bonusReachable)
                continue;
            GemPick match = BestGem(socket, true, bot, item, gemScores);
            if (match.gem)
            {
                matched.emplace_back(socket.index, match.gem->enchant->ID);
                matchedScore += match.score;
            }
            else if (!socket.prismatic)
                matchedComplete = false;
        }
        if (matchedComplete)
            matchedScore += ScoreEnchant(sSpellItemEnchantmentStore.LookupEntry(proto->socketBonus), bot, weights);

        std::vector<std::pair<uint8, uint32>> const& picks =
            (bonusReachable && matchedScore >= anyScore) ? matched : anyColor;
        if (picks.empty())
            return 0;
        SocketGems(bot, item, picks);
        return uint32(picks.size());
    }

    void EnchantItem(Player* bot, Item* item, EnchantmentSlot slot, uint32 enchantId)
    {
        bot->ApplyEnchantment(item, slot, false);
        item->SetEnchantment(slot, enchantId, 0, 0, bot->GetGUID());
        bot->ApplyEnchantment(item, slot, true);
        item->ClearSoulboundTradeable(bot);
    }

    bool HasFreeSocketPosition(ItemTemplate const* proto)
    {
        for (uint8 i = 0; i < MAX_GEM_SOCKETS; ++i)
            if (!proto->Socket[i].Color)
                return true;
        return false;
    }

    uint32 EnchantGear(Player* bot, Item* item, BotStatWeights const& weights, bool replace)
    {
        uint32 applied = 0;
        EnchantCandidate const* best = nullptr;
        float bestScore = 0.0f;
        EnchantCandidate const* buckle = nullptr;
        bool const wantsBuckle = !item->GetEnchantmentId(PRISMATIC_ENCHANTMENT_SLOT)
            && HasFreeSocketPosition(item->GetTemplate());
        // The enchant on the item now leaves when another goes on: the bot's caps are
        // measured without it, for the candidates and for itself.
        BotStatVector currentStats{};
        AddEnchantStats(currentStats, sSpellItemEnchantmentStore.LookupEntry(item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT)), 1.0f);
        for (EnchantCandidate const& candidate : EnchantCandidates())
        {
            if (candidate.prismatic)
            {
                if (wantsBuckle && !buckle && EnchantUsable(candidate, bot, item))
                    buckle = &candidate;
                continue;
            }
            float const score = MaintenanceEnchantScore(candidate.enchant, bot, weights, currentStats);
            if (score <= bestScore || !EnchantUsable(candidate, bot, item))
                continue;
            best = &candidate;
            bestScore = score;
        }

        // An enchant over the cap (put on before it existed) comes off a bot that is not an
        // alt, replaced below when something fits, else left empty.
        uint32 const existing = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
        if (existing && replace)
        {
            float const power = EnchantPower(sSpellItemEnchantmentStore.LookupEntry(existing));
            float const budget = GearBudget(bot, item);
            if (budget > 0.0f && power > budget * g_enchantBudgetShare)
            {
                SF_LOG_INFO("modules", "[mod-playerbots] Removed enchant %u from %s's %s (item level %u): "
                    "worth %.0f of a %.0f budget.", existing, bot->GetName().c_str(),
                    item->GetTemplate()->Name1.c_str(), item->GetTemplate()->ItemLevel, power, budget);
                EnchantItem(bot, item, PERM_ENCHANTMENT_SLOT, 0);
            }
        }

        if (best)
        {
            uint32 const current = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
            bool better = !current;
            if (current && replace && current != best->enchant->ID)
                better = bestScore > MaintenanceEnchantScore(sSpellItemEnchantmentStore.LookupEntry(current), bot,
                    weights, currentStats) + 0.5f;
            if (better)
            {
                EnchantItem(bot, item, PERM_ENCHANTMENT_SLOT, best->enchant->ID);
                ++applied;
                SF_LOG_INFO("modules", "[mod-playerbots] Enchanted %s's %s (item level %u): enchant %u '%s' "
                    "from %s %u (level %u, worth %.0f of a %.0f budget).",
                    bot->GetName().c_str(), item->GetTemplate()->Name1.c_str(), item->GetTemplate()->ItemLevel,
                    best->enchant->ID, best->enchant->description ? best->enchant->description : "",
                    best->source ? "item" : "spell", best->source ? best->source->ItemId : best->spell->Id,
                    best->level, best->power, GearBudget(bot, item));
            }
        }
        if (buckle)
        {
            EnchantItem(bot, item, PRISMATIC_ENCHANTMENT_SLOT, buckle->enchant->ID);
            ++applied;
        }
        return applied;
    }
}

void BotEnchants::SetEnchantBudgetShare(float share)
{
    g_enchantBudgetShare = std::max(0.05f, share);
}

BotEnchants::Result BotEnchants::Apply(Player* bot, uint32 specId, bool enchants, bool gems, bool replaceEnchants)
{
    Result result;
    if (!bot || (!enchants && !gems))
        return result;

    BotStatWeights const& weights = BotStatWeightTable::For(specId, bot->getClass());

    // A gem's score is its weighted stats plus its equip effects (meta gems). With caps,
    // the stats are valued against the bot's ratings as they stand before each item is
    // socketed, so hit or expertise gems stop once the cap is reached.
    std::vector<GemCandidate> const& candidates = GemCandidates();
    std::vector<float> gemScores;
    std::vector<float> gemEffects;
    std::vector<BotStatVector> gemStats;
    if (gems)
    {
        gemScores.reserve(candidates.size());
        for (GemCandidate const& gem : candidates)
        {
            BotStatVector stats{};
            AddEnchantStats(stats, gem.enchant, 1.0f);
            float const total = ScoreEnchant(gem.enchant, bot, weights);
            float plain = 0.0f;
            for (uint32 s = 0; s < BOT_STAT_MAX; ++s)
                plain += stats[s] * weights.Get(BotStat(s));
            gemScores.push_back(total);
            gemEffects.push_back(total - plain);
            gemStats.push_back(stats);
        }
    }

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !item->GetTemplate())
            continue;
        if (enchants)
            result.enchants += EnchantGear(bot, item, weights, replaceEnchants);
        if (gems)
        {
            if (!weights.caps.empty())
                for (size_t i = 0; i < candidates.size(); ++i)
                    gemScores[i] = gemEffects[i] + ValueStats(bot, weights, gemStats[i], NULL_SLOT, kMustReachBoost);
            result.gems += GemItem(bot, item, weights, gemScores);
        }
    }
    return result;
}

namespace
{
    // item_enchantment_template: the random properties / suffixes an item can roll.
    struct RandomEnchantChance
    {
        uint32 enchant;
        float chance;
    };

    std::unordered_map<uint32, std::vector<RandomEnchantChance>> const& RandomEnchantTable()
    {
        static std::unordered_map<uint32, std::vector<RandomEnchantChance>> table;
        static bool loaded = false;
        if (loaded)
            return table;
        loaded = true;
        if (QueryResult result = WorldDatabase.Query("SELECT entry, ench, chance FROM item_enchantment_template"))
        {
            do
            {
                Field* fields = result->Fetch();
                float const chance = fields[2].GetFloat();
                if (chance > 0.000001f && chance <= 100.0f)
                    table[fields[0].GetUInt32()].push_back({ fields[1].GetUInt32(), chance });
            } while (result->NextRow());
        }
        return table;
    }

    void AddItemMod(BotStatVector& stats, uint32 itemMod, float amount)
    {
        BotStat const stat = StatForItemMod(itemMod);
        if (stat != BOT_STAT_MAX)
            stats[stat] += amount;
    }

    // The stat effects of an enchant (gem, socket bonus, enchant, random property).
    void AddEnchantStats(BotStatVector& stats, SpellItemEnchantmentEntry const* enchant, float scale)
    {
        if (!enchant)
            return;
        for (uint8 i = 0; i < MAX_ITEM_ENCHANTMENT_EFFECTS; ++i)
            if (enchant->type[i] == ITEM_ENCHANTMENT_TYPE_STAT)
                AddItemMod(stats, enchant->spellid[i], float(enchant->amount[i]) * scale);
    }

    // A random suffix's stats have no amount: allocation * the item's suffix factor.
    void AddSuffixStats(BotStatVector& stats, ItemRandomSuffixEntry const* suffix, uint32 suffixFactor, float scale)
    {
        if (!suffix)
            return;
        for (uint8 k = 0; k < 5; ++k)
        {
            SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(suffix->enchant_id[k]);
            if (!enchant)
                continue;
            for (uint8 i = 0; i < MAX_ITEM_ENCHANTMENT_EFFECTS; ++i)
            {
                if (enchant->type[i] != ITEM_ENCHANTMENT_TYPE_STAT)
                    continue;
                uint32 const amount = enchant->amount[i] ? enchant->amount[i]
                    : suffix->prefix[k] * suffixFactor / 10000;
                AddItemMod(stats, enchant->spellid[i], float(amount) * scale);
            }
        }
    }

    void AddRandomPropertyStats(BotStatVector& stats, uint32 propertyId, float scale)
    {
        if (ItemRandomPropertiesEntry const* property = sItemRandomPropertiesStore.LookupEntry(propertyId))
            for (uint8 i = 0; i < MAX_ITEM_ENCHANTMENT_EFFECTS; ++i)
                AddEnchantStats(stats, sSpellItemEnchantmentStore.LookupEntry(property->enchant_id[i]), scale);
    }

    // Worn item: the stats it rolled. Candidate: the average of what it can roll.
    void AddRandomStats(BotStatVector& stats, ItemTemplate const* proto, Item* item)
    {
        if (item)
        {
            int32 const id = item->GetItemRandomPropertyId();
            if (id > 0)
                AddRandomPropertyStats(stats, uint32(id), 1.0f);
            else if (id < 0)
                AddSuffixStats(stats, sItemRandomSuffixStore.LookupEntry(uint32(-id)), item->GetItemSuffixFactor(), 1.0f);
            return;
        }

        uint32 const group = proto->RandomProperty ? uint32(proto->RandomProperty) : uint32(proto->RandomSuffix);
        if (!group)
            return;
        auto const& table = RandomEnchantTable();
        auto it = table.find(group);
        if (it == table.end())
            return;
        float chances = 0.0f;
        for (RandomEnchantChance const& roll : it->second)
            chances += roll.chance;
        if (chances <= 0.0f)
            return;
        uint32 const suffixFactor = proto->RandomSuffix ? GenerateEnchSuffixFactor(proto->ItemId) : 0;
        for (RandomEnchantChance const& roll : it->second)
        {
            float const share = roll.chance / chances;
            if (proto->RandomProperty)
                AddRandomPropertyStats(stats, roll.enchant, share);
            else
                AddSuffixStats(stats, sItemRandomSuffixStore.LookupEntry(roll.enchant), suffixFactor, share);
        }
    }

    // Same stat source as Player::_ApplyItemBonuses (heirlooms scale with level).
    void AddTemplateStats(BotStatVector& stats, Player* bot, ItemTemplate const* proto)
    {
        ScalingStatDistributionEntry const* ssd = proto->ScalingStatDistribution
            ? sScalingStatDistributionStore.LookupEntry(proto->ScalingStatDistribution) : nullptr;
        ScalingStatValuesEntry const* ssv = nullptr;
        if (ssd)
            ssv = sScalingStatValuesStore.LookupEntry(std::min<uint32>(bot->getLevel(), ssd->MaxLevel));
        for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
        {
            if (ssd && ssv)
            {
                if (ssd->StatMod[i] < 0)
                    continue;
                int32 const value = int32(ssv->GetStatMultiplier(proto->InventoryType) * ssd->Modifier[i]) / 10000;
                AddItemMod(stats, uint32(ssd->StatMod[i]), float(value));
            }
            else
                AddItemMod(stats, proto->ItemStat[i].ItemStatType, float(proto->ItemStat[i].ItemStatValue));
        }
    }

    // Everything a worn piece gives that leaves with it: its stats, gems, socket bonus
    // and enchant. Taken out of the bot's totals when a candidate for its slot is valued.
    BotStatVector WornStats(Player* bot, Item* item)
    {
        BotStatVector stats{};
        if (!item || !item->GetTemplate())
            return stats;
        // The stats the item really gives: raised items (Destiny Weaver) at their raised level.
        AddTemplateStats(stats, bot, item->GetStatTemplate());
        AddRandomStats(stats, item->GetTemplate(), item);
        for (uint32 slot : { uint32(PERM_ENCHANTMENT_SLOT), uint32(SOCK_ENCHANTMENT_SLOT), uint32(SOCK_ENCHANTMENT_SLOT_2),
                             uint32(SOCK_ENCHANTMENT_SLOT_3), uint32(BONUS_ENCHANTMENT_SLOT), uint32(PRISMATIC_ENCHANTMENT_SLOT) })
            AddEnchantStats(stats, sSpellItemEnchantmentStore.LookupEntry(item->GetEnchantmentId(EnchantmentSlot(slot))), 1.0f);
        return stats;
    }

    bool IsHitRating(CombatRating rating)
    {
        return rating == CombatRating::CR_HIT_MELEE || rating == CombatRating::CR_HIT_RANGED
            || rating == CombatRating::CR_HIT_SPELL;
    }

    bool IsHasteRating(CombatRating rating)
    {
        return rating == CombatRating::CR_HASTE_MELEE || rating == CombatRating::CR_HASTE_RANGED
            || rating == CombatRating::CR_HASTE_SPELL;
    }

    // How much of one stat the bot has towards a cap, in rating points.
    float CurrentStat(Player* bot, BotStat stat, CombatRating capRating)
    {
        auto rating = [bot](CombatRating cr)
        {
            return float(bot->GetUInt32Value(PLAYER_FIELD_COMBAT_RATINGS + uint32(cr)));
        };
        switch (stat)
        {
            case BOT_STAT_HIT:       return rating(IsHitRating(capRating) ? capRating : CombatRating::CR_HIT_MELEE);
            case BOT_STAT_EXPERTISE: return rating(CombatRating::CR_EXPERTISE);
            case BOT_STAT_HASTE:     return rating(IsHasteRating(capRating) ? capRating : CombatRating::CR_HASTE_MELEE);
            case BOT_STAT_SPIRIT:    return bot->GetStat(STAT_SPIRIT);
            default:                 return 0.0f;
        }
    }

    // Average weight factor of `amount` points added on top of `base` in a capped pool;
    // points below the first cap count belowFactor.
    float CapFactor(BotStatCap const& cap, float multiplier, float base, float amount, float belowFactor)
    {
        if (amount <= 0.0f || multiplier <= 0.0f)
            return 1.0f;
        float covered = 0.0f;   // weighted points
        float from = base;
        float const to = base + amount;
        float factor = belowFactor;
        for (uint8 t = 0; t <= cap.tierCount && from < to; ++t)
        {
            float const edge = t < cap.tierCount ? cap.pct[t] / multiplier : to;
            if (edge > from)
            {
                float const end = std::min(edge, to);
                covered += (end - from) * factor;
                from = end;
            }
            if (t < cap.tierCount)
                factor = cap.factorAbove[t];
        }
        return covered / amount;
    }

    // A piece's stats against the bot's caps: points that push a capped pool past its cap
    // count for what they are worth there. `slot` is the equipment slot the piece would
    // take: what the piece worn there gives is taken out of the bot's totals first.
    float ValueStats(Player* bot, BotStatWeights const& weights, BotStatVector const& stats, uint8 slot,
        float mustReachBoost, BotStatVector const* exclude)
    {
        float factor[BOT_STAT_MAX];
        std::fill(std::begin(factor), std::end(factor), 1.0f);

        if (!weights.caps.empty())
        {
            Item* worn = slot < EQUIPMENT_SLOT_END ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot) : nullptr;
            BotStatVector const wornStats = WornStats(bot, worn);
            for (BotStatCap const& cap : weights.caps)
            {
                CombatRating const rating = CombatRating(cap.rating);
                float base = 0.0f;
                float amount = 0.0f;
                for (uint32 s = 0; s < BOT_STAT_MAX; ++s)
                {
                    if (!(cap.statMask & (1u << s)))
                        continue;
                    float const excluded = exclude ? (*exclude)[s] : 0.0f;
                    base += std::max(0.0f, CurrentStat(bot, BotStat(s), rating) - wornStats[s] - excluded);
                    amount += stats[s];
                }
                float const below = cap.mustReach ? mustReachBoost : 1.0f;
                float const f = CapFactor(cap, bot->GetRatingMultiplier(rating), base, amount, below);
                for (uint32 s = 0; s < BOT_STAT_MAX; ++s)
                    if (cap.statMask & (1u << s))
                        factor[s] = f;
            }
        }

        float score = 0.0f;
        for (uint32 s = 0; s < BOT_STAT_MAX; ++s)
            score += stats[s] * weights.Get(BotStat(s)) * factor[s];
        return score;
    }

    // Gems sorted best first for one spec, so a socket's estimate stops early.
    std::vector<std::pair<float, GemCandidate const*>> const& RankedGems(Player* bot, uint32 specId,
        BotStatWeights const& weights)
    {
        static std::unordered_map<uint32, std::vector<std::pair<float, GemCandidate const*>>> cache;
        uint32 const key = specId * 16 + bot->getClass();
        auto it = cache.find(key);
        if (it != cache.end())
            return it->second;
        std::vector<std::pair<float, GemCandidate const*>> ranked;
        for (GemCandidate const& gem : GemCandidates())
            if (float const score = ScoreEnchant(gem.enchant, bot, weights); score > 0.0f)
                ranked.emplace_back(score, &gem);
        std::sort(ranked.begin(), ranked.end(),
            [](auto const& a, auto const& b) { return a.first > b.first; });
        return cache.emplace(key, std::move(ranked)).first->second;
    }

    // Each socket counted as the best gem maintenance could put in it.
    float ScoreSockets(Player* bot, uint32 specId, ItemTemplate const* proto, BotStatWeights const& weights)
    {
        float score = 0.0f;
        auto const& ranked = RankedGems(bot, specId, weights);
        for (uint8 i = 0; i < MAX_GEM_SOCKETS; ++i)
        {
            uint32 const color = proto->Socket[i].Color;
            if (!color || (color & (SOCKET_COLOR_HYDRAULIC | SOCKET_COLOR_COGWHEEL)))
                continue;
            bool const metaSocket = color == SOCKET_COLOR_META;
            for (auto const& [gemScore, gem] : ranked)
            {
                ItemTemplate const* gemProto = gem->proto;
                if ((gem->color == SOCKET_COLOR_META) != metaSocket)
                    continue;
                if (gemProto->RequiredLevel > bot->getLevel() || gemProto->ItemLevel > proto->ItemLevel)
                    continue;
                if (!HasSkill(bot, gemProto->RequiredSkill, gemProto->RequiredSkillRank) || !HasReputation(bot, gemProto))
                    continue;
                score += gemScore;
                break;
            }
        }
        return score;
    }

    // What another spec's piece is worth, as a share of its weighted stats.
    constexpr float kWrongPrimaryFactor = 0.25f;

    // True when the item carries a primary stat the spec does not use (strength, agility,
    // intellect; spell power counts with intellect) and none that it does.
    bool WrongPrimary(BotStatWeights const& weights, BotStatVector const& stats)
    {
        bool useful = false;
        bool useless = false;
        for (BotStat stat : { BOT_STAT_STRENGTH, BOT_STAT_AGILITY, BOT_STAT_INTELLECT, BOT_STAT_SPELL_POWER })
        {
            if (stats[stat] <= 0.0f)
                continue;
            if (weights.Get(stat) > 0.0f)
                useful = true;
            else
                useless = true;
        }
        return useless && !useful;
    }
}

float BotEnchants::GearValue(Player* bot, uint32 specId, ItemTemplate const* proto, Item* item, uint8 slot,
    float mustReachBoost)
{
    if (!bot || !proto)
        return 0.0f;
    BotStatWeights const& weights = BotStatWeightTable::For(specId, bot->getClass());

    BotStatVector stats{};
    AddTemplateStats(stats, bot, proto);
    AddRandomStats(stats, proto, item);
    float score = ValueStats(bot, weights, stats, slot, mustReachBoost);
    // Another spec's piece (a strength shield or agility mail on a resto shaman) only
    // counts its secondary stats; players would not take it, so it ranks far below
    // a piece with the right primary stat.
    bool const wrongPrimary = WrongPrimary(weights, stats);
    score += ScoreSockets(bot, specId, proto, weights);

    // Weapon damage drives physical specs' abilities: 1 DPS = 14 attack power,
    // and one point of strength or agility is 2 attack power.
    if (proto->Class == ITEM_CLASS_WEAPON && !weights.Get(BOT_STAT_INTELLECT))
    {
        float const primary = std::max(weights.Get(BOT_STAT_STRENGTH), weights.Get(BOT_STAT_AGILITY));
        score += proto->DPS * 7.0f * primary;
    }

    if (wrongPrimary)
        score *= kWrongPrimaryFactor;

    // Breaks ties between pieces with the same weighted stats.
    return score + float(proto->ItemLevel) * 0.01f;
}

bool BotEnchants::HasWrongPrimary(Player* bot, uint32 specId, ItemTemplate const* proto, Item* item)
{
    if (!bot || !proto)
        return false;
    BotStatVector stats{};
    AddTemplateStats(stats, bot, proto);
    AddRandomStats(stats, proto, item);
    return WrongPrimary(BotStatWeightTable::For(specId, bot->getClass()), stats);
}
