/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/
#include "../src/BotQuestSpawns.h"
#include <cassert>
#include <limits>
#include <thread>
#include <vector>
#include <utility>

int main()
{
    BotQuestSpawns index;
    BotQuestSpawns::Position result{};
    float distance = -1;
    assert(!index.Nearest(false, 0, 42, {0, 0, 0}, result, distance));
    for (unsigned i = 0; i < 16; ++i)
        index.Add(false, 0, 42, {float(100 - i), 0, 0});
    index.Add(false, 0, 42, {3, 4, 12}); // nearest beyond the old LIMIT 8
    index.Add(true, 0, 42, {1, 0, 0}); // same ID, different type
    index.Add(false, 1, 42, {2, 0, 0}); // same ID, different map
    index.Add(false, 0, 43, {0, 0, 0});
    index.Add(false, 0, 44, {std::numeric_limits<float>::quiet_NaN(), 0, 0});
    assert(index.Nearest(false, 0, 42, {0, 0, 0}, result, distance));
    assert(distance == 169 && result.z == 12);
    assert(index.Nearest(true, 0, 42, {0, 0, 0}, result, distance) && distance == 1);
    assert(index.Nearest(false, 1, 42, {0, 0, 0}, result, distance) && distance == 4);
    assert(!index.Nearest(false, 2, 42, {0, 0, 0}, result, distance));
    assert(!index.Nearest(false, 0, 44, {0, 0, 0}, result, distance));
    assert(index.Nearest(false, 0, 42, {85, 0, 0}, result, distance) && distance == 0);

    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread < 8; ++thread)
        workers.emplace_back([&index]()
        {
            BotQuestSpawns const& snapshot = index;
            for (unsigned bot = 0; bot < 2405; ++bot)
            {
                BotQuestSpawns::Position point{};
                float d = -1;
                assert(snapshot.Nearest(false, 0, 42, {0, 0, 0}, point, d) && d == 169);
                assert(!snapshot.Nearest(true, 1, 42, {0, 0, 0}, point, d));
            }
        });
    for (auto& worker : workers)
        worker.join();
    BotQuestSpawns replacement;
    replacement.Add(false, 0, 42, {8, 0, 0});
    index = std::move(replacement); // publish only after workers finish
    assert(index.Nearest(false, 0, 42, {0, 0, 0}, result, distance) && distance == 64);
    assert(!index.Nearest(true, 0, 42, {0, 0, 0}, result, distance));
}
