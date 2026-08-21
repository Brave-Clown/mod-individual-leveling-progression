#ifndef MOD_INDIVIDUAL_LEVELING_PROGRESSION_H
#define MOD_INDIVIDUAL_LEVELING_PROGRESSION_H

#include <cstdint>
#include <string>

class Player;

namespace ILP
{
    extern std::string const SETTINGS_SOURCE;

    // PlayerSettings indexes. 0 always means "unset" for flag-style settings, so
    // SetJourney(false) writes 2 (not 0) to distinguish "explicitly off" from "untouched".
    enum SettingIndex : std::uint32_t
    {
        SETTING_JOURNEY         = 0,  // 0=unset, 1=on, 2=off
        SETTING_COMPLETE        = 1,  // 0=not complete, 1=complete
        SETTING_SKIP_PENDING_T  = 2,  // unix timestamp of pending skip confirm (60s window)
        SETTING_WSG_COUNT       = 3,  // Cap 19 — WSG completions (played to end)
        SETTING_AB_COUNT        = 4,  // Cap 29 — AB completions
        SETTING_CAP39_BG_COUNT  = 5,  // Cap 39 — any BG completions at this gate
        SETTING_CAP49_BG_COUNT  = 6,  // Cap 49 — any BG completions at this gate
        SETTING_AV_COUNT        = 7,  // Finale — AV completions

        // Per-gate distinct-dungeon bitmasks. Bit position = position of that
        // boss's entry id in the gate's BossIds config list, so adding a boss
        // means appending to the list (never reordering).
        SETTING_CAP29_DUNGEON_MASK  = 8,  // Cap 29 — 6 dungeons
        SETTING_CAP39_SM_MASK       = 9,  // Cap 39 — 4 SM wings
        SETTING_CAP49_DUNGEON_MASK  = 10, // Cap 49 — 3 dungeons (Maraudon/Uldaman/ZF)
        SETTING_FINALE_DUNGEON_MASK = 11, // Finale — 9 dungeons (BRD/LBRS/UBRS/Strat ×2/Scholo/DM ×3)
        SETTING_CAPITALS_MASK       = 12, // Finale — 6 capitals (SW/IF/Dar/Org/TB/UC); bit position = list index
    };

    // Gate currently binding a character. GATE_COMPLETE means progression is finished
    // and the module no longer touches the character.
    enum Gate : std::uint8_t
    {
        GATE_CAP_19      = 0,
        GATE_CAP_29      = 1,
        GATE_CAP_39      = 2,
        GATE_CAP_40_BUMP = 3,
        GATE_CAP_49      = 4,
        GATE_FINALE      = 5,
        GATE_COMPLETE    = 6,
    };

    struct Config
    {
        bool          enable         = true;
        bool          announce       = true;
        bool          debug          = true;
        bool          journeyDefault = true;

        // Bulk-disable a whole requirement category across every gate.
        // Defaults match SPEC; flip to 0 to skip that category everywhere.
        bool          requirePvP         = true;
        bool          requireDungeons    = true;
        bool          requireFirstAid    = true;
        bool          requireExploration = true;

        std::uint32_t cap19_wsg         = 1;
        std::uint32_t cap29_ab          = 1;
        std::uint32_t cap29_dungeons    = 1;
        std::uint32_t cap29_firstAid    = 25;
        std::uint32_t cap39_bg          = 1;
        std::uint32_t cap39_smWings     = 1;
        std::uint32_t cap39_firstAid    = 50;
        std::uint32_t cap40_flightPaths = 1;
        std::uint32_t cap40_fullZones   = 6;
        std::uint32_t cap49_bg          = 1;
        std::uint32_t cap49_dungeons    = 1;
        std::uint32_t cap49_firstAid    = 75;
        std::uint32_t finale_av         = 3;
        std::uint32_t finale_dungeons   = 1;
        std::uint32_t finale_capitals   = 1;
        std::uint32_t finale_firstAid   = 100;
    };

    Config&       Cfg();
    void          LoadConfig();
    void          RefreshExploreData();

    bool          IsJourney(Player* p);
    void          SetJourney(Player* p, bool on);
    bool          IsComplete(Player* p);
    void          SetComplete(Player* p, bool on);

    Gate          CurrentGate(Player* p);
    char const*   GateName(Gate g);

    // 0 = no XP cap for this gate (finale, complete).
    std::uint8_t  CapLevelFor(Gate g);
    bool          GateSatisfied(Player* p, Gate g);

    // Increment the BG counter for the player's current gate (WSG / AB / Cap39-BG /
    // Cap49-BG / AV), notify on progress, and announce gate-release when this credit
    // pushes the whole gate into satisfied state. No-op for gates without a BG req.
    void          CreditCurrentGateBG(Player* p);

    // Set the matching dungeon bit for `p` if `creatureEntry` is a tracked end-boss
    // for `p`'s current gate. No-op if the entry isn't tracked, the bit's already
    // set, or the player isn't on a journey. Used by OnCreatureKill (for the killer
    // and group members) and by the `.ilp dev dungeon` test helper.
    void          CreditDungeonBoss(Player* p, std::uint32_t creatureEntry);

    // Set the capital bit for `p` if `zoneId` matches a tracked capital. Called
    // from OnPlayerUpdateZone (and the `.ilp dev capital` test helper).
    // No-op if the zone isn't tracked, the bit's already set, or the player isn't
    // on a journey. Doesn't require the player to be at the Finale gate — it's
    // fine to discover capitals early and have them banked for later.
    void          CreditCapitalVisit(Player* p, std::uint32_t zoneId);

    bool          SkipPendingValid(Player* p);
    void          SkipPendingSet(Player* p);
    void          SkipPendingClear(Player* p);

    // Wipe every ILP-owned PlayerSetting for `p` (journey, complete, skip-pending,
    // all BG counters, all dungeon masks). Journey snaps back to the config
    // default; gate snaps back to whatever the current level dictates.
    void          ResetProgress(Player* p);

    // If `p` is on a journey, currently at GATE_FINALE, and the Finale gate is
    // satisfied, flip SETTING_COMPLETE and announce. Idempotent — safe to call
    // from every credit hook and at login (covers passive paths like First Aid
    // skill-ups that no ILP hook sees). Once complete, CurrentGate returns
    // GATE_COMPLETE and the module stops touching the character.
    void          CheckJourneyComplete(Player* p);

    // SPEC §6 secondary: backstop on the MC map entrance for edge cases that
    // bypass Lothos (GM ports, warlock summons, characters attuned before the
    // module was installed, or characters who logged out inside MC). When `p`
    // is on a journey and not yet IsComplete and currently on map 409, send a
    // refusal line and teleport them to their homebind. No-op otherwise.
    void          EnforceMCBackstop(Player* p);

    // Counters. Wired in stages; first cut returns 0 for everything except trivial
    // lookups (First Aid). Computed-on-demand stays cheap enough to call from a
    // .ilp status handler.
    std::uint32_t WSGCompleted(Player* p);
    std::uint32_t ABCompleted(Player* p);
    std::uint32_t Cap39AnyBGCompleted(Player* p);
    std::uint32_t Cap49AnyBGCompleted(Player* p);
    std::uint32_t AVCompleted(Player* p);
    std::uint32_t Cap29DungeonsDone(Player* p);
    std::uint32_t Cap39SMWingsDone(Player* p);
    std::uint32_t Cap49DungeonsDone(Player* p);
    std::uint32_t FinaleDungeonsDone(Player* p);
    std::uint32_t FlightPathsDiscovered(Player* p);
    std::uint32_t FullZonesExplored(Player* p);
    std::uint32_t CapitalsVisited(Player* p);
    std::uint32_t FirstAidSkill(Player* p);
}

void AddIndividualLevelingProgressionScripts();

#endif // MOD_INDIVIDUAL_LEVELING_PROGRESSION_H
