# Playerbots port plan (AzerothCore/WotLK -> SkyFire/MoP 5.4.8)

This document tracks the staged effort to bring the AzerothCore playerbots
system to SkyFire. The upstream project depends on a **forked core** with extra
scripting hooks plus a very large module (bot sessions, AI, strategies, actions,
triggers, a random-bot pool, and gear/talent generation). It cannot be copied
verbatim because SkyFire is a different expansion (5.4.8) with different core
APIs, spell/talent systems, LFG internals and opcodes.

The work is therefore split into independently testable phases.

## Phase 0 - Module system (DONE)

* Added an AzerothCore-style module system to SkyFire (`modules/` +
  `AddModulesScripts()` core hook). See `modules/README.md`.
* Registered `mod-playerbots` with config, manager and `.playerbots` command.

## Phase 1 - Core hooks (the "forked core" equivalent) (IN PROGRESS)

Upstream relies on core changes. On SkyFire these are added as minimal,
well-isolated hooks (prefer new `ScriptMgr` hooks over scattered edits):

* [DONE] Socketless `WorldSession` ("bot session"): a `SetBot/IsBot` flag,
  null-socket guards in `WorldSession::Update`, and `SendPacket` already no-ops
  without a socket. Bot sessions are NOT tracked in `World::m_sessions`; they are
  owned by `PlayerbotMgr`.
* [DONE] Character login for a session: `BeginBotCharacterLogin` /
  `PollBotCharacterLogin` (async; random pool) and sync `LoginBotCharacter`
  (single-bot add/create). Random pool never spin-waits the world thread on DB.
* [DONE] Per-`Player` tick hook: `PlayerScript::OnUpdate(Player*, uint32)`,
  dispatched from `Player::Update` via `ScriptMgr::OnPlayerUpdate`. Because bot
  players are updated by their `Map`, this fires for bots automatically - the
  attach point for Phase 3 AI.
* [DONE] `PlayerbotMgr` bot lifecycle + `.playerbots add/remove/list` commands,
  with cleanup on world shutdown.

Remaining for later phases:

* Outgoing packet interception so bot AI can react to server packets it would
  normally receive as a client (Phase 3).
* Group/LFG/Battleground entry points reachable programmatically for bots
  (Phase 4).

## Phase 2 - Bot lifecycle (IN PROGRESS)

* [DONE] Random-bot pool sourced from dedicated bot accounts
  (`Playerbots.RandomBots.AccountPrefix`). Candidates are all characters on
  accounts whose username matches the prefix (looked up across the auth and
  character databases).
* [DONE] `PlayerbotMgr::Update()` driven from the module's
  `WorldScript::OnUpdate` (no core change): keeps up to
  `Playerbots.RandomBots.MaxBots` online via async login pipeline
  (`LoginInterval` starts, `MaxPendingLogins` in flight, `LoginsPerTick`
  completes LoadFromDB), trims excess when the cap is lowered, and cleans up
  bots whose player left the world. `Init.PerTick` is gear-only.
* [DONE] `.playerbots reload` (re-reads config + candidate pool) and richer
  `.playerbots status` (random/active/candidate counts).
* [DONE] Bot auto-creator (`Playerbots.AutoCreate.*` / `.playerbots create`):
  provisions bot accounts and characters by faction and tank/healer/dps ratio at
  a configured MinLevel–MaxLevel band (legacy `Level` seeds both), with unique
  generated names and correct realm id. Each new character is auto-inited
  (spec/gear) so the pool is LFG-ready without a manual init pass.
* [DONE] Bot pool wipe: `Playerbots.DeleteRandomBotAccounts` on startup and
  `.playerbots wipe confirm` delete all `AccountPrefix` accounts/characters.
* [DONE] Specialization assignment: created/init bots (level >= 10) get the spec
  matching their role and learn its spells (`Player::LearnSpecialization`).
* [DONE] Gear initialisation via create auto-init / `.playerbots init`: clears and
  refills slots from `item_template` using conf `Gear.MinQuality`–`MaxQuality`
  (init quality token overrides the cap). Among the top ilvl candidates per slot,
  one is chosen at random.
* [DONE] Role change via `.playerbots init <name> <tank|healer|dps>`: switches
  the bot's spec and re-gears it for the new role.
* [TODO] Per-master "alt bots" (a real player summoning their own alts).
* [DONE] Async random-bot login pipeline (`BeginBotCharacterLogin` /
  `PollBotCharacterLogin`, `MaxPendingLogins` + `LoginsPerTick`) so the world
  thread does not spin-wait on character DB queries.

## Phase 3 - Bot AI framework (IN PROGRESS)

* [DONE] `PlayerbotAI` controller: one instance per bot, created in
  `PlayerbotMgr::SpawnBot` and destroyed with the bot. Ticked from
  `PlayerScript::OnUpdate` via `PlayerbotMgr::UpdateBotAI` (throttled to ~500ms
  per bot). This is the attach point for the strategy engine.
* [DONE] Auto-accept group/raid invites (mirrors `HandleGroupAcceptOpcode`) so
  bots can be pulled into parties, and thus LFG/LFR/RBG queues.
* [DONE] Defensive combat: bots keep swinging at a valid victim, retaliate
  against attackers, and chase the target into melee range.
* [DONE] Server-side movement: bots follow the group leader out of combat
  (spread around the leader by a GUID-derived angle) and chase in combat. State
  is tracked so generators aren't re-issued every tick.
* [DONE] Teleport/summon: bots warp to the leader when they're on another map or
  too far to catch up on foot (auto), and `.playerbots summon` teleports all of
  the caller's grouped bots to their position (manual). Because bots have no
  client to send the teleport ack, the core exposes
  `WorldSession::FinalizeBotTeleport()` (mirrors the near/far worldport ack
  handlers) which the module calls right after `Player::TeleportTo` - without it
  the bot got stuck "being teleported" forever.
* [DONE] Combat target acquisition/assist: bots pick a unit attacking them, and
  otherwise assist the group leader's target (within 60y) so the party
  focus-fires the same mob.
* [DONE] Basic class rotation: melee classes close in and auto-attack (paladin
  and rogue also use a filler strike); ranged/caster classes (hunter, priest,
  mage, warlock) hold at ~25y and spam a single filler spell. Bots only cast
  spells they actually know; casts go through the normal path so GCD/power/range/
  LoS are validated by the core.
* [DONE] Per-spec rotation framework (`modules/mod-playerbots/src/rotations/`):
  priority picker with aura/resource/enemy-count helpers. DPS specs with
  hand-ported MoP Hekili `.simc` priorities: Ret, WW, BM/MM/Survival, Shadow,
  Affliction/Demo/Destro, Elemental/Enhancement, Feral, Arms/Fury, Combat Rogue,
  Frost Mage, Unholy DK. Wave 3: Balance, Guardian, Fire/Arcane, Assassination/
  Subtlety, Frost/Blood DK, Prot Pala/War, Brewmaster. Elemental/Balance ranged
  stance. `.playerbots init` learns recommended talent spells and accepts
  explicit names (`enhancement`, `feral`, `moonkin`, …). Local `Hekili/` is
  reference-only (gitignored).
* [DONE] Wave 4 healers + shared combat utilities:
  * Per-spec heals: Holy Pala, Disc/Holy Priest, Resto Shaman/Druid, Mistweaver
    (via `SelectNextHeal`, used by `HandleHealing`).
  * Shared `TryCombatUtilities`: class interrupt when the target is casting,
    offensive/defensive racials, on-use trinkets — wired into DPS rotation and
    healer combat.
* [DONE] World interaction (first pass):
  * Solo bots wander to nearby random ground points so they aren't frozen statues.
  * Bots auto-accept trade windows and duel challenges.
  * Bots walk to and loot nearby corpses they are allowed to take from (group
    loot rules still apply via `isAllowedToLoot`).
  * Bots walk to a nearby repairer and repair for free when equipped durability
    drops below 50%.
  * Rest: `eat`/`drink` or `nc +food` — sit and regenerate HP/mana (no spell spam);
    thresholds via `Playerbots.Rest.HealthPct` / `ManaPct`. Self-bots never get
    stood up by rest AI (AFK sit / clicked food&drink stay intact).
  * Healer strategies via `co +heal` / `co +healer dps` / `co +save mana`.
  * Bots whisper back their `co:` / `nc:` state on strategy commands.
* [DONE] Chat orders via whisper or party/raid chat:
  * `stay` / `follow` (also `come`) - hold position or resume following.
  * `flee` / `summon` - run to / teleport to the issuer.
  * `leave` - leave the current group.
  * `grind` - attack nearest hostiles; `reset` clears orders/casts.
  * `passive` / `aggressive` - stop assisting (still retaliate) or resume normal
    assist behaviour.
  * `attack` - all bots attack the issuer's current target.
  * `tank attack` / `dps attack` - same, filtered by the bot's combat role
    (from its active specialization).
  * `pull` - tanks engage the issuer's target, or the configured RTI mark.
  * `rti` / `rti skull|cross|…` - set which raid icon bots focus (default skull).
  * `maintenance` - AC `MaintenanceAction` (`PlayerbotMgr::MaintainBot`):
    spells, proficiencies, talents, glyphs, riding/mounts, hunter pets, repair;
    keeps spec and gear. Alt bots honour `Playerbots.AltMaintenance.*`. Not
    ported (no SkyFire equivalent yet): bags, food/reagents/potions,
    enchants/gems, reputation, attunement quests, pet talents, keyring/ammo
    (removed in MoP).
  * `autogear [reset] [match|<quality>|<ilvl>]` - AC `AutoGearAction`
    (`PlayerbotMgr::AutoGearBot`): incremental slot-by-slot upgrades scored by
    item level, `reset` for a full re-roll, config caps
    `Playerbots.AutoGear.*`. `autogear bis` is not ported (needs AC's BiS list
    tables).
  * Party filters: `@tank` / `@dps` / `@heal` / `@ranged` before an order.
  * `help` - list orders (whisper reply). Whisper commands get a short ack;
    party/raid orders apply silently to avoid spam.
* [DONE] Self-bot mode (`.playerbots self`): attach cast-only AI to a real
  logged-in player. Client keeps movement; AI casts fillers / per-spec rotations.
  `.playerbots init` with no args gears yourself and grouped bots.
* [DONE] **Thin AC-style strategy engine** (`src/engine/BotStrategyEngine`):
  * Named strategy sets per `BotState` (Combat / NonCombat), source of truth for
    `co` / `nc` (+/-/~/?, comma lists).
  * Chat shortcuts apply AC packs: `follow`, `stay`, `flee`, `grind`, `passive`,
    `aggressive`, `reset` rewrite both engines like AC `ChatShortcutActions`.
  * Procedural AI still reads synced flags (`_passive`, `_stay`, …) so MoP
    rotations and movement keep working.
  * DPS default includes `+dps assist` (group assist); `-dps assist` = own
    aggro / forced targets only.
* [DONE] **Trigger → Action → Queue** (`src/engine/BotAiEngine` + Action/Trigger/
  Queue/Multiplier): AC-shaped tick selects combat/rest/follow/stay/loot/wander.
  `PassiveMultiplier` zeroes combat when `+passive`. MoP `rotations/` unchanged.
* [DONE] **Target Values** (`BotTargetValues`): pull / current / dps (least HP) /
  tank / assist-tank — SelectTarget reads these like AC AiObjectContext values.
* [DONE] **wait for attack** (`co +wait for attack`, default on for DPS): non-tanks
  hold damage for `Playerbots.WaitForAttack.Seconds` after combat starts; still
  fight back if attacked. Tanks ignore. Disable with `co -wait for attack`.
* [DONE] **Role formations**: unique GUID-sorted slots per tank / healer / melee
  DPS / ranged DPS bucket (`BotFormation`) with radial stagger so same-role bots
  do not stack. Follow uses height-validated points (`BotMovement::MoveToFollowSlot`)
  to reduce wall/stair clipping.
* [DONE] **Party support utilities** (`TryPartySupport` in `TryCombatUtilities`):
  class cleanses on dispelable ally debuffs, self-defensives under ~40% HP,
  Paladin Lay on Hands / Hand of Protection on critically low allies.
* [DONE] Richer AC actions: raid-target icon preference (`rti skull` etc., default
  skull) in SelectTarget; tank `pull` as reach → opener → fight; combat kite via
  `BotFleeManager` when ranged is too close; tank auto-marks the preferred RTI on
  an unmarked pack mob.
* [DONE] Class/raid buff maintenance (self + party equivalents), recommended
  major/minor glyphs on `.playerbots init` (3+3 per spec).
* [DONE] Deeper cooldowns/DoTs/AoE on thinner DPS lines (Balance, Destro/Demo,
  Subtlety, MM, Arms, Feral). Still TODO: SimC-faithful tuning, glyph-aware
  rotation branches, boss-specific holds, trinket sync polish.
* [DONE] Party resurrection: Priest/Pala/Shaman/Druid/Monk OOC rez, Druid
  Rebirth / DK Raise Ally in combat; dead bots auto-accept rez requests.
* [DONE] **co aoe / boost / cc / avoid aoe** (thin gates on MoP rotations):
  `aoe` clamps multi-target branches; `boost` gates trinkets + offensive racials
  (class major CDs still fire); `cc` runs thin Polymorph/Fear/Trap/Hex/etc.;
  `avoid aoe` steps out of damaging dynobjects. Defaults: aoe on tank/DPS,
  boost + avoid aoe on all, cc on Mage/Hunter/Warlock.
* [DONE] **Point movement / travel destinations**: chat `go` / `go x y z` /
  `position save|go <name>`; `BotMovement::MoveTo` uses PathGenerator +
  generatePath; `TravelAction` walks until arrival. Same-map only — TravelNode /
  portals / RTSC / auto quest POIs still deferred.
* [DONE] **Init scatter teleports**: `.playerbots init … tele` / `teleport`
  picks a curated open-world anchor by post-init level + faction via
  `BotTeleportMaps::TeleportForLevel` (same map/zone gates as above).
  Master summon / follow-teleport stays unrestricted.
* [TODO] Broader random teleports to POI / banker / flight master while
  wandering (reuse `CanBotTeleportTo`).
  Level map gates: `<69` EK/Kalimdor + worgen/goblin/pandaren starts; `69–79`
  + Outland; `80–84` + Northrend; `85+` + Cata/MoP continents. Faction: no
  Alliance scatter into Orgrimmar (etc.), no Horde into Stormwind (etc.).
  Master summon / follow-teleport stays unrestricted.
* [TODO] AC wiki backlog (later): RTSC/aedm, loot lists (`ll`), item/vendor
  chat ops, glyphs, raid-specific strats, Multibot addon protocol.
* [DONE] **Non-combat sell / mount / nearby quests** (thin pass): sell
  `ITEM_QUALITY_POOR` at nearby vendors + free repair; auto-mount OOC outdoors
  when traveling or following far; accept/turn-in on nearby questgivers
  (`PrepareQuestMenu` / `AddQuest` / `RewardQuest`). Chat: `sell`, `mount` /
  `dismount`, `quests`/`accept`/`turn in`.
* [DONE] **Vendor hubs + greens dump**: in-memory vendor hub index from
  `creature`/`creature_template`; same-map travel when no vendor in 20y;
  vendor-dump unbound uncommon+ (not real AH). Conf:
  `Playerbots.Vendor.SellGreens`, `HubMaxDistance`. Chat: `sell all`.
* [DONE] **Preferred-mount DB**: `playerbots_preferred_mounts` (characters DB);
  `TryMount` prefers configured spell. Chat: `mount prefer <spellId>|clear`.
* [DONE] **Thin gossip**: hello + select by index; 1–2 hop auto for vendor/quest
  options. Chat: `gossip` / `gossip <n>`.
* [DONE] **Reward-choice AI**: `BestRewardIndex` (upgrade / sell price). Conf:
  `Playerbots.Quest.AutoPickReward`.
* [DONE] **Open-world auto-quest** (`nc +quests`): walk to questgivers, turn-in /
  accept, same-map objective travel. Default on for ungrouped random bots;
  off when `follow` pack is applied. Bag pressure routes to vendor hubs.
* [DONE] **Party quest-log sync**: when a real player accepts a quest, grouped
  bots with AI mirror it via `PlayerScript::OnQuestAdd` (`CanTake`/`CanAdd` →
  `AddQuest`) and stay on follow — no independent quest travel. TravelNode /
  quest graph still deferred.

## Reference tree

Upstream AC module checked into the repo root as `mod-playerbots/` (study /
command semantics). SkyFire runtime module remains `modules/mod-playerbots/`.
Do not compile the root tree into worldserver.

## Phase 4 - Feature testing targets (the reason for this port) (IN PROGRESS)

Merged the `lfg_mechanics` branch (LFG/LFR/scenario/challenge-mode work) into
`playerbots` so bots can use the existing dungeon finder. `lfg_mechanics` is kept
for future updates.

* [DONE] LFG auto-response: the master queues a party (normal client UI) and the
  bots auto-answer the group role check and accept the join proposal. Role check
  uses a deterministic, group-aware assignment (first tank-capable bot tanks,
  first healer-capable heals, rest dps) so a bot party forms a valid composition
  without communicating. Core exposes `LFGMgr::GetActiveProposalIdForPlayer` so
  the module can find a bot's pending proposal to accept. Solo LFG fill teleports
  bots via `LFGMgr::TeleportPlayer`, which finalizes bot worldports with
  `FinalizeBotTeleport` (no client ack). Leaving the dungeon guards `Group::Disband`
  against re-entry when bots finalize their exit teleports.
* [DONE] **Solo LFG fill** (`Playerbots.RandomBotJoinLfg=1`): ungrouped bots
  `JoinLfg` the same dungeons as queued real players, with console join logs.
  Excludes bot tanks when a player queued tank, and bot healers when a player
  queued healer. Bot-only LFG groups are still ejected.
* [DONE] Between-pull food: socket + self-bots sit only while food/drink auras
  are active (no empty-sit contagion); full allies stand and wait. Refreshment
  uses Food/Drink auras — no bag food/drink. Cancels at full resources.
* [DONE] In-dungeon behaviour: party holds follow between pulls until allies are
  almost ready (`Rest.AlmostFullHealth` / `Rest.MediumMana`); instance DPS sticks
  to the tank's victim; `+threat` throttles vs tank threat and pauses melee as well
  as spells. Boss/trash scripting still thin — depends on deeper rotations.
* [DONE] Loot rolls: bots auto Need/Greed/Pass on GROUP_LOOT / NEED_BEFORE_GREED
  (LFG). Corpse loot peels briefly when OOC with `nc +loot`.
* [TODO] LFR: bots fill raid finder queues (auto-accept the LFR prompt/roles).
* [TODO] Random battlegrounds / rated battlegrounds and arenas.
* [TODO] Open-world grinding population polish (TravelNode / full quest graph still
  deferred; same-map `nc +quests` + party quest-log sync while following are in).

## Phase 5 - Content data

* Bot equipment/talent templates appropriate to 5.4.8.
* SQL for any module-owned tables (kept under `sql/`, applied by your tooling).

## Notes on reuse

* The upstream AI is data/logic heavy but many *strategies* are conceptually
  portable; the class-specific *spell IDs and rotations* must be rebuilt for MoP.
* Keep core edits behind `ScriptMgr` hooks where possible so the core diff stays
  reviewable and this module remains self-contained.

### Rotation coverage

* [DONE] All 34 MoP specializations have combat (or heal) priority lists.
* Healer specs also have `*Dps` damage lines for `co +healer dps`.
* Trinkets sync with burst windows; Elemental/Assassination/Mage lines deepened
  (AoE, bombs, Ascendance Lava Beam, Marked for Death). Balance/Destro/Demo/
  Subtlety/MM/Arms/Feral also deepened (Incarnation, Havoc, Infernal/Doomguard,
  Hellfire, Vanish/MfD, Murder of Crows, Avatar/Skull Banner).
* [DONE] `.playerbots init` applies recommended major/minor glyphs per spec;
  `TryMaintainBuffs` keeps MotW/Fort/Brilliance/shouts/blessings/etc. up.
* [TODO] Further SimC-faithful tuning, glyph-aware rotation branches, and
  boss-specific holds.
