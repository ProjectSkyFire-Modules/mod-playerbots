/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* Per-specialization stat weights, built from stat priority lists.
*/

#ifndef _SF_BOT_STAT_WEIGHTS_H
#define _SF_BOT_STAT_WEIGHTS_H

#include "Define.h"

#include <array>
#include <vector>

enum BotStat
{
    BOT_STAT_STRENGTH,
    BOT_STAT_AGILITY,
    BOT_STAT_INTELLECT,
    BOT_STAT_SPIRIT,
    BOT_STAT_STAMINA,
    BOT_STAT_SPELL_POWER,
    BOT_STAT_ATTACK_POWER,
    BOT_STAT_HIT,
    BOT_STAT_EXPERTISE,
    BOT_STAT_CRIT,
    BOT_STAT_HASTE,
    BOT_STAT_MASTERY,
    BOT_STAT_DODGE,
    BOT_STAT_PARRY,
    BOT_STAT_MAX
};

using BotStatVector = std::array<float, BOT_STAT_MAX>;

// ITEM_MOD_* to the stat it counts as; BOT_STAT_MAX for stats no spec weighs.
BotStat StatForItemMod(uint32 itemMod);

// A stat cap: the stats in statMask (rating units, spirit 1:1) fill one pool measured
// in percent of the combat rating `rating`. Past pct[i] each point is worth
// factorAbove[i] times its usual weight.
struct BotStatCap
{
    uint32 statMask = 0;
    uint8 rating = 0;
    uint8 tierCount = 0;
    float pct[2] = {};
    float factorAbove[2] = {};
    // Wowhead says to reach it first (hit, expertise, spell hit, healer haste
    // breakpoints): maintenance fills it with gems and enchants before anything else.
    bool mustReach = false;
};

struct BotStatWeights
{
    float perPoint[BOT_STAT_MAX] = {};
    std::vector<BotStatCap> caps;

    float Get(BotStat stat) const { return perPoint[stat]; }
    // ITEM_MOD_* (item stats, gem and enchant stats); 0 for stats the spec ignores.
    float ForItemMod(uint32 itemMod) const;
    // CR_* combat rating index.
    float ForCombatRating(uint32 rating) const;
};

namespace BotStatWeightTable
{
    // Weight lost per rank down the priority list (1, 1 - step, 1 - 2*step, ...).
    void SetRankStep(float step);
    // specId: ChrSpecialization id; 0 falls back to a default spec of the class.
    BotStatWeights const& For(uint32 specId, uint8 classId);
}

#endif
