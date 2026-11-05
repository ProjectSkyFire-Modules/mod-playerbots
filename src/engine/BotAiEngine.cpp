/*
 * SkyFire playerbots — Trigger/Action/Queue engine implementation.
 */

#include "BotAiEngine.h"
#include "BotMovement.h"
#include "BotMultiplier.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include "Group.h"
#include "Player.h"
#include "Timer.h"

float BotPassiveMultiplier::GetValue(BotAction* action)
{
    if (!action)
        return 1.0f;
    // Explicit attack/pull must break passive (AC AttackMyTarget clears it).
    if (_ai && _ai->HasEngageTarget() && action->GetName() == "combat")
        return 1.0f;
    std::string const& n = action->GetName();
    if (n == "follow" || n == "stay" || n == "rest" || n == "vendor" || n == "heal")
        return 1.0f;
    if (n.find("follow") != std::string::npos || n.find("stay") != std::string::npos)
        return 1.0f;
    return 0.0f;
}

namespace
{
    // --- Triggers ---

    class CombatTrigger : public BotTrigger
    {
    public:
        CombatTrigger(PlayerbotAI* ai) : BotTrigger(ai, "combat", 300) {}
        bool IsActive() override
        {
            Player* bot = _ai->GetBot();
            if (!bot || !bot->IsAlive())
                return false;
            // Ordered attack / pull must engage even before the bot is flagged in combat.
            if (_ai->HasEngageTarget())
                return true;
            if (bot->IsInCombat() || !bot->getAttackers().empty())
                return true;
            if (_ai->IsGroupInCombatPublic())
                return true;
            // Quest/grind bots must pull OOC — high-level bots in starter zones
            // never get aggro, so waiting for IsInCombat never starts a fight.
            // HandleCombat returns false when nothing is in range so travel can run.
            return _ai->WantsOpenWorldPullsPublic();
        }
    };

    class LowResourcesTrigger : public BotTrigger
    {
    public:
        LowResourcesTrigger(PlayerbotAI* ai) : BotTrigger(ai, "low resources", 0) {}
        bool IsActive() override
        {
            Player* bot = _ai->GetBot();
            if (!bot || !bot->IsAlive() || bot->IsInCombat())
                return false;
            if (_ai->IsGroupInCombatPublic())
                return false;
            // Ordered attack / pull outranks drinking.
            if (_ai->HasEngageTarget())
                return false;
            if (!_ai->HasStrategy("food", BotState::NonCombat) && !_ai->IsForceResting())
                return false;
            return _ai->NeedsRestPublic();
        }
    };

    class InjuredAllyTrigger : public BotTrigger
    {
    public:
        InjuredAllyTrigger(PlayerbotAI* ai) : BotTrigger(ai, "injured ally", 0) {}
        bool IsActive() override
        {
            return _ai->NeedsOocHealPublic();
        }
    };

    class FarFromMasterTrigger : public BotTrigger
    {
    public:
        FarFromMasterTrigger(PlayerbotAI* ai) : BotTrigger(ai, "far from master", 500) {}
        bool IsActive() override
        {
            if (_ai->IsClientControlled() || _ai->HasStrategy("stay", BotState::NonCombat))
                return false;
            // Don't chase the master when we were ordered onto a target.
            if (_ai->HasEngageTarget())
                return false;
            if (_ai->NeedsOocHealPublic())
                return false;
            // Prefer looting nearby corpses over re-issuing follow.
            if (_ai->HasNearbyLootPublic())
                return false;
            return _ai->ShouldFollowPublic();
        }
    };

    class NearbyLootTrigger : public BotTrigger
    {
    public:
        NearbyLootTrigger(PlayerbotAI* ai) : BotTrigger(ai, "nearby loot", 500) {}
        bool IsActive() override
        {
            return _ai->HasNearbyLootPublic();
        }
    };

    class HasTravelDestinationTrigger : public BotTrigger
    {
    public:
        HasTravelDestinationTrigger(PlayerbotAI* ai) : BotTrigger(ai, "has travel destination", 300) {}
        bool IsActive() override
        {
            if (_ai->IsClientControlled())
                return false;
            if (_ai->NeedsRestPublic() || _ai->IsForceResting())
                return false;
            if (_ai->NeedsOocHealPublic())
                return false;
            Player* bot = _ai->GetBot();
            if (!bot || !bot->IsAlive() || bot->IsInCombat())
                return false;
            if (_ai->IsGroupInCombatPublic() || _ai->HasEngageTarget())
                return false;
            return _ai->HasTravelDestination();
        }
    };

    // --- Actions ---

    class CombatAction : public BotAction
    {
    public:
        CombatAction(PlayerbotAI* ai) : BotAction(ai, "combat") {}
        bool IsUseful() override
        {
            Player* bot = _ai->GetBot();
            return bot && bot->IsAlive();
        }
        bool Execute() override
        {
            if (_ai->IsClientControlled() && !_ai->IsSelfWalkEnabled())
                return _ai->RunCombatCastOnly();
            return _ai->RunCombat();
        }
    };

    class RestAction : public BotAction
    {
    public:
        RestAction(PlayerbotAI* ai) : BotAction(ai, "rest") {}
        bool IsUseful() override
        {
            Player* bot = _ai->GetBot();
            if (!bot || !bot->IsAlive() || bot->IsInCombat())
                return false;
            if (_ai->IsGroupInCombatPublic())
                return false;
            if (!bot->getAttackers().empty())
                return false;
            if (_ai->HasEngageTarget())
                return false;
            return _ai->NeedsRestPublic();
        }
        bool Execute() override { return _ai->RunRest(); }
    };

    class HealAction : public BotAction
    {
    public:
        HealAction(PlayerbotAI* ai) : BotAction(ai, "heal") {}
        bool IsUseful() override
        {
            Player* bot = _ai->GetBot();
            if (!bot || !bot->IsAlive())
                return false;
            return _ai->NeedsOocHealPublic();
        }
        bool IsPossible() override
        {
            Player* bot = _ai->GetBot();
            return bot && !bot->HasUnitState(UNIT_STATE_CASTING);
        }
        bool Execute() override { return _ai->RunHeal(); }
    };

    class FollowAction : public BotAction
    {
    public:
        FollowAction(PlayerbotAI* ai) : BotAction(ai, "follow") {}
        bool IsUseful() override
        {
            if (_ai->IsClientControlled())
                return false;
            if (_ai->NeedsRestPublic() || _ai->IsForceResting())
                return false;
            if (_ai->NeedsOocHealPublic())
                return false; // top up allies before chasing the master
            if (_ai->HasTravelDestination())
                return false;
            if (_ai->HasEngageTarget())
                return false;
            if (_ai->HasNearbyLootPublic())
                return false; // finish loot before re-follow
            if (_ai->HasStrategy("stay", BotState::NonCombat)
                || _ai->HasStrategy("stay", BotState::Combat))
                return false;
            // Healers / ranged never formation-follow the master during combat.
            Player* bot = _ai->GetBot();
            if (bot && (bot->IsInCombat() || _ai->IsGroupInCombatPublic()))
            {
                if (_ai->GetCombatRolePublic() == 1 || _ai->IsRangedClassPublic())
                    return false;
            }
            return _ai->HasStrategy("follow", BotState::NonCombat) || _ai->ShouldFollowPublic();
        }
        bool IsPossible() override { return true; } // RunFollow no-ops while casting
        bool Execute() override
        {
            _ai->RunFollow();
            return true;
        }
    };

    class TravelAction : public BotAction
    {
    public:
        TravelAction(PlayerbotAI* ai) : BotAction(ai, "travel") {}
        bool IsUseful() override
        {
            if (_ai->IsClientControlled())
                return false;
            if (_ai->NeedsRestPublic() || _ai->IsForceResting())
                return false;
            Player* bot = _ai->GetBot();
            if (!bot || !bot->IsAlive() || bot->IsInCombat())
                return false;
            if (_ai->IsGroupInCombatPublic() || _ai->HasEngageTarget())
                return false;
            return _ai->HasTravelDestination();
        }
        bool IsPossible() override { return BotMovement::CanMove(_ai->GetBot()); }
        bool Execute() override { return _ai->RunTravel(); }
    };

    class StayAction : public BotAction
    {
    public:
        StayAction(PlayerbotAI* ai) : BotAction(ai, "stay") {}
        bool IsUseful() override
        {
            if (_ai->IsClientControlled())
                return false;
            return _ai->HasStrategy("stay", BotState::NonCombat)
                || _ai->HasStrategy("stay", BotState::Combat);
        }
        bool Execute() override
        {
            _ai->RunStay();
            return true;
        }
    };

    class LootAction : public BotAction
    {
    public:
        LootAction(PlayerbotAI* ai) : BotAction(ai, "loot") {}
        bool IsUseful() override
        {
            if (!_ai->HasStrategy("loot", BotState::NonCombat))
                return false;
            if (_ai->IsClientControlled() && !_ai->IsSelfWalkEnabled())
                return false;
            Player* bot = _ai->GetBot();
            if (!bot || bot->IsInCombat() || _ai->IsGroupInCombatPublic())
                return false;
            return _ai->HasNearbyLootPublic();
        }
        bool IsPossible() override { return BotMovement::CanMove(_ai->GetBot()); }
        bool Execute() override { return _ai->RunLoot(); }
    };

    class WanderAction : public BotAction
    {
    public:
        WanderAction(PlayerbotAI* ai) : BotAction(ai, "wander") {}
        bool IsUseful() override
        {
            if (_ai->IsClientControlled())
                return false;
            if (_ai->NeedsRestPublic() || _ai->IsForceResting())
                return false;
            if (_ai->HasTravelDestination())
                return false;
            if (_ai->HasStrategy("stay", BotState::NonCombat))
                return false;
            if (_ai->ShouldFollowPublic())
                return false;
            return true;
        }
        bool IsPossible() override { return BotMovement::CanMove(_ai->GetBot()); }
        bool Execute() override
        {
            _ai->RunWander();
            return true;
        }
    };

    class VendorAction : public BotAction
    {
    public:
        VendorAction(PlayerbotAI* ai) : BotAction(ai, "vendor") {}
        bool IsUseful() override
        {
            if (_ai->IsClientControlled())
                return false;
            if (_ai->HasTravelDestination())
                return false;
            if (!_ai->NeedsVendorWorkPublic())
                return false;
            // Follow bots only break for urgent bag/repair pressure (or chat sell).
            if (_ai->ShouldFollowPublic() && !_ai->NeedsUrgentVendorPublic())
                return false;
            return true;
        }
        bool IsPossible() override { return BotMovement::CanMove(_ai->GetBot()); }
        bool Execute() override
        {
            _ai->RunVendor();
            return true;
        }
    };
}

BotAiEngine::BotAiEngine(PlayerbotAI* ai) : _ai(ai)
{
    RegisterCoreActions();
    Rebuild();
}

BotAiEngine::~BotAiEngine() = default;

void BotAiEngine::RegisterCoreActions()
{
    _actions["combat"] = std::make_unique<CombatAction>(_ai);
    _actions["rest"] = std::make_unique<RestAction>(_ai);
    _actions["heal"] = std::make_unique<HealAction>(_ai);
    _actions["travel"] = std::make_unique<TravelAction>(_ai);
    _actions["follow"] = std::make_unique<FollowAction>(_ai);
    _actions["stay"] = std::make_unique<StayAction>(_ai);
    _actions["loot"] = std::make_unique<LootAction>(_ai);
    _actions["wander"] = std::make_unique<WanderAction>(_ai);
    _actions["vendor"] = std::make_unique<VendorAction>(_ai);
}

void BotAiEngine::Rebuild()
{
    _triggerNodes.clear();
    _triggerOwners.clear();
    _multipliers.clear();

    auto addTrigger = [&](std::unique_ptr<BotTrigger> trig, std::vector<BotNextAction> handlers)
    {
        BotTriggerNode node(trig->GetName(), std::move(handlers));
        node.SetTrigger(trig.get());
        _triggerOwners.push_back(std::move(trig));
        _triggerNodes.push_back(std::move(node));
    };

    addTrigger(std::make_unique<CombatTrigger>(_ai),
        { BotNextAction("combat", BotRelevance::Combat) });

    if (_ai->HasStrategy("food", BotState::NonCombat))
        addTrigger(std::make_unique<LowResourcesTrigger>(_ai),
            { BotNextAction("rest", BotRelevance::Rest) });

    // OOC heal above follow so injured party members get topped before the pack moves.
    if (_ai->HasStrategy("heal", BotState::NonCombat))
        addTrigger(std::make_unique<InjuredAllyTrigger>(_ai),
            { BotNextAction("heal", BotRelevance::Rest + 5.0f) });

    // Above follow/loot, below rest/combat — finish go destinations.
    addTrigger(std::make_unique<HasTravelDestinationTrigger>(_ai),
        { BotNextAction("travel", BotRelevance::Move + 22.0f) });

    if (_ai->HasStrategy("follow", BotState::NonCombat)
        && !_ai->HasStrategy("stay", BotState::NonCombat))
        addTrigger(std::make_unique<FarFromMasterTrigger>(_ai),
            { BotNextAction("follow", BotRelevance::Move + 10.0f) });

    if (_ai->HasStrategy("loot", BotState::NonCombat))
        addTrigger(std::make_unique<NearbyLootTrigger>(_ai),
            { BotNextAction("loot", BotRelevance::Move + 20.0f) });

    // AC PassiveMultiplier when +passive on either engine.
    if (_ai->HasStrategy("passive", BotState::Combat)
        || _ai->HasStrategy("passive", BotState::NonCombat))
        _multipliers.push_back(std::make_unique<BotPassiveMultiplier>(_ai));
}

void BotAiEngine::ProcessTriggers()
{
    uint32 const now = getMSTime();
    for (BotTriggerNode& node : _triggerNodes)
    {
        BotTrigger* trig = node.GetTrigger();
        if (!trig || !trig->NeedCheck(now))
            continue;
        if (!trig->IsActive())
            continue;
        for (BotNextAction const& next : node.GetHandlers())
            _queue.Push(next.GetName(), next.GetRelevance());
    }
}

void BotAiEngine::PushDefaultActions()
{
    // Defaults mirror AC strategy getDefaultActions().
    // Ordered pull/attack keeps combat as the default even while still OOC.
    if (_activeState == BotState::Combat || _ai->HasEngageTarget())
    {
        _queue.Push("combat", BotRelevance::Default);
        return;
    }

    // Sticky rest: keep drinking every tick so travel/follow cannot interleave.
    if (_ai->NeedsRestPublic() || _ai->IsForceResting())
    {
        _queue.Push("rest", BotRelevance::Rest);
        // Still allow OOC heals to interleave above drink when allies are hurt.
        if (_ai->NeedsOocHealPublic())
            _queue.Push("heal", BotRelevance::Rest + 5.0f);
        return;
    }

    if (_ai->NeedsOocHealPublic())
    {
        _queue.Push("heal", BotRelevance::Rest + 5.0f);
        return;
    }

    if (_ai->HasStrategy("stay", BotState::NonCombat)
        || _ai->HasStrategy("stay", BotState::Combat))
    {
        _queue.Push("stay", BotRelevance::Default);
        if (_ai->HasStrategy("loot", BotState::NonCombat))
            _queue.Push("loot", BotRelevance::Normal);
        _queue.Push("vendor", BotRelevance::Idle + 1.0f);
        return;
    }

    if (_ai->HasTravelDestination())
    {
        _queue.Push("travel", BotRelevance::Default);
        return;
    }

    if (_ai->HasStrategy("follow", BotState::NonCombat) || _ai->ShouldFollowPublic())
    {
        _queue.Push("follow", BotRelevance::Default);
        if (_ai->HasStrategy("loot", BotState::NonCombat))
            _queue.Push("loot", BotRelevance::Normal);
        return;
    }

    if (_ai->HasStrategy("loot", BotState::NonCombat))
        _queue.Push("loot", BotRelevance::Normal);
    _queue.Push("vendor", BotRelevance::Idle + 2.0f);
    _queue.Push("wander", BotRelevance::Idle + 1.0f);
}

BotAction* BotAiEngine::FindAction(std::string const& name)
{
    auto it = _actions.find(name);
    if (it == _actions.end())
        return nullptr;
    return it->second.get();
}

bool BotAiEngine::DoNextAction()
{
    if (!_ai || !_ai->GetBot() || !_ai->GetBot()->IsAlive())
        return false;

    Player* bot = _ai->GetBot();
    bool const hasEngage = _ai->HasEngageTarget();
    bool const inCombat = hasEngage || bot->IsInCombat() || _ai->IsGroupInCombatPublic()
        || !bot->getAttackers().empty();
    _activeState = inCombat ? BotState::Combat : BotState::NonCombat;

    _queue.Clear();
    ProcessTriggers();
    PushDefaultActions();

    // Cast-only self-bot: combat + rest. +walk uses the normal queue so loot
    // and combat positioning run; follow/wander stay impossible (IsUseful).
    if (_ai->IsClientControlled() && !_ai->IsSelfWalkEnabled())
    {
        _queue.Clear();
        _queue.Push("combat", BotRelevance::Combat);
        if (!inCombat)
            _queue.Push("rest", BotRelevance::Rest);
    }

    uint32 iterations = 0;
    uint32 const maxIter = uint32(_queue.Size()) + 4;
    while (!_queue.Empty() && iterations++ < maxIter)
    {
        BotQueueItem item = _queue.Pop();
        BotAction* action = FindAction(item.name);
        if (!action)
            continue;

        action->SetRelevance(item.relevance);
        if (!action->IsUseful())
            continue;

        float relevance = item.relevance;
        for (auto& mult : _multipliers)
        {
            relevance *= mult->GetValue(action);
            action->SetRelevance(relevance);
            if (relevance <= 0.0f)
                break;
        }
        if (relevance <= 0.0f)
            continue;

        if (!action->IsPossible())
            continue;

        if (action->Execute())
            return true;
    }

    return false;
}
