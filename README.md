<h1 align="center">Individual Leveling Progression (ILP)</h1>

<p align="center">
  <strong>Per-character, achievement-gated 1&nbsp;→&nbsp;60 for AzerothCore.</strong><br>
  A deliberate-pace vanilla climb designed to run alongside
  <a href="https://github.com/ZhengPeiRu21/mod-individual-progression">mod-individual-progression</a>.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0-blue.svg" alt="AGPL-3.0"></a>
  <img src="https://img.shields.io/badge/AzerothCore-WotLK%203.3.5a-red" alt="AC WotLK 3.3.5a">
  <img src="https://img.shields.io/badge/status-early%20public%20release-orange" alt="Status: early public release">
</p>

---

## What this is

ILP holds the pre-60 climb behind per-character requirements at each bracket
ceiling. You earn XP normally and fill the bar at the cap level; the level-up
into the next level is blocked until the phase's PvX requirements are met.
Once cleared, the next cap opens.

The design is IPP's own philosophy pushed down into 1&ndash;60 — a structured,
self-paced journey. **This is not Season of Discovery.** SoD gates were
time-based and server-wide. ILP gates are per-character, checked with the
hooks and bitmasks the server already tracks.

At level 60 ILP steps aside and IPP's tier system takes over.

## Status — early public release, honest about it

- All gates are wired and were validated end-to-end on a fresh-toon 1&nbsp;→&nbsp;60
  playthrough on a private dev server, including the IPP handoff at 60.
- **It has not yet been used by anyone other than the author.** No live-server
  playtesting yet, no third-party feedback yet, and the tuning values in the
  shipped `.conf.dist` are the author's opinionated defaults — expect to
  retune.
- Built as a personal project first (see [DESIGN.md](DESIGN.md) for the
  original intent). Publishing it because it seems solid enough that other
  people running IPP might get value out of it, not because it's polished.

Come find issues. That's what a fresh public release is for.

## The phase ladder

| Gate | Cap | PvP | Dungeons | First Aid | Other |
| --- | --- | --- | --- | --- | --- |
| 1 | 19 | 10 WSG | — | — | — |
| 2 | 29 | 10 AB | any 3 distinct (SFK / RFK / BFD / WC / VC / Stocks) | 75 | — |
| 3 | 39 | 10 any BG | full SM (4 wings) | 150 | reward: mount |
| 3.5 | 40 (explore only) | — | — | — | Gadgetzan FP + N full zones (any-N-from-list) |
| 4 | 49 | 10 any BG | Maraudon + Uldaman + ZF (Chief Ukorz kill) | 225 | — |
| Finale | 60 (attune gate) | 3 AV | BRD, LBRS, UBRS, Strat ×2, Scholo, DM ×3 | 300 | all 6 vanilla capitals → MC attune opens |

Values above are the shipped SPEC production thresholds. All are `.conf`-driven
and can be lowered for testing without a rebuild — see
[conf/mod_individual_leveling_progression.conf.dist](conf/mod_individual_leveling_progression.conf.dist)
for the full list.

## Composition with IPP

ILP is a **standalone companion** to mod-individual-progression, not a fork.

- ILP owns **tempo** (when the level cap advances).
- IPP owns **difficulty** (world power scaling, tier progression, restored vanilla content).
- Both cap XP through the same AC hook; when multiple modules cap XP, the
  effective cap is the minimum, so the two compose without coordination code.
- Below 60, ILP binds. At 60, ILP steps aside and IPP's tier gates take over.

IPP is a strong recommendation, not a hard dependency — ILP builds and runs
without it. But the intent is the two together.

## Install

Standard AzerothCore module install:

```bash
cd <ac-source>/modules
git clone https://github.com/Brave-Clown/mod-individual-leveling-progression.git
cd ..
mkdir -p build && cd build
cmake ..
make -j$(nproc)
sudo make install
```

CMake picks up the module automatically via the standard modules scan. Copy
`conf/mod_individual_leveling_progression.conf.dist` to your `etc/modules/`
directory as `mod_individual_leveling_progression.conf` and edit as needed.

**Requirements:**

- **AzerothCore WotLK 3.3.5a.** Tested against the [mod-playerbots AC fork](https://github.com/mod-playerbots/azerothcore-wotlk)
  on the `Playerbot` branch. Should build on upstream AC too, but is not
  actively tested there.
- **`EnablePlayerSettings = 1`** in `worldserver.conf` — ILP stores per-character
  state through the PlayerSettings mechanism (same as IPP).
- **Optional:** [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
  (or an actively-maintained fork such as
  [Grimfeather/mod-individual-progression](https://github.com/Grimfeather/mod-individual-progression))
  installed alongside for the intended full-journey experience.

## Runtime commands

- `.ilp status [player]` — full readout of the current gate, every requirement,
  and per-slot tick lines for multi-bit pillars.
- `.ilp reload` — re-read the conf without a worldserver restart.
- `.ilp set journey <0|1> [player]` — toggle the per-character journey flag.
- `.ilp skip [player]` — mark a character progression-complete
  (two-step confirmation, GM-restricted; for utility alts).
- `.ilp dev xp|bg|dungeon|capital [player]` — dev/GM test helpers so gates can
  be validated without grinding real BGs and dungeons every time.

## Hooks used

For AzerothCore catalogue reference:

- **WorldScript:** `OnAfterConfigLoad`, `OnBeforeWorldInitialized`
- **PlayerScript:** `OnPlayerLogin`, `OnPlayerGiveXP`, `OnPlayerCreatureKill`,
  `OnPlayerUpdateZone`, `OnPlayerMapChanged`, `OnPlayerUpdateSkill`
- **UnitScript:** `OnUnitDeath` (backstop for kills where the killer isn't a Player
  — e.g. warlock/hunter pets landing the final blow)
- **CreatureScript:** Lothos Riftwaker gossip gate (MC attunement)
- **BGScript:** `OnBattlegroundEnd`
- **CommandScript:** `.ilp` command tree

CMake hooks: none custom — standard AC modules build.

SQL patches: two, in `data/sql/world/` and `data/sql/characters/`. Applied
manually via the standard AC updater on module load.

## Bots

**All bots are exempt from ILP** — no XP cap, no credit tracking, no journey
flag. Bot population and level distribution are governed entirely by
[playerbots](https://github.com/mod-playerbots/mod-playerbots) config. See
[DESIGN.md §5](DESIGN.md) for the rationale.

## Configuration

The full config lives in
[conf/mod_individual_leveling_progression.conf.dist](conf/mod_individual_leveling_progression.conf.dist).
Every threshold, every dungeon/creature list, every capital and flight-path
list is a config value — nothing is hardcoded. That means you can retune the
entire feel of the module without rebuilding.

Notable levers:

- **Category toggles** (`Require.PvP`, `Require.Dungeons`, `Require.FirstAid`,
  `Require.Exploration`) — bulk-disable a whole pillar across every gate.
  Useful for a "kids' mode" that skips PvP entirely.
- **Per-gate `*Required` counts** — the numeric threshold for each pillar.
- **Boss/creature entry lists** — treat as **append-only**. The bit position of
  each entry in the list is the bit position used in the stored per-character
  bitmask; reordering silently invalidates every player's progress.

## Credits

Authored by [Brave-Clown](https://github.com/Brave-Clown), with heavy use of
[Claude Code](https://www.anthropic.com/claude-code) as a pair-programmer.
The design (see [DESIGN.md](DESIGN.md)) is my own; substantial portions of the
implementation are Claude's work under my direction, and commits use a
`Co-Authored-By: Claude` trailer for transparency. This is vibe-coding-plus at
best — read the diffs before you trust the code with your server.

Design inspiration and companion module: [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
by ZhengPeiRu21 and its ongoing fork by [Grimfeather](https://github.com/Grimfeather/mod-individual-progression).

Built on top of [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk).

## License

[AGPL-3.0](LICENSE). Following the AzerothCore catalogue recommendation.
