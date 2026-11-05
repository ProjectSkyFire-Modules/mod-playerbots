# Rotation source for mod-playerbots combat selectors.

Each combat Select* in `src/rotations/` follows these lists in order.
Unknown or too-low-level spells are skipped (`CanTryCast` / `HasSpell`) and the
next line is tried. DoTs and keep-up buffs refresh when remaining ≤ ~3s.

Spell and aura IDs: look up names in `dbc_export/Spell.csv`. Cast spell ID and
applied aura ID sometimes differ (e.g. Corruption 172 → 146739, Serpent Sting
1978 → 118253); those maps live in `AuraIdForSpell()` in BotRotation.cpp.

Healers are not covered here yet.

---

Druid

Guardian (Tank)

    Form: Bear Form

    Rotation: Mangle (on cooldown) > Thrash (apply bleed) > Lacerate (stack up to 3 times) > Swipe (filler) > Savage Defense (spend Rage for dodge mitigation)

Feral (DPS)

    Form: Cat Form

    Rotation: Savage Roar (keep buff active) > Rake (keep bleed active) > Mangle / Shred (build combo points) > Rip (finisher at 5 points) > Ferocious Bite (finisher if Rip is already active)

Balance (DPS)

    Form: Moonkin Form

    Rotation: Moonfire / Sunfire (keep active) > Starsurge (on cooldown) > Wrath or Starfire (spam depending on which way your Eclipse bar is moving)

Hunter

Beast Mastery (DPS)

    Rotation: Hunter's Mark > Serpent Sting (apply poison) > Kill Command (on cooldown) > Arcane Shot (dump Focus) > Cobra Shot (build Focus)

Marksmanship (DPS)

    Rotation: Hunter's Mark > Serpent Sting > Chimera Shot (on cooldown) > Aimed Shot (on proc or to dump Focus) > Steady Shot (build Focus)

Survival (DPS)

    Rotation: Hunter's Mark > Serpent Sting > Black Arrow (on cooldown) > Explosive Shot (on cooldown) > Arcane Shot (dump Focus) > Cobra Shot (build Focus)

Mage

Arcane (DPS)

    Buff: Mage Armor

    Rotation: Arcane Blast (spam to build up to 4 Arcane Charges) > Arcane Missiles (on proc) > Arcane Barrage (cast at 4 charges to reset mana cost)

Fire (DPS)

    Buff: Molten Armor

    Rotation: Living Bomb (keep active) > Inferno Blast (use when you get a "Heating Up" proc to force a guaranteed crit) > Pyroblast (use the instant cast proc) > Fireball (filler)

Frost (DPS)

    Buff: Frost Armor

    Rotation: armor (Frost, else Molten until Frost Armor is learned) > Water Elemental > Living Bomb or Nether Tempest (keep active) > Frozen Orb (on cooldown) > Ice Lance (on Fingers of Frost) > Frostfire Bolt (on Brain Freeze; the passive is level 77) > Blizzard, or Flamestrike when Blizzard is not known yet > Cone of Cold and Frost Nova when a mob is in melee > Fire Blast (on cooldown, in range) > Frostbolt (filler)

Monk

Brewmaster (Tank)

    Stance: Stance of the Sturdy Ox

    Rotation: Fortifying Brew (below 55% health) > Purifying Brew (Heavy Stagger, or Moderate Stagger with Shuffle up) > Elusive Brew (10+ stacks, or 5+ below 70% health) > Blackout Kick (Shuffle about to drop) > Guard > Keg Smash (if it won't cap Chi) > Breath of Fire (3+ targets with Dizzying Haze, Shuffle 6s+) / Rushing Jade Wind (3+ targets) > Chi Wave > Blackout Kick (near Chi cap, or Shuffle under 6s) > Chi Brew (0-1 Chi) > Tiger Palm (keep Tiger Power active) > Spinning Crane Kick (4+ targets without Rushing Jade Wind) / Expel Harm (below 90% health) / Jab (build Chi, pooling energy for Keg Smash) > Tiger Palm (free filler)

    Pull: Chi Wave if talented, otherwise Provoke

Windwalker (DPS)

    Stance: Stance of the Fierce Tiger

    Rotation: Rising Sun Kick (on cooldown) > Tiger Palm (keep Tiger Power active) > Fists of Fury (on cooldown) > Blackout Kick (dump Chi) > Jab (build Chi)

Paladin

Protection (Tank)

    Buffs: Righteous Fury, Seal of Insight

    Rotation: Crusader Strike (or Hammer of the Righteous for AoE) > Judgment > Avenger's Shield > Consecration > Shield of the Righteous (dump Holy Power for physical mitigation)

Retribution (DPS)

    Buffs: Seal of Truth

    Rotation: Inquisition (spend Holy Power to keep buff active) > Crusader Strike > Judgment > Exorcism > Templar's Verdict (dump Holy Power for damage)

Priest

Shadow (DPS)

    Stance: Shadowform

    Buff: Inner Fire

    Rotation: Vampiric Touch (keep active) > Shadow Word: Pain (keep active) > Mind Blast (on cooldown to build Shadow Orbs) > Devouring Plague (cast at 3 Orbs) > Mind Flay (filler)

Rogue

Assassination (DPS)

    Buffs: Deadly Poison (lethal) + Leeching Poison (non-lethal; Crippling if Leeching unknown). MoP allows only one of each.

    Rotation: Mutilate (build combo points) > Rupture (keep bleed active) > Envenom (finisher at 4-5 points) > Dispatch (use when target is below 30% health or on proc)

Combat (DPS)

    Buffs: Deadly Poison (lethal) + Crippling Poison (non-lethal). MoP allows only one of each.

    Rotation: Revealing Strike (keep debuff active) > Sinister Strike (build combo points) > Slice and Dice (keep attack speed buff active) > Eviscerate (finisher at 5 points)

Subtlety (DPS)

    Buffs: Deadly Poison (lethal) + Crippling Poison (non-lethal). MoP allows only one of each.

    Rotation: Hemorrhage (keep bleed active) > Backstab (build combo points) > Slice and Dice (keep buff active) > Rupture (finisher) > Eviscerate (finisher)

Shaman

Elemental (DPS)

    Buff: Lightning Shield

    Rotation: Flame Shock (keep active) > Lava Burst (on cooldown or when it procs) > Earth Shock (cast at 6-7 Lightning Shield charges) > Lightning Bolt (filler)

Enhancement (DPS)

    Buffs: Windfury Weapon (Main Hand), Flametongue Weapon (Off-Hand), Lightning Shield

    Rotation: Flame Shock (keep active) > Stormstrike (on cooldown) > Lava Lash (on cooldown) > Unleash Elements > Lightning Bolt (cast when you have 5 Maelstrom Weapon charges)

Warlock

Affliction (DPS)

    Buff: Dark Intent

    Rotation: Agony (keep active) > Corruption (keep active) > Unstable Affliction (keep active) > Haunt (cast to buff DoT damage) > Malefic Grasp (channel as filler)

Demonology (DPS)

    Buff: Dark Intent

    Rotation: Corruption (keep active) > Hand of Gul'dan (build Demonic Fury) > Soul Fire (on Molten Core proc) > Shadow Bolt (filler) > Metamorphosis (activate to dump Demonic Fury using Touch of Chaos)

Destruction (DPS)

    Buff: Dark Intent

    Rotation: Immolate (keep active) > Conflagrate (build Burning Embers) > Chaos Bolt (dump Embers) > Incinerate (filler)

Warrior

Protection (Tank)

    Stance: Defensive Stance

    Rotation: Shield Slam (on cooldown) > Revenge (on cooldown) > Thunder Clap (2+ NPCs) > Devastate (filler) > Shield Block or Shield Barrier (spend Rage to survive)

    Pull: Charge into 2+ NPCs, then Thunder Clap on landing.

Arms (DPS)

    Stance: Battle Stance

    Rotation: Mortal Strike (on cooldown) > Colossus Smash (on cooldown) > Overpower (on proc) > Slam (filler) > Heroic Strike (dump excess Rage if capping)

Fury (DPS)

    Stance: Titan's Grip or Single-Minded Fury (Passive)

    Rotation: Bloodthirst (on cooldown) > Colossus Smash (on cooldown) > Raging Blow (on proc) > Wild Strike (on Bloodsurge proc) > Heroic Strike (dump excess Rage)
