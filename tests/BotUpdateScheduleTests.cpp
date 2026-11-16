/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "../src/BotUpdateSchedule.h"
#include <array>
#include <cassert>
#include <limits>

int main()
{
    std::array<unsigned, 10> buckets{};
    for (std::uint64_t guid = 1; guid <= 1405; ++guid)
    {
        auto timer = BotUpdateSchedule::InitialOffset(guid);
        ++buckets[timer / 50];
        unsigned decisions = 0;
        for (unsigned tick = 0; tick < 100; ++tick)
            decisions += BotUpdateSchedule::Advance(timer, 50);
        assert(decisions == 10);
        assert(timer == BotUpdateSchedule::InitialOffset(guid));
        auto const before = timer;
        assert(BotUpdateSchedule::Advance(timer, 7904));
        assert(timer == (before + 7904) % 500);
    }
    for (auto count : buckets)
        assert(count >= 130 && count <= 150);
    std::uint32_t timer = 499;
    assert(BotUpdateSchedule::Advance(timer, std::numeric_limits<std::uint32_t>::max()));
    assert(timer == (std::uint64_t(499) + std::numeric_limits<std::uint32_t>::max()) % 500);
    assert(!BotUpdateSchedule::Advance(timer, 0));
}
