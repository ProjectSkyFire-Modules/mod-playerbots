/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/

#ifndef SKYFIRE_BOT_UPDATE_SCHEDULE_H
#define SKYFIRE_BOT_UPDATE_SCHEDULE_H

#include <cstdint>

namespace BotUpdateSchedule
{
    constexpr std::uint32_t Interval = 500;

    inline std::uint32_t InitialOffset(std::uint64_t guid)
    {
        return static_cast<std::uint32_t>((guid * 2654435761ULL) % Interval);
    }

    inline bool Advance(std::uint32_t& timer, std::uint32_t diff)
    {
        std::uint64_t const elapsed = std::uint64_t(timer) + diff;
        // Preserve each bot's phase through irregular map ticks. Never run
        // catch-up decisions in a loop after a stall.
        timer = static_cast<std::uint32_t>(elapsed % Interval);
        return elapsed >= Interval;
    }
}

#endif
