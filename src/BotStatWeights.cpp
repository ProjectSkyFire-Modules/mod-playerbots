/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* Per-specialization stat weights.
*
* The priorities are Wowhead's MoP Classic pre-raid stat priorities ("The Best
* Stats for Each Specialization", tanks: Balanced profile for Blood and
* Protection warriors, Defensive for Protection paladins and Brewmasters).
* A stat's weight is its rank: 1 for the first, minus the rank step (0.05) for
* each rank down; stats on one rank share it. Unlisted stats weigh 0.
* Per point, primary stats (Str/Agi/Int, spell power) count double and
* stamina 1.5 times a secondary rating, as MoP gems give 160 primary,
* 240 stamina or 320 rating. Caps (hit, expertise, spell hit, haste soft caps
* and breakpoints) are in CapsFor; autogear values each piece against them.
*/

#include "BotStatWeights.h"
#include "ItemPrototype.h"
#include "SharedDefines.h"
#include "Unit.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <string>

namespace
{
    struct SpecPriority
    {
        uint32 specId;
        char const* priority; // '>' between ranks, spaces between stats on one rank
    };

    SpecPriority const kPriorities[] =
    {
        { 250, "hit exp > parry > dodge > str > sta > crit > haste > mastery" }, // Blood
        { 251, "str > hit exp > crit > mastery > haste" },                       // Frost DK
        { 252, "str > hit exp > crit > haste > mastery" },                       // Unholy
        { 102, "int > sp > hit spi exp > haste > crit > mastery" },              // Balance
        { 103, "agi > hit exp > mastery > crit > haste" },                       // Feral
        { 104, "sta > hit exp > crit > agi > haste > mastery" },                 // Guardian
        { 105, "int > haste > spi mastery > crit" },                             // Restoration druid
        { 253, "agi > hit exp > crit > haste > mastery" },                       // Beast Mastery
        { 254, "agi > hit exp > crit > haste > mastery" },                       // Marksmanship
        { 255, "agi > hit exp > crit > haste > mastery" },                       // Survival
        {  62, "int > hit exp > mastery > haste > crit" },                       // Arcane
        {  63, "int > hit exp > crit > haste > mastery" },                       // Fire
        {  64, "int > hit exp > haste > crit > mastery" },                       // Frost mage
        { 268, "agi sta > hit > exp > mastery > haste crit" },                   // Brewmaster
        { 269, "agi > hit exp > haste > crit > mastery" },                       // Windwalker
        { 270, "int sp > spi > haste > crit > mastery" },                        // Mistweaver
        {  65, "spi > int > haste > mastery > crit" },                           // Holy paladin
        {  66, "hit > exp > mastery sta > haste > dodge parry > str" },          // Protection paladin
        {  70, "hit exp > haste > str > mastery > crit" },                       // Retribution
        { 256, "int > crit spi > mastery > haste" },                             // Discipline
        { 257, "int > spi > crit > mastery > haste" },                           // Holy priest
        { 258, "int > hit spi exp > haste > crit > mastery" },                   // Shadow
        { 259, "agi > hit exp > mastery > haste > crit" },                       // Assassination
        { 260, "agi > hit exp > haste > mastery > crit" },                       // Combat
        { 261, "agi > hit exp > haste > crit > mastery" },                       // Subtlety
        { 262, "int > hit spi exp > haste > mastery > crit" },                   // Elemental
        { 263, "agi > hit exp > mastery > haste > crit" },                       // Enhancement
        { 264, "int > haste > spi > crit > mastery" },                           // Restoration shaman
        { 265, "int > hit exp > haste > mastery > crit" },                       // Affliction
        { 266, "int > hit exp > haste > mastery > crit" },                       // Demonology
        { 267, "int > hit exp > mastery > haste > crit" },                       // Destruction
        {  71, "hit exp > str > crit > mastery > haste" },                       // Arms
        {  72, "hit exp > str > crit > mastery > haste" },                       // Fury
        {  73, "hit > exp > parry > dodge > crit > str > sta > mastery > haste" }, // Protection warrior
    };

    // Spec used for a class whose bot has no specialization yet.
    uint32 DefaultSpec(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_WARRIOR:      return 71;
            case CLASS_PALADIN:      return 70;
            case CLASS_HUNTER:       return 253;
            case CLASS_ROGUE:        return 260;
            case CLASS_PRIEST:       return 258;
            case CLASS_DEATH_KNIGHT: return 251;
            case CLASS_SHAMAN:       return 262;
            case CLASS_MAGE:         return 64;
            case CLASS_WARLOCK:      return 265;
            case CLASS_MONK:         return 269;
            case CLASS_DRUID:        return 102;
            default:                 return 0;
        }
    }

    bool ParseStat(std::string const& token, BotStat& stat)
    {
        static std::pair<char const*, BotStat> const names[] =
        {
            { "str", BOT_STAT_STRENGTH }, { "agi", BOT_STAT_AGILITY }, { "int", BOT_STAT_INTELLECT },
            { "spi", BOT_STAT_SPIRIT }, { "sta", BOT_STAT_STAMINA }, { "sp", BOT_STAT_SPELL_POWER },
            { "ap", BOT_STAT_ATTACK_POWER }, { "hit", BOT_STAT_HIT }, { "exp", BOT_STAT_EXPERTISE },
            { "crit", BOT_STAT_CRIT }, { "haste", BOT_STAT_HASTE }, { "mastery", BOT_STAT_MASTERY },
            { "dodge", BOT_STAT_DODGE }, { "parry", BOT_STAT_PARRY },
        };
        for (auto const& name : names)
        {
            if (token == name.first)
            {
                stat = name.second;
                return true;
            }
        }
        return false;
    }

    float PointFactor(BotStat stat)
    {
        switch (stat)
        {
            case BOT_STAT_STRENGTH:
            case BOT_STAT_AGILITY:
            case BOT_STAT_INTELLECT:
            case BOT_STAT_SPELL_POWER:
                return 2.0f;
            case BOT_STAT_STAMINA:
                return 1.5f;
            default:
                return 1.0f;
        }
    }

    // Stat caps from the same Wowhead page. Past a hard cap a point is worth little
    // (kOverCap = 0.2 of its weight); past a soft cap or haste breakpoint it drops to about
    // the next stat down the list.
    constexpr float kOverCap = 0.2f;
    constexpr uint32 Bit(BotStat stat) { return 1u << stat; }

    BotStatCap Cap(uint32 mask, CombatRating rating, float pct, float factor, bool mustReach = true)
    {
        BotStatCap cap;
        cap.mustReach = mustReach;
        cap.statMask = mask;
        cap.rating = uint8(rating);
        cap.tierCount = 1;
        cap.pct[0] = pct;
        cap.factorAbove[0] = factor;
        return cap;
    }

    BotStatCap Cap2(uint32 mask, CombatRating rating, float pct1, float factor1, float pct2, float factor2)
    {
        BotStatCap cap = Cap(mask, rating, pct1, factor1);
        cap.tierCount = 2;
        cap.pct[1] = pct2;
        cap.factorAbove[1] = factor2;
        return cap;
    }

    std::vector<BotStatCap> CapsFor(uint32 specId)
    {
        uint32 const hit = Bit(BOT_STAT_HIT);
        uint32 const exp = Bit(BOT_STAT_EXPERTISE);
        uint32 const spi = Bit(BOT_STAT_SPIRIT);
        uint32 const haste = Bit(BOT_STAT_HASTE);
        switch (specId)
        {
            // Melee: 7.5% hit, 7.5% expertise.
            case 251: case 252: case 103: case 269: case 70:
            case 259: case 260: case 261: case 263: case 71:
                return { Cap(hit, CombatRating::CR_HIT_MELEE, 7.5f, kOverCap),
                         Cap(exp, CombatRating::CR_EXPERTISE, 7.5f, kOverCap) };
            // Hunters: the same caps for ranged.
            case 253: case 254: case 255:
                return { Cap(hit, CombatRating::CR_HIT_RANGED, 7.5f, kOverCap),
                         Cap(exp, CombatRating::CR_EXPERTISE, 7.5f, kOverCap) };
            // Fury: hit past 7.5% still lands white hits up to 26.5%, as the last priority.
            case 72:
                return { Cap2(hit, CombatRating::CR_HIT_MELEE, 7.5f, 0.3f, 26.5f, kOverCap),
                         Cap(exp, CombatRating::CR_EXPERTISE, 7.5f, kOverCap) };
            // Blood (balanced): expertise past 7.5% is the last priority, up to 15%.
            case 250:
                return { Cap(hit, CombatRating::CR_HIT_MELEE, 7.5f, kOverCap),
                         Cap2(exp, CombatRating::CR_EXPERTISE, 7.5f, 0.3f, 15.0f, kOverCap) };
            // Protection warrior and paladin, Guardian, Brewmaster: expertise to the 15% hard cap.
            case 73: case 66: case 104: case 268:
                return { Cap(hit, CombatRating::CR_HIT_MELEE, 7.5f, kOverCap),
                         Cap(exp, CombatRating::CR_EXPERTISE, 15.0f, kOverCap) };
            // Balance: hit, spirit and expertise fill 15% spell hit; haste soft cap at 24.22%
            // (GCD at 1s), past which it falls just below crit.
            case 102:
                return { Cap(hit | exp | spi, CombatRating::CR_HIT_SPELL, 15.0f, kOverCap),
                         Cap(haste, CombatRating::CR_HASTE_SPELL, 24.22f, 0.9f, false) };
            // Shadow, Elemental: hit, spirit and expertise fill 15% spell hit.
            case 258: case 262:
                return { Cap(hit | exp | spi, CombatRating::CR_HIT_SPELL, 15.0f, kOverCap) };
            // Mages, warlocks: hit and expertise fill 15% spell hit.
            case 62: case 63: case 64: case 265: case 266: case 267:
                return { Cap(hit | exp, CombatRating::CR_HIT_SPELL, 15.0f, kOverCap) };
            // Healer haste breakpoints (first one of the tier): past it haste drops to last.
            case 105: // Restoration druid, 3043 rating at 90
                return { Cap(haste, CombatRating::CR_HASTE_SPELL, 7.16f, 0.85f) };
            case 264: // Restoration shaman, 3379 rating at 90
                return { Cap(haste, CombatRating::CR_HASTE_SPELL, 7.95f, 0.8f) };
            case 65:  // Holy paladin, Eternal Flame 8.25%
                return { Cap(haste, CombatRating::CR_HASTE_SPELL, 8.25f, 0.9f) };
            default:
                return {};
        }
    }

    float g_rankStep = 0.05f;
    std::map<uint32, BotStatWeights> g_cache;

    BotStatWeights Build(uint32 specId, float step)
    {
        BotStatWeights weights;
        char const* priority = nullptr;
        for (SpecPriority const& entry : kPriorities)
            if (entry.specId == specId)
                priority = entry.priority;
        if (!priority)
            return weights;

        float rankWeight[BOT_STAT_MAX] = {};
        std::istringstream in(priority);
        std::string token;
        uint32 rank = 0;
        while (in >> token)
        {
            if (token == ">")
            {
                ++rank;
                continue;
            }
            BotStat stat;
            if (ParseStat(token, stat))
                rankWeight[stat] = std::max(0.05f, 1.0f - step * float(rank));
        }

        // Spell power is worth a little less than intellect when unlisted;
        // attack power is half a point of the strength or agility it comes from.
        if (!rankWeight[BOT_STAT_SPELL_POWER] && rankWeight[BOT_STAT_INTELLECT])
            rankWeight[BOT_STAT_SPELL_POWER] = std::max(0.05f, rankWeight[BOT_STAT_INTELLECT] - step);
        float const physical = std::max(rankWeight[BOT_STAT_STRENGTH], rankWeight[BOT_STAT_AGILITY]);

        for (uint32 i = 0; i < BOT_STAT_MAX; ++i)
            weights.perPoint[i] = rankWeight[i] * PointFactor(BotStat(i));
        if (!rankWeight[BOT_STAT_ATTACK_POWER])
            weights.perPoint[BOT_STAT_ATTACK_POWER] = physical * 1.0f;
        weights.caps = CapsFor(specId);
        return weights;
    }
}

BotStat StatForItemMod(uint32 itemMod)
{
    switch (itemMod)
    {
        case ITEM_MOD_AGILITY:                  return BOT_STAT_AGILITY;
        case ITEM_MOD_STRENGTH:                 return BOT_STAT_STRENGTH;
        case ITEM_MOD_INTELLECT:                return BOT_STAT_INTELLECT;
        case ITEM_MOD_SPIRIT:                   return BOT_STAT_SPIRIT;
        case ITEM_MOD_STAMINA:                  return BOT_STAT_STAMINA;
        case ITEM_MOD_DODGE_RATING:             return BOT_STAT_DODGE;
        case ITEM_MOD_PARRY_RATING:             return BOT_STAT_PARRY;
        case ITEM_MOD_HIT_MELEE_RATING:
        case ITEM_MOD_HIT_RANGED_RATING:
        case ITEM_MOD_HIT_SPELL_RATING:
        case ITEM_MOD_HIT_RATING:               return BOT_STAT_HIT;
        case ITEM_MOD_CRIT_MELEE_RATING:
        case ITEM_MOD_CRIT_RANGED_RATING:
        case ITEM_MOD_CRIT_SPELL_RATING:
        case ITEM_MOD_CRIT_RATING:              return BOT_STAT_CRIT;
        case ITEM_MOD_HASTE_MELEE_RATING:
        case ITEM_MOD_HASTE_RANGED_RATING:
        case ITEM_MOD_HASTE_SPELL_RATING:
        case ITEM_MOD_HASTE_RATING:             return BOT_STAT_HASTE;
        case ITEM_MOD_EXPERTISE_RATING:         return BOT_STAT_EXPERTISE;
        case ITEM_MOD_MASTERY_RATING:           return BOT_STAT_MASTERY;
        case ITEM_MOD_SPELL_POWER:              return BOT_STAT_SPELL_POWER;
        case ITEM_MOD_ATTACK_POWER:
        case ITEM_MOD_RANGED_ATTACK_POWER:      return BOT_STAT_ATTACK_POWER;
        default:                                return BOT_STAT_MAX;
    }
}

float BotStatWeights::ForItemMod(uint32 itemMod) const
{
    BotStat const stat = StatForItemMod(itemMod);
    return stat == BOT_STAT_MAX ? 0.0f : Get(stat);
}

float BotStatWeights::ForCombatRating(uint32 rating) const
{
    switch (CombatRating(rating))
    {
        case CombatRating::CR_DODGE:          return Get(BOT_STAT_DODGE);
        case CombatRating::CR_PARRY:          return Get(BOT_STAT_PARRY);
        case CombatRating::CR_HIT_MELEE:
        case CombatRating::CR_HIT_RANGED:
        case CombatRating::CR_HIT_SPELL:      return Get(BOT_STAT_HIT);
        case CombatRating::CR_CRIT_MELEE:
        case CombatRating::CR_CRIT_RANGED:
        case CombatRating::CR_CRIT_SPELL:     return Get(BOT_STAT_CRIT);
        case CombatRating::CR_HASTE_MELEE:
        case CombatRating::CR_HASTE_RANGED:
        case CombatRating::CR_HASTE_SPELL:    return Get(BOT_STAT_HASTE);
        case CombatRating::CR_EXPERTISE:      return Get(BOT_STAT_EXPERTISE);
        case CombatRating::CR_MASTERY:        return Get(BOT_STAT_MASTERY);
        default:                return 0.0f;
    }
}

void BotStatWeightTable::SetRankStep(float step)
{
    step = std::clamp(step, 0.0f, 0.5f);
    if (step != g_rankStep)
        g_cache.clear();
    g_rankStep = step;
}

BotStatWeights const& BotStatWeightTable::For(uint32 specId, uint8 classId)
{
    bool known = false;
    for (SpecPriority const& entry : kPriorities)
        known = known || entry.specId == specId;
    if (!known)
        specId = DefaultSpec(classId);

    auto it = g_cache.find(specId);
    if (it == g_cache.end())
        it = g_cache.emplace(specId, Build(specId, g_rankStep)).first;
    return it->second;
}
