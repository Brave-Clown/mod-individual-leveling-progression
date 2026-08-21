# DESIGN — Individual Leveling Progression (`mod-individual-leveling-progression`)

> This is the original design intent, written before implementation began. Read it
> for the reasoning behind the design. See [README.md](README.md) for what actually
> shipped, and §11 (Implementation Notes) for where the shipped code differs from the
> intent below and why.

> An AzerothCore companion module that gates the pre-60 climb behind PvX requirements,
> running alongside [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression) (IPP).
> This is the implementation brief. Items marked TBD are placeholders for the
> maintainer to fill; everything else is settled.

---

## 1. Concept & Philosophy

- Per-character, achievement-gated leveling. The player engages with each tier of vanilla content on the way up, and the level cap advances only once a phase's requirements are met.
- This is not Season of Discovery. SoD gates were time-based and server-wide. This applies IPP's philosophy to the 1–60 climb: a structured, self-paced journey.
- The goal is a deliberate-pace vanilla experience for the maintainer and, optionally, his kids.

---

## 2. Architecture

- Standalone companion module, not a fork of IPP.
- Composes with IPP through the XP-gain hook. When several modules cap XP, the effective cap is the lowest of them. Below 60 this module binds; at 60 it releases and IPP's tier system takes over. No coordination code is needed.
- State lives in Player Settings (per-character), the same mechanism IPP uses. Requires `EnablePlayerSettings = 1` in `worldserver.conf`.
- The cap bites at the ding boundary. The player earns XP normally and fills the bar at the cap level, but the level-up into the next level is blocked until requirements are met. This mirrors how IPP holds a character at 60.
- All combat and world power scaling stays with IPP. This module never touches difficulty: IPP owns difficulty, this module owns tempo. IPP's optional vanilla power adjustment, if enabled, applies during leveling too, so the two compose cleanly.
- Config and DB driven. All tunable values (caps, requirement counts, dungeon lists, flight-path lists, zone lists, First Aid thresholds) live in config, so they can be retuned without recompiling. That helps both fast iteration and release.

---

## 3. Hooks & Detection

| Requirement | Mechanism |
| --- | --- |
| Level cap | player-gives-XP hook; zero XP at the ceiling until the phase clears |
| BG completions | battleground-end hook; count games played to completion (not wins), bracket-scoped |
| Dungeon clears | creature-kill hook; match against the phase's end-boss creature entries |
| First Aid skill | `GetSkillValue(SKILL_FIRST_AID)`; trivial, no hook needed |
| Capital reached | explored-zones sub-area bitmask; "set foot" means the city's area bit is lit |
| Full-zone exploration | the zone's `Explore X Zone` achievement criteria (Blizzard's curated areatable bits, not raw `AreaTable.dbc` rows) are all satisfied in the player's per-character explored-zones bitmask. Read directly off `PLAYER_EXPLORED_ZONES_1`, never via the achievement system, so account-shared achievements (`mod-account-achievements`) cannot pre-credit alts |
| Flight-path discovery | taxi-node bitmask (server-side) |
| MC attunement gate | reuse IPP's conditional NPC/GameObject visibility (see §6) |

Everything in the design is detectable with these. The exploration system in particular can tell a partially explored zone from a fully explored one, because exploration is tracked per sub-area, not per zone.

---

## 4. The Phase Ladder

Caps sit on bracket ceilings (19 / 29 / 39 / 49) so the required PvP for each phase is always done from the top of a bracket, the strongest position. Two threads ramp across the whole climb (PvP follows BG unlock order; First Aid climbs steadily), plus signature gates with their own identity.

### Cap 19 → 20 — PvP intro

- PvP: 10 Warsong Gulch matches (10–19 bracket).

### Cap 29 → 30 — group up

- PvP: 10 Arathi Basin matches (20–29 bracket).
- Dungeons: clear any 3 distinct from Shadowfang Keep, Razorfen Kraul, Blackfathom Deeps, Wailing Caverns, The Deadmines (VC), The Stockades.
- First Aid: 75.

> The 3-distinct-dungeon rule doubles as the grouping requirement (no separate group quest needed) and solves faction distance for free, since each side runs whatever is nearby.

### Cap 39 → 40 — the halfway hump (reward: ding 40 + riding skill + a mount)

- PvP: 10 any battleground (30–39 bracket).
- Dungeon: full Scarlet Monastery, all four wings. End bosses: Graveyard (Bloodmage Thalnos), Library (Arcanist Doan), Armory (Herod), Cathedral (Scarlet Commander Mograine and High Inquisitor Whitemane).
- First Aid: 150.

### Speed-bump at 40 — the world opens (exploration only; held at 40 until cleared)

This is a deliberate non-ceiling cap. It carries no PvP requirement, so the bracket-floor problem never applies: exploring at 40 is no harder than at 49. It ties to the mount the player just earned.

- Flight path: Gadgetzan only (mandatory, shared). Kept simple on purpose: no per-faction far landmark, no breadth requirement.
- Full-zone exploration: any 6 fully explored zones from a shared candidate list (production target; tunable). The idea is that players naturally over-explore the zones they are already passing through during 10–40 leveling, so requiring 6 from a generous list nudges them to finish what they have already been in rather than forcing trips to new far zones. It is a soft nudge, not a wall. The list is faction-neutral (anyone can explore any zone the engine lets them survive in); friendly-faction bias is enforced naturally by the level and guard mismatch, not by the module.
  - Mechanic: a zone counts as fully explored when its `Explore X Zone` achievement criteria are all satisfied, read out of the player's per-character explored bitmask (see §3). Account-shared achievements via `mod-account-achievements` do not pre-credit alts; every character has to do the walk.
  - Candidate list: shipped in the conf as vanilla zone-explore achievement IDs (see `IndividualLevelingProgression.Cap40.FullZoneAchievementIds`). Edit the list or the `FullZonesRequired` count at runtime and reload with `.ilp reload`. The shipped list covers all 40 vanilla Azeroth zones (see §11).

### Cap 49 → 50 — the last major hump (intentionally heavy)

- PvP: 10 any battleground (40–49 bracket).
- Dungeons: Maraudon + Uldaman.
- Group/world quest: the Zul'Farrak Mallet chain. Sacred Mallet, then trek to the Hinterlands to forge the Mallet of Zul'Farrak, then use it to summon Gahz'rilla in ZF (which brings a ZF run along with it). Exact quest steps to be confirmed during implementation.
- First Aid: 225 (the cap right before the Triage quest).

> Uldaman is in Badlands and ZF ties to the Hinterlands, so there is natural overlap with the exploration pillar if Badlands or Hinterlands are chosen as explore zones.

### Finale — earn the raid (level 60; not a level cap, purely the MC attunement gate)

The player is a full 60 throughout. The only thing this module still holds shut is the Molten Core attunement.

- Dungeon tour (clear each): Blackrock Depths, Lower Blackrock Spire, Upper Blackrock Spire (which drags in the Seal of Ascension attune chain), Stratholme (both Undead and Live sides), Scholomance, Dire Maul (all three: N/E/W).
- PvP: 3 Alterac Valley completions (IPP's restored vanilla AV). Bump to 4 if desired.
- Exploration: all six vanilla capitals reached (set foot): Stormwind, Ironforge, Darnassus, Orgrimmar, Thunder Bluff, Undercity. Reaching the enemy faction's three is the rite of passage. BC capitals (Exodar, Silvermoon) are not required, even though IPP keeps those zones.
- First Aid: 300. Requiring 300 bundles the Triage quest automatically, since vanilla will not let you past 225 without it.

When all finale items are complete, the MC attunement opens, this module steps fully aside, and IPP tier progression is the only thing gating the character from then on.

---

## 5. Bots & Multiplayer Rules

- All bots exempt (revised 2026-06-08 from the original "rndbots exempt, altbots gated" split). Bot population and level distribution are governed entirely by `playerbots.conf` knobs (`AiPlayerbot.RandomBotMinLevel`/`MaxLevel`, `RandomBotXPRate`, `RandomBotInWorldTime`, and so on), not by ILP. ILP does not touch bots at all: no XP cap, no credit tracking, no journey flag.
- Why the revision: the original SPEC's altbot-shared-credit design assumed altbots could fall behind their master without playerbots' auto-level-follow. In practice playerbots already keeps altbots near the master's level, so altbots are not a problem without ILP intervention. And the maintainer's goal is to be behind the bot pack rather than leading it, which just requires turning ILP off for bots rather than gating them.
- Shared credit for real characters still applies: a group of journey-on real characters all get credit for a shared event (BG finished, or boss killed within the group's reward distance). This covers solo, co-op, and kids on separate accounts.
- Implementation: detect "is bot" via the playerbots API and early-return at the top of every ILP hook (the XP cap and every credit function). One helper, called everywhere.

---

## 6. MC Attunement Gating

Primary mechanism (preferred, since it is diegetic and reuses IPP's own pattern): condition the attunement step itself on the finale flags. The Core Fragment / Lothos Riftwaker does not offer the attunement until the finale checklist is green, the same way IPP shows and hides content by progression.

Backstop: a lightweight door-check on the Molten Core map entrance for edge cases (GM ports, already-attuned characters, summons).

---

## 7. Journey Flag & Skip Command

- Per-character journey on/off flag.
- A GM command, with two-step confirmation, to mark a character progression-complete. This is for utility alts that were command-leveled to 60 and need to raid immediately without grinding the journey.
- Restricted to GM security level, so a released build does not let regular players self-skip; admins grant exceptions where they want.
- Deliberately not driven by mod-account-achievements, which would blanket-complete every alt on the account without a per-alt choice.

---

## 8. Player-Facing Feedback

- A `.progression`-style command and whisper that prints the current cap, the current phase, and each requirement with a live counter (for example `Warsong Gulch: 6/10`).
- Auto-whispers on hitting a cap and on each requirement completion.
- Verbose mode is the default (needed for the kids). An optional cryptic mode is just a config flag that swaps the message text for vague flavor hints; it touches no gate logic, so it can be added whenever.
- A custom NPC with a gossip menu is a nice later polish, not needed for v1.

---

## 9. Build Notes / Conventions

- Use IPP's repo skeleton as the template (`conf/`, `src/`, `data/sql/`, the CMake glue / `include.sh`) and gut it down to scaffold.
- Pin AzerothCore and mod-playerbots to specific, matching commits.
- Use a disposable dev LXC, reusing existing extracted map/vmap/mmap data. Never build on the live server.
- Git from line one, in its own repo.
- Build the debug/inspection command and a low-requirement test config first, before the gates themselves. It turns multi-minute verifications into seconds.
- Keep every tunable value in config (see §2).
- Note: AzerothCore ships the WotLK LFG dungeon finder by default. Whether IPP disables it for the vanilla tier is unconfirmed and worth checking against IPP's config/changes list. It does not affect this module's detection (boss kills count however the group formed). Disabling LFG for an authentic travel-and-manual-group feel is a separate, optional toggle.

---

## 10. Open Placeholders (maintainer's call — none block the build)

1. AV finale count is set to 3; bump to 4 if desired.
2. Cap 40 zone count. `FullZonesRequired` ships at 6 (the production target). The candidate list ships as all 40 vanilla Azeroth zones (see §11); trim or expand it after a full run-through shows what felt right.

---

## Locked Summary Table

| Gate | Cap | PvP | Dungeons | First Aid | Other |
| --- | --- | --- | --- | --- | --- |
| 1   | 19  | 10 WSG | —   | —   | —   |
| 2   | 29  | 10 AB | any 3 distinct (low pool) | 75  | —   |
| 3   | 39  | 10 any BG | full SM (4 wings) | 150 | reward: mount |
| 3.5 | 40 (explore only) | —   | —   | —   | Gadgetzan FP + ~6 full zones (faction-neutral, achievement-criterion check) |
| 4   | 49  | 10 any BG | Maraudon + Uldaman + ZF (Mallet) | 225 | —   |
| Finale | 60 (attune gate) | 3 AV | BRD, LBRS, UBRS, Strat ×2, Scholo, DM ×3 | 300 | all 6 capitals → MC attune opens |

---

## 11. Implementation Notes

The design above froze before implementation. This section records where the
shipped code diverges from it, and why. It is kept as an appendix rather than
rewriting §1–§10, so the original thinking stays intact.

### §4 Cap 49 — Zul'Farrak final: Chief Ukorz kill, not the Mallet chain

**Design said:** the Sacred Mallet to Mallet of Zul'Farrak chain, summoning
Gahz'rilla.

**Shipped:** a Chief Ukorz Sandscalp (creature entry `7267`) kill at the end of
ZF. The `Cap49.MalletQuestId` conf option, the synthesized "carrot" bit, and the
mallet branch in `Cap49DungeonsDone` were all removed.

**Why:** IPP rewrites the Carrot on a Stick acquisition path, which breaks the
Mallet chain in ways that vary by IPP version. Ukorz is the canonical ZF end
boss, has no quest dependency, and behaves the same regardless of IPP's tier
state. A shared-dependency test found this cleanly; see the Cap 49 slot in
`Cap49DungeonBossIds` in the shipped `.conf.dist`.

### §4 Cap 40 — full-zone list expanded to all 40 vanilla Azeroth zones

**Design said:** "any 6 fully explored zones from a shared candidate list …
~30 vanilla zones the player would plausibly pass through 1–40."

**Shipped:** `IndividualLevelingProgression.Cap40.FullZoneAchievementIds`
enumerates all 40 vanilla Azeroth zones, not ~30. `FullZonesRequired` still ships
at the SPEC production target.

**Why:** a real playthrough found the original ~30-zone list excluded zones a
player naturally explores by 40 (for example Winterspring or Eastern Plaguelands
in some pathings). Expanding to the full 40 keeps the "finish what you're already
in" idea and gives the player more valid credit surface. It stays faction-neutral,
as designed.

### Missing hook — pet kills didn't credit the master

**Design said:** dungeon clears use the creature-kill hook, matching against
end-boss entries.

**Shipped:** in addition to `PlayerScript::OnPlayerCreatureKill`, ILP subscribes
to `UnitScript::OnUnitDeath` and resolves credit via `Creature::GetLootRecipient`.

**Why:** AzerothCore's `OnPlayerCreatureKill` only fires when the killer is a
`Player`. A warlock or hunter pet landing the finishing blow on a boss silently
skipped the hook, leaving the boss uncredited. This was confirmed in a dev
playtest at Wailing Caverns and Blackfathom Deeps at Cap 29. The `OnUnitDeath`
backstop resolves the intended real-player credit via loot recipient and calls the
same `CreditDungeonBoss` path.

### Startup ordering — DBC not loaded at `OnAfterConfigLoad`

**Design said (implicitly):** compute the explore-achievement lookup tables at
config-load time.

**Shipped:** `RefreshExploreData()` runs on both `OnAfterConfigLoad` and
`OnBeforeWorldInitialized`.

**Why:** `World::SetInitialWorldSettings` fires `OnAfterConfigLoad` before
`LoadDBCStores`. The first pass therefore walks an empty
`sAchievementCriteriaStore`, and every Cap 40 zone counter comes back as `0` for
every player. A second pass on `OnBeforeWorldInitialized` runs post-DBC-load and
populates the tables correctly. The bug only surfaced when `.reload config` did
not happen to fire between startup and the first real query; the original 1→60
playtest survived it by accident.

### Journey completion flip — `OnPlayerUpdateSkill` closes the Finale

**Design said (§7):** journey-on/off flag and a GM skip command; nothing about the
auto-complete moment.

**Shipped:** `SETTING_COMPLETE` is flipped by `CheckJourneyComplete`, invoked
opportunistically from several hooks, including `OnPlayerUpdateSkill`.

**Why:** if the last outstanding Finale requirement is First Aid 300, hitting that
skill value has no ILP-owned hook. Without the `OnPlayerUpdateSkill` subscription,
the character would stay "not complete" until the next login or credit event. The
hook is idempotent and safe to call from anywhere, and it guarantees the transition
happens the instant the last requirement lands.

### Multi-level XP clamp — one grant, one gate

**Design said:** the cap bites at the ding boundary; XP fills the bar and the
level-up into the next level is blocked.

**Shipped:** the clamp targets the first unsatisfied gate, not just the current
one, so a single XP grant that would otherwise ding through multiple cap
boundaries stops at the first one the character has not earned.

**Why:** GM `.xp` grants and IPP's own bonus-XP mechanics can occasionally deliver
a single XP payload that spans multiple levels. A naive clamp against the current
gate would let one payload ding a character from 18 to 22, skipping the Cap 19 gate
entirely. The clamp now walks forward and stops at the earliest gate not yet
cleared.

### Bots — final rule is "all bots exempt", per §5 revision

The §5 revision landed as designed. It is flagged here only because the section
was rewritten mid-project. The original design was "rndbots exempt, altbots
gated"; the shipped rule is all bots exempt. In code this is a single `IsBotOwned`
helper called at the top of every ILP hook. The rationale is in §5.

---

## 12. Future ideas — a possible TBC tier (61–70)

Nothing in this section is built or committed. It is a place to keep design
thoughts for a possible Burning Crusade tier so they are not lost. It assumes the
TBC content itself is present (for example through the IPP fork's TBC progression);
ILP would sit on top and gate tempo the same way it does at 1–60.

Shape of the idea:

- **No level-70 cap.** The tier would gate the 61–69 climb and then release at 70.
  The Karazhan attunement chain is a hard enough gate on its own, so the module does
  not need to add a 70 ceiling. This mirrors the vanilla finale, where the last gate
  is an attunement rather than a level cap.
- **Dungeons:** clear some of the Outland leveling 5-mans. Candidate pool: Hellfire
  Ramparts, The Blood Furnace, The Slave Pens, The Underbog, Mana-Tombs, Auchenai
  Crypts, Sethekk Halls, Old Hillsbrad Foothills. Same "any N distinct" pattern as
  the vanilla Cap 29 dungeon pool.
- **Flight paths:** discover a set of Outland flight paths, using the same
  taxi-bitmask mechanic already used for Gadgetzan at Cap 40.
- **Signature quest:** the Nagrand Ring of Blood quest chain (level ~65), a series of
  arena fights that ends with Mogor the Ogre. A distinct, memorable gate in the spirit
  of the Zul'Farrak and Scarlet Monastery gates. Exact quest and creature IDs to be
  confirmed during implementation.
- **World PvP:** capture some Outland world-PvP objectives instead of instanced
  battlegrounds. Candidates: the Hellfire Peninsula towers (Overlook, Stadium,
  Broken Hill) and the Zangarmarsh beacons (Twin Spire Ruins). This gives the TBC
  tier its own PvP identity, distinct from the vanilla BG gates. Detection would need
  an OutdoorPvP or objective-capture hook, to be confirmed.

Open questions for whenever this is picked up:

- Which specific dungeons, and how many.
- How many flight paths, and which ones.
- Whether world-PvP objective capture is cleanly detectable through an AC hook.
- Whether a profession thread (First Aid, or another) should keep climbing through 70,
  or whether the vanilla thread simply ends at 300.
