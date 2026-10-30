# Playerbot Commands

Reference for SkyFire `mod-playerbots`. Commands fall into two groups:

1. **GM slash commands** — typed in chat as `.playerbots …` (require GM permission).
2. **Chat orders** — whisper a bot, or say them in party/raid chat so every grouped bot hears them.

Whisper replies with a short ack. Party/raid orders apply silently (no spam).

---

## GM slash commands (`.playerbots …`)

| Command | What it does |
| --- | --- |
| `.playerbots status` | Module on/off, random-bot counts, pending logins, login/init pacing. |
| `.playerbots list` | Names/GUIDs of active (socket) bots. |
| `.playerbots add <name>` | Log an offline character in as a full bot session. |
| `.playerbots remove <name>` | Log that bot out and free its session. |
| `.playerbots summon` | Teleport all bots in your group to you. |
| `.playerbots reload` | Reload `playerbots.conf` and the random-bot candidate pool. |
| `.playerbots create` | Run the auto-creator (accounts + characters from config; each new char is auto-inited with conf gear). |
| `.playerbots wipe confirm` | Delete all accounts matching `AccountPrefix` (and their characters). Requires the word `confirm`. |
| `.playerbots init [<name>\|all\|self] [tank\|healer\|dps\|<spec>] [rare\|epic\|…] [relevel] [tele]` | Re-apply specialization, talents, and gear. `all` (and grouped bots on bare init) run in a background queue (`Playerbots.Init.PerTick`). Level stays unless `relevel` is passed. `tele` / `teleport` scatters to a level/faction-safe anchor after init (uses post-`relevel` level). |
| `.playerbots self [on\|off]` | Attach/detach cast-only AI on **your** character. |

### Resetting the bot pool (e.g. all level 80 epic)

1. Set `Playerbots.AutoCreate.MinLevel` / `MaxLevel`, **`AccountCount`** (accounts to create; not the same as `MaxBots`), CharactersPerAccount, and gear quality in `playerbots.conf`.
2. Leave `AutoCreate.InitOnCreate = 0` for fast create (recommended for large pools); set `1` only if you need every new char geared before first login.
3. Either set `Playerbots.DeleteRandomBotAccounts = 1` and restart once, **or** run `.playerbots wipe confirm`.
4. Set `DeleteRandomBotAccounts = 0` again.
5. Run `.playerbots create` (or enable `AutoCreate.OnStartup` and restart).
6. Optional: bring bots online and `.playerbots init all` / `relevel` as needed.

### `.playerbots init`

Re-gears and re-applies spec/talents for the bot's **current level** by default.
Init also pulls class-trainer spells for that level, riding skills, and one
random mount per unlocked riding tier (level-gated: 20 / 40 / 60 / 70).

`Playerbots.AutoCreate.MinLevel` / `MaxLevel` do **not** change levels on a
normal init — they only apply when creating characters, or when you pass
`relevel`.

| Args | Effect |
| --- | --- |
| *(none)* | Initialize **yourself**, then every bot in your group. |
| `self` | Initialize only yourself. |
| `<charname>` | Initialize that active bot (or self-bot). |
| `all` | Queue every active socket bot for background init (`Init.PerTick` each world tick). |
| `… tank` / `healer` / `dps` | Switch to that role’s **default** spec, then gear. |
| `… <spec>` | Switch to an explicit specialization (needed for hybrid DPS). |
| `… rare` / `epic` / … | Cap gear quality (default **epic**). Also: `blue`, `purple`, `quality=rare`. |
| `… relevel` | Roll a new level in conf `MinLevel`–`MaxLevel`, then init for that level. |
| `… tele` / `teleport` | After gear (and after `relevel` if used), teleport to a safe open-world anchor for the bot’s **final** level and faction. Levels **1–5** go to homebind / race spawn instead. |

First-time gear (create with `InitOnCreate`, or deferred first login when still bare)
also teleports when `Playerbots.AutoCreate.TeleportOnInit = 1` (default), but
**skips levels 1–5** so starters remain in their normal starting area. Later
manual inits do **not** auto-teleport — add `tele` explicitly.

Tokens may be in any order, e.g. `.playerbots init self fury rare` or
`.playerbots init all rare relevel tele`.

**Examples**

```
.playerbots init all fury rare              # keep levels, re-spec/re-gear
.playerbots init all rare relevel           # roll 1–25 (or whatever conf says), then gear
.playerbots init all relevel tele           # relevel, gear, then scatter by new level
.playerbots init all tele                   # keep levels, re-gear, then scatter by current level
.playerbots init Botname tank               # party bot stays level 30, becomes tank
```

To force everyone to one level: set `MinLevel = MaxLevel = 80`, then
`.playerbots init all relevel`. Or use GM `.level` / `.modify level` on individuals.

**Hybrid DPS:** `dps` alone is not enough when a class has two damage specs:

| Class | `dps` defaults to | Explicit specs |
| --- | --- | --- |
| Shaman | Elemental | `elemental` / `ele`, `enhancement` / `enh` |
| Druid | Balance (moonkin) | `moonkin` / `balance`, `feral` |
| Druid tank/heal | — | `guardian`, `resto` / `healer` |

Other useful tokens: `ret`, `shadow`, `bm`, `ww`, `aff`, `arms`, `fury`, `frost`, etc.

### Combat rotations

Bots (and `.playerbots self`) use per-spec priority lists for **all 34 MoP specs**:

| Class | Specs |
|-------|--------|
| Warrior | Arms, Fury, Protection |
| Paladin | Holy (heal + healer-dps), Protection, Retribution |
| Hunter | Beast Mastery, Marksmanship, Survival |
| Rogue | Assassination, Combat, Subtlety |
| Priest | Discipline / Holy (heal + healer-dps), Shadow |
| Death Knight | Blood, Frost, Unholy |
| Shaman | Elemental, Enhancement, Restoration (heal + healer-dps) |
| Mage | Arcane, Fire, Frost |
| Warlock | Affliction, Demonology, Destruction |
| Monk | Brewmaster, Mistweaver (heal + healer-dps), Windwalker |
| Druid | Balance, Feral, Guardian, Restoration (heal + healer-dps) |

Priorities are simplified MoP dungeon/raid lines (Hekili/SimC-inspired). Healers
use `SelectNextHeal` until nobody needs healing; with `co +healer dps` they switch
to the healer-dps damage lists above.

### Tank / healer in groups

- **Tanks** peel mobs attacking party members and taunt when they lose threat.
- **Healers** use per-spec priorities (Holy Pala, Disc/Holy Priest, Resto Sham/Druid,
  Mistweaver): HoTs/shields, urgency Flash heals, and group AoE when several allies
  are injured. They still fall back to a simple class heal if the selector finds
  nothing ready.

Init bots to the right role before queueing: `.playerbots init Botname tank` /
`healer` / `dps` (or an explicit spec).

### Shared combat utilities

In combat, bots (including healers and `.playerbots self`) will:

- **Interrupt** when their target is casting (class kick / silence).
- **Party support**: cleanse/dispel removable debuffs on allies (Pala Cleanse,
  Priest Purify, Shaman Cleanse Spirit, Mage Remove Curse, Druid Nature's Cure,
  Monk Detox); self-defensives under ~40% HP; Paladins use **Lay on Hands**
  (ally under ~25%) and **Hand of Protection** (non-tank ally under ~35%,
  respects Forbearance).
- Use **racials** (Blood Fury, Berserking, Arcane Torrent, etc.; defensive racials
  when low HP). Offensive racials and on-use **trinkets** require `co +boost`
  (default on).
- **Crowd control** when `co +cc` (default on for Mage/Hunter/Warlock): Polymorph,
  Fear/Banish, Freezing Trap, Hex, Shackle, Cyclone, Sap — prefers adds over the
  tank's current target.
- **Avoid AoE** when `co +avoid aoe` (default on): step out of damaging ground
  dynobjects (Rain of Fire–style). Tanks in tank mode stay put.

Multi-target rotation branches require `co +aoe` (default on for tank/DPS, off
for healers). Disable with `co -aoe` for single-target only.

### Formation / follow

Out of combat, bots take **unique slots** by role (tank / healer / melee DPS /
ranged DPS), GUID-sorted within the bucket so two DPS no longer stand on the
same flank. Follow destinations are height-clamped; bots `MovePoint` to the
slot when clipped or off-slot instead of parking inside walls/stairs.

Party rest (`nc +food`): bots sit only while food/drink auras are active. Full
allies stand and wait; empty sits no longer spread through the group.

### LFG / LFR party roles

MoP blocks dungeon finder join until every party member has a **party role** set
(tank / healer / dps). Bots now set that from their active specialization as soon
as they are grouped, and still auto-accept the LFG role check + ready proposal.

When a real player queues (solo or with bots) and the dungeon forms, that player
is preferred as **LFG group leader** so bots auto-follow them like a normal party.

Queue needs a workable mix (at least one tank + healer for normal dungeons). All-DPS
parties will fail the role check with wrong roles.

**LFG with bots:** set `Playerbots.RandomBotJoinLfg = 1` so ungrouped bots fill
your dungeon finder queue (console logs each join). Bot tanks are skipped when
you queue as tank; bot healers are skipped when you queue as healer. You can
still invite bots to your party — grouped bots answer the role check and ready
check. Bot-only LFG groups are ejected back to the open world.

Init also teaches level-gated armor proficiency (mail at 40 for hunter/shaman,
plate at 40 for warrior/paladin, etc.) and gears by **RequiredLevel** near the
bot’s level (so a level 90 no longer gets ~70 ilvl from the old ItemLevel cap).

Examples:

```
.playerbots init
.playerbots init self enhancement
.playerbots init self moonkin
.playerbots init self feral
.playerbots init self fury rare
.playerbots init rare self fury
.playerbots init Arix elemental
.playerbots init all dps
```

### `.playerbots self`

Attaches AI to your logged-in character **without** replacing the client session:

- You keep WASD / jump / camera.
- AI picks combat targets and casts class filler / Wave-1 rotation spells when you are in range with LoS.
- Toggle again, or `.playerbots self off`, to detach.

Useful for testing rotations and for `.playerbots init` gearing yourself.

---

## Chat orders (whisper or party/raid)

Send these as the message text (case-insensitive).

| Order | What it does |
| --- | --- |
| `help` | Whisper back the order list. |
| `stay` | Hold position (no follow/wander). Self-bot: no-op (you move). |
| `follow` / `come` | Resume following the group leader. Self-bot: no-op. |
| `flee` | Clear combat orders and teleport to the issuer. Self-bot: no-op. Ranged bots also kite in combat when too close (config `Playerbots.Flee`). |
| `leave` | Leave the current group. Self-bot: ignored. |
| `summon` | Teleport the bot to the issuer. Self-bot: ignored. |
| `grind` | Aggressive; pick nearest attackable hostiles. |
| `reset` | Clear stay/passive/grind/forced target, stop attack/casts. |
| `passive` | Do not assist or pull; still fight back if attacked. |
| `aggressive` / `aggro` | Resume normal assist behaviour. |
| `attack` | Attack the **issuer’s** current target. |
| `tank attack` | Tank-spec bots pull your target; other bots hold until a mob is actually hitting the party, then auto-assist (lowest-HP first). Use `passive` to stay out. |
| `pull` | Tank-spec bots: reach your target (or RTI mark), cast a ranged opener / taunt, then fight. |
| `rti` / `rti skull\|…` | Set which raid icon bots focus (default skull). Tanks auto-mark that icon on an unmarked pack mob in combat. |
| `dps attack` | Damage-role bots attack your target. |
| `maintenance` | Keep spec and gear; learn trainer/level spells and armor proficiencies, re-apply recommended talents and glyphs, riding + mounts, fill hunter Call Pet slots, repair. Alt bots skip parts disabled by `Playerbots.AltMaintenance.*`. Off with `Playerbots.Maintenance.Command = 0`. |
| `autogear` | Upgrade worn PvE gear for the bot's current spec: a slot changes only for a higher item level (a piece with the wrong primary stat counts half), and the old piece goes to the bags (full bags keep it). Capped by `Playerbots.AutoGear.QualityLimit` / `ItemLevelLimit`. Not in combat. |
| `autogear reset` | Destroy worn gear and regear every slot from scratch (re-rolls the PvP set too). Combines with the options below: `autogear reset green`, `autogear reset 450`, `autogear reset match`. |
| `autogear <quality>` | Cap quality: `white`/`common`, `green`/`uncommon`, `blue`/`rare`, `purple`/`epic`, `orange`/`legendary` (never above the config limit). |
| `autogear <ilvl>` | Cap item level, e.g. `autogear 450` (clamped to `ItemLevelLimit` when non-zero). Numbers 0–5 are rejected as qualities. |
| `autogear match` | Cap item level at the issuer's average equipped item level (filled slots only). |
| `debug` / `debug on` / `debug off` | Bot says combat actions in chat (`Combat: Casting Arcane Shot`). Also `co +debug`. |
| `eat` / `drink` / `food` | Sit and use bag food/drink (or fallback regen) until nearly full. Self-bots: AI will not stand you up or interrupt clicked food/drink; regen continues past the start threshold. |
| `heal` | Healer-only shortcut: strict heal mode (same as `co +heal`). |
| `healer dps` | Healer-only shortcut: `co +healer dps`. |
| `save mana` / `save mana off` | Healer-only shortcut: `co +save mana` / `co -save mana`. |
| `go` / `go here` | Walk to you (or your selected unit) on the same map. |
| `go x y z` / `go x;y;z` | Walk to coordinates on the bot’s current map (path-checked). |
| `go <name>` | Walk to a saved position (see `position`). |
| `position save <name>` | Store the bot’s current XYZ under `<name>`. |
| `position` / `position ?` | List saved position names. |
| `position go <name>` | Same as `go <name>`. |
| `sell` / `sell junk` | Walk to a nearby vendor (or same-map vendor hub) and sell gray junk; also repairs if the NPC can. Idle bots do this opportunistically. |
| `sell all` | Same path, but also vendor-dumps unbound greens+ (soulbound/quest/usable gear kept). |
| `mount` / `dismount` | Force mount/dismount. Bots also **auto-mount when the master mounts**, matching ground vs flying vs swift flying, and dismount when the master does (or in combat). |
| `mount prefer <spellId>` | Remember a preferred ground or flying mount spell for this bot (`playerbots_preferred_mounts`). |
| `mount prefer clear` | Clear preferred mounts for this bot. |
| `gossip` / `gossip <n>` | List or select a gossip menu option on a nearby NPC. |
| `quests` / `accept` / `turn in` | Accept/turn-in nearby (walks in), with reward-choice AI. With `nc +quests`, also travels to same-map objectives. |

Grouped bots on the `follow` pack (`-quests,-grind`) do **not** path to givers/objectives alone. When the master accepts a quest, eligible bots on the same map auto-add it to their quest log so they share progress while staying in formation. Auto turn-in while following is not enabled.

Travel is cancelled by combat, `follow`, `stay`, `flee`, `summon`, or `reset`. Arrival resumes follow when `nc +follow` is on. Same-map only (no TravelNode / portals yet).

### Combat / non-combat strategies (`co` / `nc`)

Strategies are **role-gated** from the bot's current specialization:

| Role | Combat (`co`) strategies |
| --- | --- |
| **Tank** | `tank` (peel + hold threat), `tank assist`, `dps` (tank plays as DPS / low threat) |
| **Healer** | `heal` (strict heal), `healer dps`, `save mana`, `wait for attack` (optional) |
| **DPS** | `dps`, `dps assist` (party assist; default on), `threat`, `wait for attack` (default on — delay open) |
| **All** | `passive`, `grind`, `debug` (say casts in chat) |

| Non-combat (`nc`) | Effect |
| --- | --- |
| `food` | Auto sit + regen HP/mana when low (default on) |
| `follow` / `stay` | Follow leader or hold position |
| `loot` | Loot corpses |
| `quests` | Open-world accept/turn-in + same-map objective travel (default on for ungrouped random bots with `+grind`; cleared by `follow` pack). Grouped bots still mirror the master’s accepts into their logs while following. |
| `passive` / `grind` | Same shared flags |

Chat shortcuts rewrite strategy packs like AzerothCore (not just one bool):

| Shortcut | Pack (approx.) |
| --- | --- |
| `follow` | NC `+follow,-passive,-grind`; combat `-stay,-passive,-grind` |
| `stay` | NC/combat `+stay,-passive` (combat also `-follow`) |
| `flee` | both `+follow,-stay,+passive` then run to master |
| `grind` | NC `+grind,-passive,-stay` (+ combat grind) |
| `passive` / `aggressive` | `+passive` / `-passive` on both engines |
| `reset` | role-default strategy sets |

Wrong-role strategies are rejected (`!healer dps(wrong role)`) so `/p co +healer dps` only changes healers.

```
/w BotName co ?
/w Yourself co +healer dps
/p @heal co +healer dps
/p @tank co +tank
/p @dps co +threat
/p @dps co -dps assist
/p nc +food
```

Bots always whisper their `co:` and `nc:` state back.

Default on attach/init: tanks `+tank +tank assist`, healers `+heal +save mana`, DPS `+dps +dps assist +threat`, everyone `+food +follow +loot`.


### Party role filters

Prefix an order with a filter so only matching bots react (party/raid):

| Filter | Who reacts |
| --- | --- |
| `@tank …` | Tank-spec bots |
| `@dps …` | Damage-spec bots |
| `@heal …` / `@healer …` | Healer-spec bots |
| `@ranged …` | Hunter / priest / mage / warlock / Elemental / Balance (non-tank) |

Examples:

```
/w BotName stay
/p follow
/p attack
/p @tank attack
/p @dps follow
/r grind
```

---

## `co` / `nc` strategies

AzerothCore-style combat (`co`) and non-combat (`nc`) strategy toggles are
supported (`food`, `save mana`, `healer dps`, `heal`, `follow`, `stay`, `loot`,
`grind`, `passive`, plus combat `aoe`, `boost`, `cc`, `avoid aoe`, `threat`,
`wait for attack`, role packs).

Full AC strategy engine features not yet ported (RTSC, raid strats, `ll`, etc.)
remain tracked in `PORTING.md`.

Closest equivalents for older one-shot orders:

| AC-style idea | Use instead |
| --- | --- |
| Stay put | `stay` or `nc +stay` |
| Follow master | `follow` or `nc +follow` |
| Attack target | `attack` / `tank attack` / `dps attack` |
| Attack anything nearby | `grind` or `co +grind` |
| Stop assisting | `passive` or `co +passive` |
| Resume assist | `aggressive` or `co -passive` |
| Clear bot state | `reset` |
| Re-gear | `autogear` / `autogear reset` or `.playerbots init` |
| Spells / talents refresh | `maintenance` or `.playerbots init` |
| Eat / drink | `eat` / `drink` or `nc +food` |
| Strict healer / healer DPS | `co +heal` / `co +healer dps` |
| Conserve healer mana | `co +save mana` |
| Single-target only | `co -aoe` |
| Hold burst trinkets/racials | `co -boost` |
| Enable crowd control | `co +cc` |
| Disable ground AoE dodge | `co -avoid aoe` |
| Walk to point / master | `go` / `go x y z` / `position save\|go <name>` |
| Sell junk / greens | `sell` / `sell junk` / `sell all` |
| Mount / dismount | `mount` / `dismount` / `mount prefer` |
| Gossip | `gossip` / `gossip <n>` |
| Nearby / auto quest | `quests` / `accept` / `turn in` / `nc +quests` |

---

## Quick start

1. Enable the module in `playerbots.conf` (`Playerbots.Enable = 1`).
2. `.playerbots add SomeOfflineChar` — or let random bots spawn from config.
3. Invite bots, then `/p follow` or `/p attack` with a mob selected.
4. Optional: `.playerbots self` on your own character to test casting while you move.
5. `.playerbots init` after leveling to refresh gear/spec (use `enhancement` / `feral` / `moonkin` when you need a specific hybrid DPS tree).

See `README.md` for build/config and `PORTING.md` for port status and backlog.
