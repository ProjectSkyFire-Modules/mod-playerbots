/*
 * Blood / Frost / Unholy Death Knight — MoP priorities
 * Unknown (low-level) spells are skipped via CanTryCast (which also checks runes).
 */

#include "BotRotationLists.h"
#include "Item.h"
#include "Pet.h"
#include "Player.h"
#include "Unit.h"

namespace BotRotation
{
namespace
{
    enum DeathKnightSpells : uint32
    {
        // Shared
        BLOOD_PRESENCE      = 48263,
        UNHOLY_PRESENCE     = 48265,
        FROST_PRESENCE      = 48266,
        HORN_OF_WINTER      = 57330,
        OUTBREAK            = 77575,
        ICY_TOUCH           = 45477,
        PLAGUE_STRIKE       = 45462,
        BLOOD_PLAGUE        = 55078,
        FROST_FEVER         = 55095,
        BLOOD_BOIL          = 48721,
        PESTILENCE          = 50842,
        DEATH_AND_DECAY     = 43265,
        DEATH_COIL          = 47541,
        DEATH_STRIKE        = 49998,
        RAISE_DEAD          = 46584,
        EMPOWER_RUNE_WEAPON = 47568,

        // Talents
        UNHOLY_BLIGHT       = 115989,
        BLOOD_TAP           = 45529,
        BLOOD_CHARGE        = 114851,

        // Unholy
        FESTERING_STRIKE    = 85948,
        SCOURGE_STRIKE      = 55090,
        DARK_TRANSFORMATION = 63560,
        SHADOW_INFUSION     = 91342,
        SUDDEN_DOOM         = 81340,
        UNHOLY_FRENZY       = 49016,
        SUMMON_GARGOYLE     = 49206,
        SOUL_REAPER_UNHOLY  = 130736,

        // Frost
        HOWLING_BLAST       = 49184,
        OBLITERATE          = 49020,
        FROST_STRIKE        = 49143,
        PILLAR_OF_FROST     = 51271,
        RIME                = 59052,
        KILLING_MACHINE     = 51124,
        SOUL_REAPER_FROST   = 130735,

        // Blood
        BONE_SHIELD         = 49222,
        HEART_STRIKE        = 55050,
        RUNE_STRIKE         = 56815,
        RUNE_TAP            = 48982,
        VAMPIRIC_BLOOD      = 55233,
        DANCING_RUNE_WEAPON = 49028,
        CRIMSON_SCOURGE     = 81141,
        SCENT_OF_BLOOD      = 50421,
        SOUL_REAPER_BLOOD   = 114866,
    };

    uint32 RunicPower(Player* bot)
    {
        return bot->GetPower(POWER_RUNIC_POWER) / 10;
    }

    uint32 ReadyRunes(Player* bot)
    {
        uint32 ready = 0;
        for (uint8 i = 0; i < MAX_RUNES; ++i)
            if (bot->GetRuneCooldown(i) == 0)
                ++ready;
        return ready;
    }

    bool DiseasesUp(Player* bot, Unit* target)
    {
        return HasMyAura(bot, target, BLOOD_PLAGUE) && HasMyAura(bot, target, FROST_FEVER);
    }

    // Shared opener: keep both diseases ticking. frostFeverSpell is Howling Blast for Frost.
    uint32 SelectDiseases(Player* bot, Unit* target, uint32 frostFeverSpell)
    {
        bool const plague = HasMyAura(bot, target, BLOOD_PLAGUE);
        bool const fever = HasMyAura(bot, target, FROST_FEVER);
        if (plague && fever)
            return 0;
        if (CanTryCast(bot, UNHOLY_BLIGHT))
            return UNHOLY_BLIGHT;
        if (CanTryCast(bot, OUTBREAK))
            return OUTBREAK;
        if (!fever && CanTryCast(bot, frostFeverSpell))
            return frostFeverSpell;
        if (!fever && frostFeverSpell != ICY_TOUCH && CanTryCast(bot, ICY_TOUCH))
            return ICY_TOUCH;
        if (!plague && CanTryCast(bot, PLAGUE_STRIKE))
            return PLAGUE_STRIKE;
        return 0;
    }

    // Pestilence only while a nearby enemy still lacks our diseases.
    uint32 SelectDiseaseSpread(Player* bot, Unit* target)
    {
        if (!DiseasesUp(bot, target))
            return 0;
        Unit* other = FindSecondaryEnemy(bot, target, 10.0f);
        if (!other || HasMyAura(bot, other, BLOOD_PLAGUE))
            return 0;
        if (CanTryCast(bot, PESTILENCE))
            return PESTILENCE;
        return 0;
    }

    uint32 SelectRuneRecovery(Context const& ctx, uint32 chargesNeeded)
    {
        Player* bot = ctx.bot;
        if (AuraStacks(bot, BLOOD_CHARGE) >= chargesNeeded && ReadyRunes(bot) < MAX_RUNES
            && CanTryCast(bot, BLOOD_TAP))
            return BLOOD_TAP;
        if (ReadyRunes(bot) == 0 && RunicPower(bot) <= 60 && WantsCooldowns(ctx)
            && CanTryCast(bot, EMPOWER_RUNE_WEAPON))
            return EMPOWER_RUNE_WEAPON;
        return 0;
    }
}

uint32 SelectUnholy(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    uint32 const runic = RunicPower(bot);
    Pet* ghoul = bot->GetPet();

    if (!HasAuraUp(bot, UNHOLY_PRESENCE) && CanTryCast(bot, UNHOLY_PRESENCE))
        return UNHOLY_PRESENCE;
    if (!HasAuraUp(bot, HORN_OF_WINTER) && CanTryCast(bot, HORN_OF_WINTER))
        return HORN_OF_WINTER;
    if (!ghoul && CanTryCast(bot, RAISE_DEAD))
        return RAISE_DEAD;

    if (uint32 const disease = SelectDiseases(bot, target, ICY_TOUCH))
        return disease;
    bool const diseases = DiseasesUp(bot, target);

    if (diseases && WantsCooldowns(ctx))
    {
        if (CanTryCast(bot, UNHOLY_FRENZY))
            return UNHOLY_FRENZY;
        if (CanTryCast(bot, SUMMON_GARGOYLE))
            return SUMMON_GARGOYLE;
    }

    if (ghoul && AuraStacks(ghoul, SHADOW_INFUSION) >= 5 && CanTryCast(bot, DARK_TRANSFORMATION))
        return DARK_TRANSFORMATION;

    if (ctx.targetHealthPct <= 35.0f && CanTryCast(bot, SOUL_REAPER_UNHOLY))
        return SOUL_REAPER_UNHOLY;

    if (uint32 const recover = SelectRuneRecovery(ctx, 10))
        return recover;

    bool const suddenDoom = HasAuraUp(bot, SUDDEN_DOOM);
    if (ctx.enemies >= 3)
    {
        if (uint32 const spread = SelectDiseaseSpread(bot, target))
            return spread;
        if (CanTryCast(bot, DEATH_AND_DECAY))
            return DEATH_AND_DECAY;
        if (diseases && CanTryCast(bot, BLOOD_BOIL))
            return BLOOD_BOIL;
    }

    if ((runic >= 90 || suddenDoom) && CanTryCast(bot, DEATH_COIL))
        return DEATH_COIL;
    if (CanTryCast(bot, SCOURGE_STRIKE))
        return SCOURGE_STRIKE;
    if (CanTryCast(bot, FESTERING_STRIKE))
        return FESTERING_STRIKE;
    // Scourge Strike is level 58: Plague Strike spends the Unholy runes until then.
    if (!bot->HasSpell(SCOURGE_STRIKE) && CanTryCast(bot, PLAGUE_STRIKE))
        return PLAGUE_STRIKE;

    // Also feeds Shadow Infusion toward the next Dark Transformation.
    if (CanTryCast(bot, DEATH_COIL))
        return DEATH_COIL;

    if (CanTryCast(bot, HORN_OF_WINTER))
        return HORN_OF_WINTER;

    return 0;
}

uint32 SelectFrostDK(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    uint32 const runic = RunicPower(bot);

    if (!HasAuraUp(bot, FROST_PRESENCE) && CanTryCast(bot, FROST_PRESENCE))
        return FROST_PRESENCE;
    if (!HasAuraUp(bot, HORN_OF_WINTER) && CanTryCast(bot, HORN_OF_WINTER))
        return HORN_OF_WINTER;

    if (CanTryCast(bot, PILLAR_OF_FROST))
        return PILLAR_OF_FROST;
    if (WantsCooldowns(ctx) && !bot->GetPet() && CanTryCast(bot, RAISE_DEAD))
        return RAISE_DEAD;

    if (uint32 const disease = SelectDiseases(bot, target, HOWLING_BLAST))
        return disease;

    if (ctx.targetHealthPct <= 35.0f && CanTryCast(bot, SOUL_REAPER_FROST))
        return SOUL_REAPER_FROST;

    if (uint32 const recover = SelectRuneRecovery(ctx, 10))
        return recover;

    bool const killingMachine = HasAuraUp(bot, KILLING_MACHINE);
    bool const rime = HasAuraUp(bot, RIME);

    if (ctx.enemies >= 3)
    {
        if (uint32 const spread = SelectDiseaseSpread(bot, target))
            return spread;
        if (CanTryCast(bot, DEATH_AND_DECAY))
            return DEATH_AND_DECAY;
        if (CanTryCast(bot, HOWLING_BLAST))
            return HOWLING_BLAST;
        if ((runic >= 90 || killingMachine) && CanTryCast(bot, FROST_STRIKE))
            return FROST_STRIKE;
        if (CanTryCast(bot, OBLITERATE))
            return OBLITERATE;
        if (CanTryCast(bot, FROST_STRIKE))
            return FROST_STRIKE;
        return CanTryCast(bot, HORN_OF_WINTER) ? HORN_OF_WINTER : 0;
    }

    // Dual wield plays Howling Blast heavy; two-handers lean on Obliterate.
    Item* offhand = bot->GetWeaponForAttack(WeaponAttackType::OFF_ATTACK, true);
    if (offhand)
    {
        if ((runic >= 90 || killingMachine) && CanTryCast(bot, FROST_STRIKE))
            return FROST_STRIKE;
        if (rime && CanTryCast(bot, HOWLING_BLAST))
            return HOWLING_BLAST;
        if (runic <= 76 && CanTryCast(bot, OBLITERATE))
            return OBLITERATE;
        if (CanTryCast(bot, HOWLING_BLAST))
            return HOWLING_BLAST;
        if (runic >= 60 && CanTryCast(bot, FROST_STRIKE))
            return FROST_STRIKE;
    }
    else
    {
        if (killingMachine && CanTryCast(bot, OBLITERATE))
            return OBLITERATE;
        if (runic >= 76 && CanTryCast(bot, FROST_STRIKE))
            return FROST_STRIKE;
        if (rime && CanTryCast(bot, HOWLING_BLAST))
            return HOWLING_BLAST;
        if (CanTryCast(bot, OBLITERATE))
            return OBLITERATE;
        if (CanTryCast(bot, HOWLING_BLAST))
            return HOWLING_BLAST;
    }

    // Obliterate is level 58: Plague Strike spends the Unholy runes until then.
    if (!bot->HasSpell(OBLITERATE) && CanTryCast(bot, PLAGUE_STRIKE))
        return PLAGUE_STRIKE;
    if (CanTryCast(bot, FROST_STRIKE))
        return FROST_STRIKE;
    if (CanTryCast(bot, HORN_OF_WINTER))
        return HORN_OF_WINTER;

    return 0;
}

uint32 SelectBlood(Context const& ctx)
{
    Player* bot = ctx.bot;
    Unit* target = ctx.target;
    if (!bot || !target)
        return 0;

    uint32 const runic = RunicPower(bot);
    float const hpPct = bot->GetMaxHealth()
        ? (100.0f * float(bot->GetHealth()) / float(bot->GetMaxHealth())) : 100.0f;

    if (!HasAuraUp(bot, BLOOD_PRESENCE) && CanTryCast(bot, BLOOD_PRESENCE))
        return BLOOD_PRESENCE;
    if (!HasAuraUp(bot, HORN_OF_WINTER) && CanTryCast(bot, HORN_OF_WINTER))
        return HORN_OF_WINTER;
    if (!HasAuraUp(bot, BONE_SHIELD) && CanTryCast(bot, BONE_SHIELD))
        return BONE_SHIELD;

    if (hpPct < 50.0f && CanTryCast(bot, VAMPIRIC_BLOOD))
        return VAMPIRIC_BLOOD;
    if (hpPct < 40.0f && CanTryCast(bot, RUNE_TAP))
        return RUNE_TAP;
    if (hpPct < 70.0f && CanTryCast(bot, DEATH_STRIKE))
        return DEATH_STRIKE;
    if (WantsCooldowns(ctx) && CanTryCast(bot, DANCING_RUNE_WEAPON))
        return DANCING_RUNE_WEAPON;

    if (uint32 const disease = SelectDiseases(bot, target, ICY_TOUCH))
        return disease;
    bool const diseases = DiseasesUp(bot, target);

    if (ctx.targetHealthPct < 35.0f && CanTryCast(bot, SOUL_REAPER_BLOOD))
        return SOUL_REAPER_BLOOD;

    // Crimson Scourge makes the next Blood Boil / Death and Decay free.
    if (HasAuraUp(bot, CRIMSON_SCOURGE))
    {
        if (CanTryCast(bot, BLOOD_BOIL))
            return BLOOD_BOIL;
        if (CanTryCast(bot, DEATH_AND_DECAY))
            return DEATH_AND_DECAY;
    }

    if ((hpPct < 90.0f || AuraStacks(bot, SCENT_OF_BLOOD) >= 3) && CanTryCast(bot, DEATH_STRIKE))
        return DEATH_STRIKE;

    if (uint32 const recover = SelectRuneRecovery(ctx, 10))
        return recover;

    if (ctx.enemies >= 3)
    {
        if (CanTryCast(bot, DEATH_AND_DECAY))
            return DEATH_AND_DECAY;
        if (diseases && CanTryCast(bot, BLOOD_BOIL))
            return BLOOD_BOIL;
    }

    if (CanTryCast(bot, HEART_STRIKE))
        return HEART_STRIKE;
    if (CanTryCast(bot, DEATH_STRIKE))
        return DEATH_STRIKE;
    // Heart Strike is level 60 and Death Strike 56: spend the runes on what is known.
    if (!bot->HasSpell(HEART_STRIKE))
    {
        if (diseases && CanTryCast(bot, BLOOD_BOIL))
            return BLOOD_BOIL;
        if (!bot->HasSpell(DEATH_STRIKE) && CanTryCast(bot, PLAGUE_STRIKE))
            return PLAGUE_STRIKE;
    }

    if (runic >= 60)
    {
        if (CanTryCast(bot, RUNE_STRIKE))
            return RUNE_STRIKE;
        if (CanTryCast(bot, DEATH_COIL))
            return DEATH_COIL;
    }

    if (CanTryCast(bot, HORN_OF_WINTER))
        return HORN_OF_WINTER;

    return 0;
}

} // namespace BotRotation
