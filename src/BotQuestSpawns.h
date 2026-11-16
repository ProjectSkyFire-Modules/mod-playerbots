/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/

#ifndef SKYFIRE_BOT_QUEST_SPAWNS_H
#define SKYFIRE_BOT_QUEST_SPAWNS_H

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Built on the world thread outside map updates; read-only during AI ticks.
class BotQuestSpawns
{
public:
    struct Position { float x, y, z; };

    void Add(bool gameObject, std::uint32_t map, std::uint32_t entry, Position position)
    {
        if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z))
            _spawns[gameObject][Key(map, entry)].push_back(position);
    }

    bool Nearest(bool gameObject, std::uint32_t map, std::uint32_t entry,
        Position origin, Position& result, float& distanceSquared) const
    {
        auto const& store = _spawns[gameObject];
        auto const found = store.find(Key(map, entry));
        if (found == store.end())
            return false;
        float best = -1.0f;
        for (Position const& point : found->second)
        {
            float const dx = point.x - origin.x;
            float const dy = point.y - origin.y;
            float const dz = point.z - origin.z;
            float const distance = dx * dx + dy * dy + dz * dz;
            if (best < 0.0f || distance < best)
            {
                best = distance;
                result = point;
            }
        }
        distanceSquared = best;
        return best >= 0.0f;
    }

private:
    static std::uint64_t Key(std::uint32_t map, std::uint32_t entry)
    {
        return (std::uint64_t(map) << 32) | entry;
    }
    std::unordered_map<std::uint64_t, std::vector<Position>> _spawns[2];
};

#endif
