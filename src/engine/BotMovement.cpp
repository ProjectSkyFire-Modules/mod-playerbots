/*
 * Cast-safe movement helpers.
 */

#include "BotMovement.h"

#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include "CellImpl.h"
#include "GameObject.h"
#include "GameObjectModel.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "Object.h"
#include "PathGenerator.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

#include <algorithm>
#include <cmath>
#include <list>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace BotMovement
{
bool IsCasting(Player* bot)
{
    // Skip Auto Shot / wand autorepeat — those are background fire, not a cast.
    // Include pending instants (skipInstant=false). Do not treat bare CASTING
    // state alone — that stuck true and blocked facing / move-flag cleanup.
    return bot && bot->IsNonMeleeSpellCasted(false, false, true, false, false);
}

    void ClaimSelfWalk(Player* bot)
    {
        if (!bot)
            return;
        if (PlayerbotAI* ai = sPlayerbotMgr->GetBotAI(bot))
            ai->ClaimSelfMovement();
    }

    bool CanMove(Player* bot)
    {
        if (!bot || !bot->IsAlive() || IsCasting(bot))
            return false;
        // Moving while seated cancels drink/food auras — rest must finish first.
        if (bot->IsSitState())
            return false;
        return true;
    }

    // Point/follow Launch() copies walkmode from MOVEMENTFLAG_WALKING. A stuck
    // walk flag makes every MovePoint crawl; strip it before issuing motion.
    void EnsureRunning(Player* bot)
    {
        if (!bot)
            return;
        if (bot->IsWalking())
            bot->SetWalk(false);
    }

    bool IsAirborne(Unit const* unit)
    {
        if (!unit)
            return false;
        if (unit->IsFlying())
            return true;
        if (unit->HasUnitMovementFlag(MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_FLYING))
            return true;
        if (unit->HasAuraType(SPELL_AURA_FLY)
            || unit->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED))
            return true;
        return false;
    }

    bool StopAndIdle(Player* bot)
    {
        if (!CanMove(bot))
            return false;

        // Stop spline / clear UNIT_STATE_MOVING while still animating.
        if (!bot->IsStopped() || (bot->movespline && !bot->movespline->Finalized()))
            bot->StopMoving();

        // Spell::prepare rejects cast-time spells when isMoving() — that checks
        // MOVEMENTFLAG_MASK_MOVING, not UNIT_STATE_MOVING. Point/facing Launch()
        // sets MOVEMENTFLAG_FORWARD; once the spline finalizes, StopMoving
        // early-outs and leaves the flag stuck. Casters then only land instants
        // (DoTs) and look like a broken rotation.
        bot->RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
        EnsureRunning(bot);

        if (bot->GetMotionMaster()->GetCurrentMovementGeneratorType() != IDLE_MOTION_TYPE)
        {
            bot->GetMotionMaster()->Clear();
            bot->GetMotionMaster()->MoveIdle();
        }
        return true;
    }

    bool ClearMotion(Player* bot)
    {
        if (!CanMove(bot))
            return false;
        bot->GetMotionMaster()->Clear();
        return true;
    }

    bool ComputeFollowPoint(Unit* leader, Player* bot, float distance, float angle,
        float& x, float& y, float& z, bool airborne)
    {
        if (!leader || !bot)
            return false;

        // Same convention as FollowMovementGenerator / GetClosePoint:
        // absolute angle = leader orientation + relative angle.
        float const size = bot->GetObjectSize();
        leader->GetClosePoint(x, y, z, size, distance, angle);

        if (airborne)
        {
            // Stay at the leader's altitude — never snap the slot to ground mesh.
            z = leader->GetPositionZ();
        }
        else
        {
            leader->UpdateAllowedPositionZ(x, y, z);
            bot->UpdateAllowedPositionZ(x, y, z);
        }
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }

    bool IsBadlyOffGround(Player* bot, float maxDelta)
    {
        if (!bot || !bot->IsInWorld())
            return false;
        if (IsAirborne(bot))
            return false;

        float x = bot->GetPositionX();
        float y = bot->GetPositionY();
        float z = bot->GetPositionZ();
        float groundZ = z;
        bot->UpdateAllowedPositionZ(x, y, groundZ);
        return std::fabs(z - groundZ) > maxDelta;
    }

    bool MoveToFollowSlot(Player* bot, Unit* leader, float distance, float angle, bool airborne)
    {
        if (!CanMove(bot) || !leader)
            return false;

        float x, y, z;
        if (!ComputeFollowPoint(leader, bot, distance, angle, x, y, z, airborne))
            return false;

        // Air: no navmesh path (that clamps Z to ground and "sucks" flyers down).
        return MovePoint(bot, x, y, z, !airborne);
    }

    bool PathCrossesClosedDoor(Player* bot, float x, float y, float z)
    {
        if (!bot || !bot->IsInWorld())
            return false;

        float const reach = std::min(bot->GetExactDist(x, y, z) + 10.0f, 100.0f);
        std::list<GameObject*> doors;
        Skyfire::GameObjectInRangeCheck check(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), reach);
        Skyfire::GameObjectListSearcher<Skyfire::GameObjectInRangeCheck> searcher(bot, doors, check);
        bot->VisitNearbyGridObject(reach, searcher);
        doors.remove_if([](GameObject* go)
        {
            return go->GetGoType() != GAMEOBJECT_TYPE_DOOR || go->GetGoState() != GOState::GO_STATE_READY
                || !go->m_model || !go->m_model->isEnabled();
        });
        if (doors.empty())
            return false;

        PathGenerator path(bot);
        path.CalculatePath(x, y, z);
        Movement::PointsArray const& points = path.GetPath();
        G3D::Vector3 const lift(0.0f, 0.0f, 1.5f);
        for (size_t i = 1; i < points.size(); ++i)
        {
            G3D::Vector3 const from = points[i - 1] + lift;
            G3D::Vector3 const to = points[i] + lift;
            float const length = (to - from).magnitude();
            if (length < 0.01f)
                continue;

            G3D::Ray const ray = G3D::Ray::fromOriginAndDirection(from, (to - from) / length);
            for (GameObject* door : doors)
            {
                float dist = length;
                if (door->m_model->intersectRay(ray, dist, true, bot->GetPhaseMask()))
                    return true;
            }
        }
        return false;
    }

    float PulsedDamageRadius(Unit* unit, uint32* spellId)
    {
        float radius = 0.0f;
        if (!unit)
            return radius;

        Unit::AuraEffectList const* lists[2] =
        {
            &unit->GetAuraEffectsByType(SPELL_AURA_PERIODIC_TRIGGER_SPELL),
            &unit->GetAuraEffectsByType(SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE)
        };
        for (Unit::AuraEffectList const* list : lists)
        {
            for (AuraEffect const* aurEff : *list)
            {
                if (!aurEff)
                    continue;
                SpellInfo const* info = aurEff->GetSpellInfo();
                if (!info)
                    continue;
                // Not IsPositive(): entry-targeted damage (TARGET_UNIT_DEST_AREA_ENTRY) is classed as positive.
                // A hostile creature pulsing school damage is enough.
                SpellInfo const* trigger = sSpellMgr->GetSpellInfo(info->Effects[aurEff->GetEffIndex()].TriggerSpell);
                if (!trigger)
                    continue;
                for (uint8 j = 0; j < MAX_SPELL_EFFECTS; ++j)
                {
                    if (trigger->Effects[j].Effect != SPELL_EFFECT_SCHOOL_DAMAGE)
                        continue;
                    float const r = trigger->Effects[j].CalcRadius(unit);
                    if (r > radius)
                    {
                        radius = r;
                        if (spellId)
                            *spellId = trigger->Id;
                    }
                }
            }
        }
        return radius;
    }

    void CollectSweepHazards(Player* bot, float range, std::vector<SweepHazard>& hazards)
    {
        hazards.clear();
        if (!bot || !bot->IsInWorld())
            return;

        std::list<Unit*> units;
        Skyfire::AnyUnitInObjectRangeCheck check(bot, range);
        Skyfire::UnitListSearcher<Skyfire::AnyUnitInObjectRangeCheck> searcher(bot, units, check);
        bot->VisitNearbyObject(range, searcher);

        for (Unit* unit : units)
        {
            if (!unit || unit->GetTypeId() == TypeID::TYPEID_PLAYER || !unit->IsAlive())
                continue;
            if (!unit->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE) || bot->IsFriendlyTo(unit))
                continue;
            Movement::MoveSpline const* spline = unit->movespline.get();
            if (!spline || !spline->Initialized() || spline->Finalized() || spline->onTransport)
                continue;

            uint32 spellId = 0;
            float const radius = PulsedDamageRadius(unit, &spellId);
            if (radius < 0.5f || radius > 15.0f)
                continue;

            // The unit's own position only follows its spline every few hundred ms, yards behind on a fast sweep
            Movement::Location const loc = spline->ComputePosition();
            if (std::fabs(bot->GetPositionZ() - loc.z) > 8.0f)
                continue;

            float speed = 0.0f;
            Movement::MoveSpline::MySpline const& path = spline->_Spline();
            int32 const idx = spline->_currentSplineIdx();
            if (idx >= path.first() && idx < path.last())
            {
                int32 const segmentMs = path.length(idx + 1) - path.length(idx);
                if (segmentMs > 0)
                    speed = (path.getPoint(idx + 1) - path.getPoint(idx)).length() * 1000.0f / float(segmentMs);
            }
            if (speed < 0.1f)
                continue;

            hazards.push_back({ loc.x, loc.y, loc.z, radius, std::cos(loc.orientation), std::sin(loc.orientation), speed, spellId });
        }
    }

    float SweepExposure(Player* bot, std::vector<SweepHazard> const& hazards, float x, float y, float holdTime,
        float* minClear)
    {
        float constexpr kStep = 0.1f;
        float constexpr kMargin = 0.5f;
        // Straight-line extrapolation drifts off a turning sweep; don't trust it further out
        float constexpr kMaxHorizon = 3.0f;

        float const startX = bot->GetPositionX();
        float const startY = bot->GetPositionY();
        float const runSpeed = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
        float const moveTime = std::sqrt((x - startX) * (x - startX) + (y - startY) * (y - startY)) / runSpeed;
        float const horizon = std::min(moveTime + holdTime, kMaxHorizon);

        float exposure = 0.0f;
        float clear = 100.0f;
        for (float t = 0.0f; t <= horizon; t += kStep)
        {
            float const f = moveTime > 0.0f ? std::min(t / moveTime, 1.0f) : 1.0f;
            float const px = startX + (x - startX) * f;
            float const py = startY + (y - startY) * f;
            bool hit = false;
            for (SweepHazard const& h : hazards)
            {
                float const hx = h.x + h.dirX * h.speed * t;
                float const hy = h.y + h.dirY * h.speed * t;
                float const gap = std::sqrt((px - hx) * (px - hx) + (py - hy) * (py - hy)) - h.radius;
                clear = std::min(clear, gap);
                if (gap < kMargin)
                    hit = true;
            }
            if (hit)
                exposure += kStep;
        }
        if (minClear)
            *minClear = clear;
        return exposure;
    }

    bool PathCrossesSweep(Player* bot, float x, float y)
    {
        if (!bot || !bot->IsInWorld() || !bot->IsInCombat())
            return false;

        std::vector<SweepHazard> hazards;
        CollectSweepHazards(bot, 40.0f, hazards);
        if (hazards.empty())
            return false;

        float constexpr kHold = 1.0f;
        float const go = SweepExposure(bot, hazards, x, y, kHold);
        if (go <= 0.0f)
            return false;
        float const runSpeed = std::max(bot->GetSpeed(MOVE_RUN), 1.0f);
        float const moveTime = bot->GetExactDist2d(x, y) / runSpeed;
        float const stay = SweepExposure(bot, hazards, bot->GetPositionX(), bot->GetPositionY(), moveTime + kHold);
        return go > stay;
    }

    bool MoveFollowLeader(Player* bot, Unit* leader, float distance, float angle)
    {
        if (!CanMove(bot) || !leader)
            return false;
        if (PathCrossesClosedDoor(bot, leader->GetPositionX(), leader->GetPositionY(), leader->GetPositionZ()))
            return false;
        if (PathCrossesSweep(bot, leader->GetPositionX(), leader->GetPositionY()))
            return false;
        EnsureRunning(bot);
        ClaimSelfWalk(bot);
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MoveFollow(leader, distance, angle);
        return true;
    }

    bool MoveChase(Player* bot, Unit* target, float distance)
    {
        if (!CanMove(bot) || !target || !target->IsAlive())
            return false;
        if (PathCrossesClosedDoor(bot, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ()))
            return false;
        if (PathCrossesSweep(bot, target->GetPositionX(), target->GetPositionY()))
            return false;
        EnsureRunning(bot);
        ClaimSelfWalk(bot);
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MoveChase(target, distance);
        return true;
    }

    bool MovePoint(Player* bot, float x, float y, float z, bool generatePath, bool checkSweep)
    {
        if (!CanMove(bot))
            return false;
        if (generatePath && PathCrossesClosedDoor(bot, x, y, z))
            return false;
        if (checkSweep && PathCrossesSweep(bot, x, y))
            return false;
        EnsureRunning(bot);
        ClaimSelfWalk(bot);
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(0, x, y, z, generatePath);
        return true;
    }

    bool MoveTo(Player* bot, float x, float y, float z)
    {
        if (!CanMove(bot))
            return false;
        EnsureRunning(bot);

        bot->UpdateAllowedPositionZ(x, y, z);
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            return false;

        PathGenerator path(bot);
        bool const ok = path.CalculatePath(x, y, z);
        PathType const type = path.GetPathType();
        if (!ok || (type & PATHFIND_NOPATH) || (type & PATHFIND_BLANK))
            return false;

        // Prefer the navmesh's actual end when the path is incomplete/short.
        G3D::Vector3 const& end = path.GetActualEndPosition();
        float destX = end.x;
        float destY = end.y;
        float destZ = end.z;
        bot->UpdateAllowedPositionZ(destX, destY, destZ);
        if (PathCrossesClosedDoor(bot, destX, destY, destZ))
            return false;
        if (PathCrossesSweep(bot, destX, destY))
            return false;

        ClaimSelfWalk(bot);
        bot->GetMotionMaster()->Clear();
        bot->GetMotionMaster()->MovePoint(1, destX, destY, destZ, true);
        return true;
    }

    void FaceOrientation(Player* bot, float orientation)
    {
        if (!bot || IsCasting(bot))
            return;
        // Orientation only — SetFacingTo launches a MoveSpline that sets
        // MOVEMENTFLAG_FORWARD and breaks cast-time spells.
        bot->SetOrientation(orientation);
    }

    void FaceUnit(Player* bot, Unit* target)
    {
        if (!bot || !target || IsCasting(bot))
            return;
        // SetInFront = orientation only. SetFacingToObject Launch() marks the
        // bot as moving and Spell::prepare then fails every cast-time spell.
        bot->SetInFront(target);
    }

    void ClearDeadSelection(Player* bot)
    {
        if (!bot)
            return;
        if (Unit* selected = bot->GetSelectedUnit())
            if (!selected->IsAlive())
                bot->SetSelection(0);
    }
}
