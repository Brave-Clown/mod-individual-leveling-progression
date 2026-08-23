# Individual Leveling Progression (ILP)

Per-character, achievement-gated 1–60 leveling for AzerothCore. The level cap
rises as you take part in the world (battlegrounds, dungeons, exploration,
professions), so the climb only slows down if you skip past it. Built to run
alongside
[mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
(IPP).

[![License: AGPL-3.0](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)
![AzerothCore WotLK 3.3.5a](https://img.shields.io/badge/AzerothCore-WotLK%203.3.5a-red)

## Overview

ILP holds the level cap at each bracket ceiling (19, 29, 39, 49) until the
character has met that bracket's requirements. You earn XP normally and fill
the bar at the cap level; only the level-up into the next level is blocked.
Once the requirements are met, the cap opens and leveling continues.

The requirements do not have to be finished at the cap; a player can complete
them at any point on the way up. The point is engagement rather than delay.
Someone who runs battlegrounds, clears the listed dungeons, and levels First Aid
as they go will reach each cap with the work already done and never actually be
stopped. Someone who rushes straight up on XP alone is held at the ceiling until
they go back and do it. Either way the cap opens on what you have done in the
world, and time alone never moves it.

This is not Season of Discovery. SoD gates were time-based and server-wide; ILP
gates are per-character and checked against state the server already tracks: XP,
battleground results, boss kills, exploration and taxi bitmasks, and skill
values.

At level 60 the level gating ends. The only thing ILP still holds shut is the
Molten Core attunement, which opens once the finale requirements are met. From
there IPP's tier system takes over.

## Status

Every gate is implemented and was validated end-to-end on a fresh 1–60
playthrough on a private dev server, including the handoff to IPP at 60. It has
not been tested on a live server or by anyone other than the author, and the
values in the shipped `.conf.dist` are the author's defaults, so expect to
retune for your server. Bug reports are welcome.

## Phase ladder

| Gate | Cap | PvP | Dungeons | First Aid | Other |
| --- | --- | --- | --- | --- | --- |
| 1 | 19 | 10 WSG | — | — | — |
| 2 | 29 | 10 AB | any 3 distinct (SFK / RFK / BFD / WC / VC / Stocks) | 75 | — |
| 3 | 39 | 10 any BG | full SM (4 wings) | 150 | reward: mount |
| 3.5 | 40 (explore only) | — | — | — | Gadgetzan flight path + 6 full zones (any 6 from list) |
| 4 | 49 | 10 any BG | Maraudon + Uldaman + ZF (Chief Ukorz kill) | 225 | — |
| Finale | 60 (attune gate) | 3 AV | BRD, LBRS, UBRS, Strat ×2, Scholo, DM ×3 | 300 | all 6 vanilla capitals → MC attune opens |

Every threshold above is a config value and can be lowered for testing without
a rebuild. See
[conf/mod_individual_leveling_progression.conf.dist](conf/mod_individual_leveling_progression.conf.dist)
for the full list.

## Composition with IPP

ILP is a standalone companion to mod-individual-progression, not a fork.

- ILP controls tempo: when the level cap advances.
- IPP controls difficulty: world power scaling, tier progression, and restored
  vanilla content.
- Both cap XP through the same AzerothCore hook. When more than one module caps
  XP, the effective cap is the lowest of them, so the two work together with no
  coordination code.
- Below 60, ILP does the gating. At 60 it steps aside and IPP's tier gates take
  over.

IPP is recommended but not required. ILP builds and runs without it; the two
together are the intended setup.

## Requirements

- AzerothCore WotLK 3.3.5a. Tested against the
  [mod-playerbots AC fork](https://github.com/mod-playerbots/azerothcore-wotlk)
  on the `Playerbot` branch. It should build on upstream AzerothCore as well,
  but that is not actively tested.
- `EnablePlayerSettings = 1` in `worldserver.conf`. ILP stores per-character
  state through the PlayerSettings mechanism, the same as IPP.
- Optional:
  [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression),
  or an actively maintained fork such as
  [Grimfeather/mod-individual-progression](https://github.com/Grimfeather/mod-individual-progression),
  installed alongside for the full-journey experience.

## Installation

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

CMake picks up the module through the standard modules scan. Copy
`conf/mod_individual_leveling_progression.conf.dist` into your `etc/modules/`
directory as `mod_individual_leveling_progression.conf` and edit as needed.

## Configuration

The full config is in
[conf/mod_individual_leveling_progression.conf.dist](conf/mod_individual_leveling_progression.conf.dist).
Every threshold, dungeon and creature list, capital, and flight-path list is a
config value; nothing is hardcoded. You can retune the whole module without
rebuilding.

Main levers:

- Category toggles (`Require.PvP`, `Require.Dungeons`, `Require.FirstAid`,
  `Require.Exploration`) disable a whole pillar across every gate. Useful for a
  kids' mode that skips PvP.
- Per-gate `*Required` counts set the numeric threshold for each pillar.
- Boss and creature entry lists are append-only. The position of each entry in
  the list is the bit position used in the stored per-character bitmask, so
  reordering silently invalidates existing progress.

## Runtime commands

- `.ilp status [player]` — current gate, every requirement, and per-slot lines
  for multi-bit pillars.
- `.ilp reload` — re-read the conf without a worldserver restart.
- `.ilp set journey <0|1> [player]` — toggle the per-character journey flag.
- `.ilp skip [player]` — mark a character progression-complete (two-step
  confirmation, GM-restricted; for utility alts).
- `.ilp dev xp|bg|dungeon|capital [player]` — GM test helpers to exercise gates
  without grinding real battlegrounds and dungeons.

## Hooks used

For AzerothCore catalogue reference:

- WorldScript: `OnAfterConfigLoad`, `OnBeforeWorldInitialized`
- PlayerScript: `OnPlayerLogin`, `OnPlayerGiveXP`, `OnPlayerCreatureKill`,
  `OnPlayerUpdateZone`, `OnPlayerMapChanged`, `OnPlayerUpdateSkill`
- UnitScript: `OnUnitDeath` (backstop for kills where the killer is not a
  Player, e.g. a warlock or hunter pet landing the final blow)
- CreatureScript: Lothos Riftwaker gossip gate (MC attunement)
- BGScript: `OnBattlegroundEnd`
- CommandScript: the `.ilp` command tree

No custom CMake; standard AzerothCore module build. Two SQL patches ship in
`data/sql/world/` and `data/sql/characters/`, applied by the standard AC updater
on module load.

## Bots

All bots are exempt from ILP: no XP cap, no credit tracking, no journey flag.
Bot population and level distribution are governed entirely by
[playerbots](https://github.com/mod-playerbots/mod-playerbots) config. See
[DESIGN.md](DESIGN.md) §5 for the reasoning.

## Roadmap

The module currently covers the 1–60 vanilla climb. A Burning Crusade tier (61–70)
is an idea under consideration, not a commitment. The current thinking: gate the
Outland climb with leveling-dungeon clears, Outland flight-path discovery, the Nagrand
"Ring of Blood" quest chain, and a couple of Outland world-PvP objectives,
with no level-70 cap (the Karazhan attunement is gate enough). See DESIGN.md §12 for
details.

A second idea under consideration is configurable profession requirements: today
First Aid is the only profession pillar and it is hardcoded, but the same tracking
could be opened up so a server can require any profession, or a chosen number of
primaries and secondaries, from a preset shape down to a fully custom list. See
DESIGN.md §13.

Feedback and ideas are welcome via issues.

## Credits

Design and direction by [Brave-Clown](https://github.com/Brave-Clown). Much of
the implementation was written with
[Claude Code](https://www.anthropic.com/claude-code) as a coding assistant,
under the author's direction; commits carry a `Co-Authored-By: Claude` trailer.
Read the code before running it on your server.

Inspired by and built to run with
[mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
by ZhengPeiRu21, and its
[Grimfeather fork](https://github.com/Grimfeather/mod-individual-progression).
Built on [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk).

## License

[AGPL-3.0](LICENSE), following the AzerothCore catalogue recommendation.
