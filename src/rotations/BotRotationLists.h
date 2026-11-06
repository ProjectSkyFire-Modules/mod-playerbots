/*
 * Declarations for per-spec rotation selectors.
 */

#ifndef _SF_BOT_ROTATION_LISTS_H
#define _SF_BOT_ROTATION_LISTS_H

#include "BotRotation.h"

namespace BotRotation
{
    // Wave 1
    uint32 SelectRetribution(Context const& ctx);
    uint32 SelectWindwalker(Context const& ctx);
    uint32 SelectBeastMastery(Context const& ctx);
    uint32 SelectShadow(Context const& ctx);
    uint32 SelectAffliction(Context const& ctx);
    uint32 SelectElemental(Context const& ctx);

    // Wave 2
    uint32 SelectEnhancement(Context const& ctx);
    uint32 SelectFeral(Context const& ctx);
    uint32 SelectMarksmanship(Context const& ctx);
    uint32 SelectSurvival(Context const& ctx);
    uint32 SelectArms(Context const& ctx);
    uint32 SelectFury(Context const& ctx);
    uint32 SelectCombat(Context const& ctx);
    uint32 SelectFrostMage(Context const& ctx);
    uint32 SelectDestruction(Context const& ctx);
    uint32 SelectDemonology(Context const& ctx);
    uint32 SelectUnholy(Context const& ctx);

    // Wave 3 - remaining DPS + tanks
    uint32 SelectBalance(Context const& ctx);
    uint32 SelectLowLevelDruid(Context const& ctx);
    uint32 SelectGuardian(Context const& ctx);
    uint32 SelectFire(Context const& ctx);
    uint32 SelectArcane(Context const& ctx);
    uint32 SelectAssassination(Context const& ctx);
    uint32 SelectSubtlety(Context const& ctx);
    uint32 SelectFrostDK(Context const& ctx);
    uint32 SelectBlood(Context const& ctx);
    uint32 SelectProtectionPaladin(Context const& ctx);
    uint32 SelectProtectionWarrior(Context const& ctx);
    uint32 SelectBrewmaster(Context const& ctx);

    // Wave 4 - healers (heal + healer-dps damage lines)
    uint32 SelectHolyPaladin(HealContext const& ctx);
    uint32 SelectDiscipline(HealContext const& ctx);
    uint32 SelectHolyPriest(HealContext const& ctx);
    uint32 SelectRestorationShaman(HealContext const& ctx);
    uint32 SelectRestorationDruid(HealContext const& ctx);
    uint32 SelectMistweaver(HealContext const& ctx);

    uint32 SelectHolyPaladinDps(Context const& ctx);
    uint32 SelectDisciplineDps(Context const& ctx);
    uint32 SelectHolyPriestDps(Context const& ctx);
    uint32 SelectRestorationShamanDps(Context const& ctx);
    uint32 SelectRestorationDruidDps(Context const& ctx);
    uint32 SelectMistweaverDps(Context const& ctx);
}

#endif // _SF_BOT_ROTATION_LISTS_H
