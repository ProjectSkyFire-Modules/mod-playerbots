/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* Enchants and gems for a bot's equipped gear, chosen by its spec's stat weights.
*/

#ifndef _SF_BOT_ENCHANTS_H
#define _SF_BOT_ENCHANTS_H

#include "Define.h"

class Item;
class Player;
struct ItemTemplate;

namespace BotEnchants
{
    struct Result
    {
        uint32 enchants = 0;
        uint32 gems = 0;
    };

    // Enchants first (a belt buckle adds a socket), then gems in empty sockets.
    // replaceEnchants: swap an existing enchant for a better one; when false only
    // items without an enchant get one. Gems only ever go into empty sockets.
    Result Apply(Player* bot, uint32 specId, bool enchants, bool gems, bool replaceEnchants);

    // An enchant may be worth at most this share of the gear's stat budget
    // (RandPropPoints of an epic chest at its item level), in primary-stat points.
    void SetEnchantBudgetShare(float share);

    // Autogear's score for a piece: its stats weighted for the spec (random
    // suffixes as rolled when item is given, else their average), valued against
    // the spec's caps (BotStatWeights::caps) from the bot's own ratings without
    // the piece worn in `slot`; each socket as the best gem maintenance would put
    // there, and weapon DPS for physical specs. Enchants are not counted: they do
    // not move with the item.
    float GearValue(Player* bot, uint32 specId, ItemTemplate const* proto, Item* item = nullptr,
        uint8 slot = 255);
}

#endif
