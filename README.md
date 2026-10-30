# mod-playerbots (SkyFire)

A port of the AzerothCore playerbots system to SkyFire (World of Warcraft
5.4.8 / Mists of Pandaria), intended to populate a test server with bot players
so features such as LFG, LFR, random battlegrounds, arenas and open-world
activity can be exercised without a large human population.

**Command reference:** see [COMMANDS.md](COMMANDS.md) for every GM slash
command and whisper/party order (including what is *not* ported yet, such as
`co` / `nc`).

Upstream references:

* Core fork:  https://github.com/mod-playerbots/azerothcore-wotlk/tree/Playerbot
* Module:     https://github.com/mod-playerbots/mod-playerbots
* AC command wiki: https://github.com/mod-playerbots/mod-playerbots/wiki/Playerbot-Commands

## Status

This module is being ported in phases. What is present today:

* Integrated into SkyFire's new module system (builds + links automatically).
* Configuration (`playerbots.conf.dist`) and a `PlayerbotMgr` that reads it.
* Phase 1 core hooks: socketless bot `WorldSession`, synchronous character
  login, and a per-player tick hook (`PlayerScript::OnUpdate`).
* GM commands and chat orders — full tables in [COMMANDS.md](COMMANDS.md).
* Basic bot AI: follow/assist when grouped, combat fillers, LFG role/proposal
  answers, solo wander, auto-accept trade/duel, corpse loot, and opportunistic
  free repair near repair NPCs.
* Self-bot mode (`.playerbots self`): you move, AI casts fillers / Wave-1
  per-spec rotations (Ret, WW, BM, Shadow, Affliction, Elemental).
* Per-spec rotation framework under `src/rotations/` (Hekili `.simc` as offline
  reference).

### Self-bot mode

`.playerbots self` attaches AI to **your** character without replacing the
client session:

* You move and jump as normal.
* In combat the AI picks a target (selection / attackers / `attack` order) and
  casts the class filler when you are in range with LoS.
* `.playerbots init` refreshes gear and spec. On yourself: `.playerbots init`
  or `.playerbots init self`. Whisper a bot `autogear` to upgrade its gear for
  its current spec, or `maintenance` to refresh spells/talents/glyphs/mounts
  and repair (see [COMMANDS.md](COMMANDS.md)).

Toggle again or `.playerbots self off` to detach.

### Bot auto-creation

The auto-creator provisions dedicated bot accounts and fills them with
characters, so you do not have to create bot accounts/characters by hand. It is
driven entirely by the `Playerbots.AutoCreate.*` keys and is **incremental and
idempotent** - existing accounts and characters are reused, only the missing
ones are created.

* Accounts are named `<AccountPrefix><n>` (e.g. `RNDBOT1` .. `RNDBOTn`) up to
  `Playerbots.AutoCreate.AccountCount`. New accounts get the realm's expansion
  so every race/class is available.
* Each account is filled to `Playerbots.AutoCreate.CharactersPerAccount`
  characters. For each character the faction is chosen by `AlliancePct`, the role
  by `TankPct` / `HealerPct` (remainder become DPS), and a class capable of that
  role plus a valid race for the faction are selected. Characters are created at
  `Playerbots.AutoCreate.Level`. Death Knights and neutral Pandaren are skipped.
* If the start level is at least 10, the character is given the specialization
  matching its rolled role (e.g. a tank Warrior gets Protection) and learns the
  spells that spec grants for its level, so it can actually fill the role. Use
  `.playerbots init` to re-apply this later (after leveling, respec, etc.).

Run it on demand with `.playerbots create`, or set
`Playerbots.AutoCreate.OnStartup = 1` to run it once when the world boots. Newly
created characters are picked up by the random-bot pool automatically.

### Gear and role changes (`.playerbots init`)

`.playerbots init` re-gears an active bot for its **current level** and spec
(level is unchanged unless you pass `relevel`):

* Existing equipment (except shirt/tabard) is cleared and each slot is refilled
  with the best uncommon-to-epic item the bot can equip, biased toward the
  spec's primary stat (STR/AGI/INT). Armor slots prefer the heaviest armor type
  the class can wear at that level; weapons/shields/off-hands/ranged are chosen
  per class and role. Re-run it after leveling to pull in level-appropriate
  upgrades (e.g. gear a fresh level-80 bot).
* Optional `relevel` rolls a new level in `AutoCreate.MinLevel`–`MaxLevel`
  (set Min=Max for a fixed level), then gears for that level.
* Optional `tele` / `teleport` scatters the bot to a level- and faction-safe
  open-world anchor **after** gear (and after `relevel` if used), so the
  destination matches the bot’s final level. Levels 1–5 go to homebind /
  race spawn instead of a random hub.
* First-time gear on create / first login also scatters when
  `Playerbots.AutoCreate.TeleportOnInit = 1` (default), except levels 1–5
  which stay at their normal starting area.
* Learns remaining **class trainer** spells for the bot's level, plus SkillLine /
  specialization spells.
* Teaches **riding** by level (20/40/60/70/80) and one random mount per unlocked
  tier (normal ground, swift ground, flying, epic flying) from faction pools.
* Add a role token (`tank`/`healer`/`dps`) to change the bot's spec first, then
  re-gear for the new role - e.g. `.playerbots init Arix tank`.

What is **not** implemented yet (tracked in `PORTING.md`):

* Mounts (learn/use a default ground/flying mount).
* Vendor sell-junk, rest/eat/drink, gossip/quest NPC interaction.
* Deeper per-spec combat rotations.
* Full AC-style `co`/`nc` strategy engine, RTSC, loot-lists, item/vendor chat ops.
* The automated LFG/LFR/RBG queue behaviour.

To try it: set `Playerbots.Enable = 1`, then `.playerbots add <charname>` for a
character that is offline. The bot appears in the world at its saved location.

## Building

Nothing special is required. With the SkyFire module system enabled
(`-DMODULES=1`, the default), this folder is compiled into the `modules`
library and linked into `worldserver` automatically.

## Configuration

Copy `playerbots.conf.dist` to `playerbots.conf` next to `worldserver.conf`
(the build also stages a copy next to the binary) and set `Playerbots.Enable = 1`.

## Database (SQL)

Module SQL lives under `sql/` and is **not** applied by the core updater. Apply
files to the matching database:

| Path | Database |
|------|----------|
| [`sql/characters/playerbots_preferred_mounts.sql`](sql/characters/playerbots_preferred_mounts.sql) | **characters** |

Preferred mounts: the table is also created at runtime with
`CREATE TABLE IF NOT EXISTS` on module load, so a fresh characters DB will not
error. Keep the SQL file for tooling, backups, and manual apply.

## Why this is a port, not a drop-in

The upstream module targets AzerothCore, which is WotLK (3.3.5a). SkyFire is
MoP (5.4.8): the class/spell system, talent trees, LFG internals, packet/opcode
layout, and many core APIs differ substantially. Upstream also requires a
*forked core* with extra hooks, so a straight copy does not compile. `PORTING.md`
describes the staged approach used here.
