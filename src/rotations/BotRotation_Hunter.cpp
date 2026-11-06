// Beast Mastery / Marksmanship / Survival — modules/mod-playerbots/rotation.md
// Unknown (low-level) spells are skipped via CanTryCast.

#include "BotRotationLists.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Unit.h"

namespace BotRotation
{
namespace
{
    enum HunterSpells : uint32
    {
        HUNTERS_MARK      = 1130,
        SERPENT_STING     = 1978,
        ARCANE_SHOT       = 3044,
        COBRA_SHOT        = 77767,
        STEADY_SHOT       = 56641,
        MULTI_SHOT        = 2643,
        KILL_SHOT         = 53351,
        RAPID_FIRE        = 3045,
        STAMPEDE          = 121818,
        THRILL_OF_THE_HUNT = 34720,

        // Talents
        FERVOR            = 82726,
        DIRE_BEAST        = 120679,
        A_MURDER_OF_CROWS = 131894,
        LYNX_RUSH         = 120697,
        GLAIVE_TOSS       = 117050,
        POWERSHOT         = 109259,
        BARRAGE           = 120360,

        // Beast Mastery
        KILL_COMMAND      = 34026,
        BESTIAL_WRATH     = 19574,
        FOCUS_FIRE        = 82692,
        FRENZY            = 19615,
        BEAST_CLEAVE      = 118455,  // pet buff from Multi-Shot

        // Marksmanship
        CHIMERA_SHOT      = 53209,
        AIMED_SHOT        = 19434,
        MASTER_MARKSMAN   = 82926,  // "Fire!": next Aimed Shot is instant and free
        STEADY_FOCUS      = 53220,
        CAREFUL_AIM       = 34483,  // level 20: Aimed / Steady crit above 80% health

        // Survival
        BLACK_ARROW       = 3674,
        EXPLOSIVE_SHOT    = 53301,
        LOCK_AND_LOAD     = 56453,
    };

    // Any hunter's mark gives the damage bonus. A mark cast moments ago counts as up even
    // when the aura is not visible on the target, so it is not recast every GCD.
    bool NeedsMark(Player* bot, Unit* target)
    {
        if (HasAuraUp(target, HUNTERS_MARK))
            return false;
        PlayerbotAI* ai = sPlayerbotMgr->GetBotAI(bot);
        return !ai || !ai->CastOnRecently(HUNTERS_MARK, target->GetGUID(), 30 * IN_MILLISECONDS);
    }

    bool NeedsSerpent(Player* bot, Unit* target)
    {
        return NeedsMyAuraRefresh(bot, target, SERPENT_STING, 3.0f);
    }

    uint32 Focus(Player* bot)
    {
        return bot->GetPower(POWER_FOCUS);
    }

    // Talent shots every spec fires on cooldown, and the shared burst buttons.
    uint32 SelectHunterCooldowns(Context const& ctx)
    {
        Player* bot = ctx.bot;
        uint32 const focus = Focus(bot);
        bool const burst = WantsCooldowns(ctx);

        if (burst && CanTryCast(bot, RAPID_FIRE) && !HasAuraUp(bot, RAPID_FIRE))
            return RAPID_FIRE;
        if (burst && CanTryCast(bot, STAMPEDE))
            return STAMPEDE;
        if (burst && focus >= 60 && CanTryCast(bot, A_MURDER_OF_CROWS))
            return A_MURDER_OF_CROWS;
        if (focus <= 50 && CanTryCast(bot, FERVOR))
            return FERVOR;
        if (CanTryCast(bot, DIRE_BEAST))
            return DIRE_BEAST;
        if (bot->GetPet() && CanTryCast(bot, LYNX_RUSH))
            return LYNX_RUSH;
        if (focus >= 15 && CanTryCast(bot, GLAIVE_TOSS))
            return GLAIVE_TOSS;
        if (focus >= 45 && CanTryCast(bot, POWERSHOT))
            return POWERSHOT;
        if (focus >= 40 && CanTryCast(bot, BARRAGE))
            return BARRAGE;
        return 0;
    }
}

uint32 SelectBeastMastery(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    uint32 const focus = Focus(bot);
    Pet* pet = bot->GetPet();
    bool const wrath = HasAuraUp(bot, BESTIAL_WRATH);

    if (NeedsMark(bot, target) && CanTryCast(bot, HUNTERS_MARK))
        return HUNTERS_MARK;

    if (pet && focus >= 50 && CanTryCast(bot, BESTIAL_WRATH))
        return BESTIAL_WRATH;
    if (uint32 const cd = SelectHunterCooldowns(ctx))
        return cd;
    // Focus Fire eats the pet's Frenzy stacks; never during Bestial Wrath.
    if (pet && !wrath && AuraStacks(pet, FRENZY) >= 5 && CanTryCast(bot, FOCUS_FIRE))
        return FOCUS_FIRE;

    if (ctx.targetHealthPct <= 20.0f && CanTryCast(bot, KILL_SHOT))
        return KILL_SHOT;

    if (ctx.enemies >= 3 && CanTryCast(bot, MULTI_SHOT))
        return MULTI_SHOT;
    if (pet && CanTryCast(bot, KILL_COMMAND))
        return KILL_COMMAND;
    if (ctx.enemies >= 2 && pet && AuraRemains(pet, BEAST_CLEAVE) < 1.5f && focus >= 40
        && CanTryCast(bot, MULTI_SHOT))
        return MULTI_SHOT;

    if (NeedsSerpent(bot, target) && CanTryCast(bot, SERPENT_STING))
        return SERPENT_STING;

    if ((HasAuraUp(bot, THRILL_OF_THE_HUNT) || (wrath && focus >= 35) || focus >= 79)
        && CanTryCast(bot, ARCANE_SHOT))
        return ARCANE_SHOT;

    if (CanTryCast(bot, COBRA_SHOT))
        return COBRA_SHOT;
    if (CanTryCast(bot, STEADY_SHOT))
        return STEADY_SHOT;

    return 0;
}

uint32 SelectMarksmanship(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    uint32 const focus = Focus(bot);

    if (NeedsMark(bot, target) && CanTryCast(bot, HUNTERS_MARK))
        return HUNTERS_MARK;

    if (uint32 const cd = SelectHunterCooldowns(ctx))
        return cd;

    // Steady Focus comes from two Steady Shots in a row; keep it rolling.
    if (bot->HasSpell(CHIMERA_SHOT) && AuraRemains(bot, STEADY_FOCUS) <= 4.0f
        && !HasAuraUp(bot, MASTER_MARKSMAN) && CanTryCast(bot, STEADY_SHOT))
        return STEADY_SHOT;

    if (CanTryCast(bot, CHIMERA_SHOT))
        return CHIMERA_SHOT;
    if (NeedsSerpent(bot, target) && CanTryCast(bot, SERPENT_STING))
        return SERPENT_STING;
    if (HasAuraUp(bot, MASTER_MARKSMAN) && CanTryCast(bot, AIMED_SHOT))
        return AIMED_SHOT;
    // Hard-cast Aimed Shot inside Careful Aim or Rapid Fire.
    bool const carefulAim = ctx.targetHealthPct > 80.0f && bot->HasSpell(CAREFUL_AIM);
    if (focus >= 50 && (carefulAim || HasAuraUp(bot, RAPID_FIRE)) && CanTryCast(bot, AIMED_SHOT))
        return AIMED_SHOT;
    if (ctx.enemies >= 4 && CanTryCast(bot, MULTI_SHOT))
        return MULTI_SHOT;
    if (ctx.targetHealthPct <= 20.0f && CanTryCast(bot, KILL_SHOT))
        return KILL_SHOT;
    // Before Chimera Shot (60) Aimed Shot is the focus dump, not Arcane Shot.
    if (!bot->HasSpell(CHIMERA_SHOT) && focus >= 60 && CanTryCast(bot, AIMED_SHOT))
        return AIMED_SHOT;
    if ((HasAuraUp(bot, THRILL_OF_THE_HUNT) || focus >= 60) && CanTryCast(bot, ARCANE_SHOT))
        return ARCANE_SHOT;
    if (CanTryCast(bot, STEADY_SHOT))
        return STEADY_SHOT;

    return 0;
}

uint32 SelectSurvival(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    uint32 const focus = Focus(bot);
    bool const lockAndLoad = HasAuraUp(bot, LOCK_AND_LOAD);

    if (NeedsMark(bot, target) && CanTryCast(bot, HUNTERS_MARK))
        return HUNTERS_MARK;

    if (uint32 const cd = SelectHunterCooldowns(ctx))
        return cd;

    // Lock and Load: free Explosive Shots, cooldown reset.
    if (lockAndLoad && CanTryCast(bot, EXPLOSIVE_SHOT))
        return EXPLOSIVE_SHOT;

    if (ctx.enemies > 3 && focus >= 50 && CanTryCast(bot, MULTI_SHOT))
        return MULTI_SHOT;

    if (NeedsSerpent(bot, target) && CanTryCast(bot, SERPENT_STING))
        return SERPENT_STING;
    if (CanTryCast(bot, EXPLOSIVE_SHOT))
        return EXPLOSIVE_SHOT;
    if (ctx.targetHealthPct <= 20.0f && CanTryCast(bot, KILL_SHOT))
        return KILL_SHOT;
    if (!HasMyAura(bot, target, BLACK_ARROW) && focus >= 35 && CanTryCast(bot, BLACK_ARROW))
        return BLACK_ARROW;
    if (ctx.enemies >= 2 && focus >= 50 && CanTryCast(bot, MULTI_SHOT))
        return MULTI_SHOT;
    if ((HasAuraUp(bot, THRILL_OF_THE_HUNT) || focus >= 70) && CanTryCast(bot, ARCANE_SHOT))
        return ARCANE_SHOT;
    if (CanTryCast(bot, COBRA_SHOT))
        return COBRA_SHOT;
    if (CanTryCast(bot, STEADY_SHOT))
        return STEADY_SHOT;

    return 0;
}

} // namespace BotRotation
