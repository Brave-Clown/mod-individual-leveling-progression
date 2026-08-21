#include "IndividualLevelingProgression.h"

#include "Battleground.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Group.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"

#include <ctime>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ILP
{
    std::string const SETTINGS_SOURCE = "mod-individual-leveling-progression";

    static Config s_cfg;

    // Boss entry → (gate, bit position in that gate's mask). Repopulated on every
    // LoadConfig() so a `.ilp reload` picks up edited boss-id lists.
    static std::unordered_map<std::uint32_t, std::pair<Gate, std::uint8_t>> s_bossMap;

    // Exploration lookups, all rebuilt on LoadConfig().
    static std::vector<std::uint32_t>                                    s_flightPathIds;   // taxi node IDs
    static std::vector<std::uint32_t>                                    s_capitalZoneIds;  // bit position = list index
    static std::unordered_map<std::uint32_t, std::uint8_t>               s_capitalBitOf;    // zoneId → bit

    // Cap40 full-zone exploration tracked by Blizzard's "Explore X Zone"
    // achievement criteria. Per achievement: a list of criteria; each criterion
    // is a list of areatable exploreFlags (match-any inside the criterion,
    // match-all across criteria). Read out of PLAYER_EXPLORED_ZONES_1 directly,
    // never via the achievement system — so an alt with an account-shared
    // achievement still has to actually walk the zone.
    static std::vector<std::uint32_t>                                                          s_exploreAchievementIds;
    static std::unordered_map<std::uint32_t, std::vector<std::vector<std::uint32_t>>>          s_exploreCriteriaFlags;

    Config& Cfg() { return s_cfg; }

    // SPEC §5 (revised 2026-06-08): ILP touches no bot — neither rndbots nor
    // bot-driven altbots. Bot leveling is governed by playerbots.conf; applying
    // ILP throttles or credit tracking would distort bracket population. "Bot"
    // is a runtime state, not a character flag — this returns true only while
    // the playerbots AI is attached. If a player logs in to one of their altbot
    // characters themselves, the AI detaches and ILP applies normally from that
    // moment on (state on the character is whatever it accumulated as a real
    // player; bot-driven sessions never write to it).
    static inline bool IsBot(Player* p)
    {
        return p && sPlayerbotsMgr.GetPlayerbotAI(p) != nullptr;
    }

    static SettingIndex MaskIndexFor(Gate g)
    {
        switch (g)
        {
            case GATE_CAP_29: return SETTING_CAP29_DUNGEON_MASK;
            case GATE_CAP_39: return SETTING_CAP39_SM_MASK;
            case GATE_CAP_49: return SETTING_CAP49_DUNGEON_MASK;
            case GATE_FINALE: return SETTING_FINALE_DUNGEON_MASK;
            default:          return SETTING_JOURNEY;  // sentinel — caller must check
        }
    }

    static std::uint32_t DungeonReqFor(Gate g)
    {
        switch (g)
        {
            case GATE_CAP_29: return s_cfg.cap29_dungeons;
            case GATE_CAP_39: return s_cfg.cap39_smWings;
            case GATE_CAP_49: return s_cfg.cap49_dungeons;
            case GATE_FINALE: return s_cfg.finale_dungeons;
            default:          return 0;
        }
    }

    static char const* DungeonLabelFor(Gate g)
    {
        switch (g)
        {
            case GATE_CAP_29: return "Distinct dungeons";
            case GATE_CAP_39: return "Scarlet Monastery";
            case GATE_CAP_49: return "Dungeons";
            case GATE_FINALE: return "Dungeons";
            default:          return "Dungeons";
        }
    }

    // Parse a comma-separated list of unsigned ints into `out`. Whitespace tolerated,
    // empty / non-numeric tokens skipped (but still consume a list position so bit
    // positions stay stable when an entry is removed by blanking it out).
    static void ParseUIntCSV(std::string const& csv, std::vector<std::uint32_t>& out)
    {
        std::stringstream ss(csv);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            size_t a = item.find_first_not_of(" \t");
            size_t b = item.find_last_not_of(" \t");
            std::uint32_t v = 0;
            if (a != std::string::npos)
            {
                std::string t = item.substr(a, b - a + 1);
                try { v = static_cast<std::uint32_t>(std::stoul(t)); }
                catch (...) { v = 0; }
            }
            out.push_back(v);
        }
    }

    // CSV → (gate, bit) map population. Duplicate entries across gates: last wins,
    // which is fine because the gate filter in CreditDungeonBoss only fires the
    // credit when the entry's mapped gate matches the player's *current* gate.
    static void ParseBossList(std::string const& csv, Gate g)
    {
        std::vector<std::uint32_t> ids;
        ParseUIntCSV(csv, ids);
        for (std::uint8_t bit = 0; bit < ids.size() && bit < 32; ++bit)
            if (ids[bit] != 0)
                s_bossMap[ids[bit]] = { g, bit };
    }

    // Walk Achievement_Criteria.dbc once per LoadConfig() and resolve each
    // tracked "Explore X Zone" achievement into the exact criterion list
    // Blizzard uses. Each EXPLORE_AREA criterion's `areaReference` is a
    // WorldMapOverlay ID; the overlay names up to MAX_WORLD_MAP_OVERLAY_AREA_IDX
    // areatable rows where any one set bit satisfies that criterion. The
    // achievement is "complete" when every criterion is satisfied — exactly
    // matching the achievement's own logic, but driven off the player's
    // per-character explored-zones bitmask (so account-shared achievements
    // don't pre-credit alts).
    static void PrecomputeExploreAchievements(std::vector<std::uint32_t> const& achIds)
    {
        s_exploreCriteriaFlags.clear();
        if (achIds.empty()) return;

        std::unordered_set<std::uint32_t> wanted(achIds.begin(), achIds.end());
        wanted.erase(0);
        if (wanted.empty()) return;

        for (std::uint32_t i = 0; i < sAchievementCriteriaStore.GetNumRows(); ++i)
        {
            AchievementCriteriaEntry const* c = sAchievementCriteriaStore.LookupEntry(i);
            if (!c) continue;
            if (c->requiredType != ACHIEVEMENT_CRITERIA_TYPE_EXPLORE_AREA) continue;
            if (!wanted.count(c->referredAchievement)) continue;

            WorldMapOverlayEntry const* ov = sWorldMapOverlayStore.LookupEntry(c->explore_area.areaReference);
            if (!ov) continue;

            std::vector<std::uint32_t> flags;
            // areatableID[] is 0-terminated by convention; mirror AC's runtime
            // loop which breaks on the first null lookup.
            for (std::uint32_t j = 0; j < MAX_WORLD_MAP_OVERLAY_AREA_IDX; ++j)
            {
                AreaTableEntry const* a = sAreaTableStore.LookupEntry(ov->areatableID[j]);
                if (!a) break;
                flags.push_back(a->exploreFlag);
            }
            if (!flags.empty())
                s_exploreCriteriaFlags[c->referredAchievement].push_back(std::move(flags));
        }
    }

    void LoadConfig()
    {
        s_cfg.enable          = sConfigMgr->GetOption<bool>("IndividualLevelingProgression.Enable",         true);
        s_cfg.announce        = sConfigMgr->GetOption<bool>("IndividualLevelingProgression.Announce",       true);
        s_cfg.debug           = sConfigMgr->GetOption<bool>("IndividualLevelingProgression.Debug",          true);
        s_cfg.journeyDefault  = sConfigMgr->GetOption<bool>("IndividualLevelingProgression.JourneyDefault", true);

        s_cfg.requirePvP         = sConfigMgr->GetOption<bool>("IndividualLevelingProgression.Require.PvP",         true);
        s_cfg.requireDungeons    = sConfigMgr->GetOption<bool>("IndividualLevelingProgression.Require.Dungeons",    true);
        s_cfg.requireFirstAid    = sConfigMgr->GetOption<bool>("IndividualLevelingProgression.Require.FirstAid",    true);
        s_cfg.requireExploration = sConfigMgr->GetOption<bool>("IndividualLevelingProgression.Require.Exploration", true);

        s_cfg.cap19_wsg         = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Cap19.WSGRequired",         1);
        s_cfg.cap29_ab          = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Cap29.ABRequired",          1);
        s_cfg.cap29_dungeons    = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Cap29.DungeonsRequired",    1);
        s_cfg.cap29_firstAid    = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Cap29.FirstAidRequired",    25);
        s_cfg.cap39_bg          = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Cap39.BGRequired",          1);
        s_cfg.cap39_smWings     = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Cap39.SMWingsRequired",     1);
        s_cfg.cap39_firstAid    = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Cap39.FirstAidRequired",    50);
        s_cfg.cap40_flightPaths = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Cap40.FlightPathsRequired", 1);
        s_cfg.cap40_fullZones   = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Cap40.FullZonesRequired",   6);
        s_cfg.cap49_bg          = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Cap49.BGRequired",          1);
        s_cfg.cap49_dungeons    = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Cap49.DungeonsRequired",    1);
        s_cfg.cap49_firstAid    = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Cap49.FirstAidRequired",    75);
        s_cfg.finale_av         = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Finale.AVRequired",         3);
        s_cfg.finale_dungeons   = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Finale.DungeonsRequired",   1);
        s_cfg.finale_capitals   = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Finale.CapitalsRequired",   1);
        s_cfg.finale_firstAid   = sConfigMgr->GetOption<uint32>("IndividualLevelingProgression.Finale.FirstAidRequired",   100);

        s_bossMap.clear();
        ParseBossList(sConfigMgr->GetOption<std::string>("IndividualLevelingProgression.Cap29.DungeonBossIds",  ""), GATE_CAP_29);
        ParseBossList(sConfigMgr->GetOption<std::string>("IndividualLevelingProgression.Cap39.SMWingBossIds",   ""), GATE_CAP_39);
        ParseBossList(sConfigMgr->GetOption<std::string>("IndividualLevelingProgression.Cap49.DungeonBossIds",  ""), GATE_CAP_49);
        ParseBossList(sConfigMgr->GetOption<std::string>("IndividualLevelingProgression.Finale.DungeonBossIds", ""), GATE_FINALE);

        s_flightPathIds.clear();
        ParseUIntCSV(sConfigMgr->GetOption<std::string>("IndividualLevelingProgression.Cap40.FlightPathIds", ""), s_flightPathIds);

        s_capitalZoneIds.clear();
        s_capitalBitOf.clear();
        ParseUIntCSV(sConfigMgr->GetOption<std::string>("IndividualLevelingProgression.Finale.CapitalIds", ""), s_capitalZoneIds);
        for (std::uint8_t bit = 0; bit < s_capitalZoneIds.size() && bit < 32; ++bit)
            if (s_capitalZoneIds[bit] != 0)
                s_capitalBitOf[s_capitalZoneIds[bit]] = bit;

        s_exploreAchievementIds.clear();
        ParseUIntCSV(sConfigMgr->GetOption<std::string>("IndividualLevelingProgression.Cap40.FullZoneAchievementIds", ""), s_exploreAchievementIds);
        PrecomputeExploreAchievements(s_exploreAchievementIds);
    }

    // World startup runs OnAfterConfigLoad (where LoadConfig fires) BEFORE
    // LoadDBCStores — so the first PrecomputeExploreAchievements pass walks
    // an empty sAchievementCriteriaStore and the map stays empty until the
    // first `.reload config`. Late-bind on OnBeforeWorldInitialized (post-DBC)
    // to repopulate using the achievement IDs already parsed from config.
    void RefreshExploreData()
    {
        PrecomputeExploreAchievements(s_exploreAchievementIds);
    }

    bool IsJourney(Player* p)
    {
        if (!p) return false;
        uint32 v = p->GetPlayerSetting(SETTINGS_SOURCE, SETTING_JOURNEY).value;
        if (v == 0) return s_cfg.journeyDefault;
        return v == 1;
    }

    void SetJourney(Player* p, bool on)
    {
        if (!p) return;
        p->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_JOURNEY, on ? 1u : 2u);
    }

    bool IsComplete(Player* p)
    {
        if (!p) return false;
        return p->GetPlayerSetting(SETTINGS_SOURCE, SETTING_COMPLETE).IsEnabled(1);
    }

    void SetComplete(Player* p, bool on)
    {
        if (!p) return;
        p->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_COMPLETE, on ? 1u : 0u);
    }

    bool SkipPendingValid(Player* p)
    {
        if (!p) return false;
        uint32 ts = p->GetPlayerSetting(SETTINGS_SOURCE, SETTING_SKIP_PENDING_T).value;
        if (ts == 0) return false;
        return (static_cast<uint32>(std::time(nullptr)) - ts) <= 60;
    }

    void SkipPendingSet(Player* p)
    {
        if (!p) return;
        p->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_SKIP_PENDING_T, static_cast<uint32>(std::time(nullptr)));
    }

    void SkipPendingClear(Player* p)
    {
        if (!p) return;
        p->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_SKIP_PENDING_T, 0u);
    }

    void ResetProgress(Player* p)
    {
        if (!p) return;
        // Zero every index this module owns. SETTING_JOURNEY back to 0 means
        // "unset", so IsJourney() will fall through to JourneyDefault on the
        // next read — matching what a brand-new character would see.
        SettingIndex const all[] = {
            SETTING_JOURNEY, SETTING_COMPLETE, SETTING_SKIP_PENDING_T,
            SETTING_WSG_COUNT, SETTING_AB_COUNT,
            SETTING_CAP39_BG_COUNT, SETTING_CAP49_BG_COUNT, SETTING_AV_COUNT,
            SETTING_CAP29_DUNGEON_MASK, SETTING_CAP39_SM_MASK,
            SETTING_CAP49_DUNGEON_MASK, SETTING_FINALE_DUNGEON_MASK,
        };
        for (SettingIndex idx : all)
            p->UpdatePlayerSetting(SETTINGS_SOURCE, idx, 0u);
    }

    Gate CurrentGate(Player* p)
    {
        if (!p) return GATE_COMPLETE;
        if (IsComplete(p)) return GATE_COMPLETE;
        uint8 lvl = p->GetLevel();
        if (lvl < 20) return GATE_CAP_19;
        if (lvl < 30) return GATE_CAP_29;
        if (lvl < 40) return GATE_CAP_39;
        if (lvl == 40) return GATE_CAP_40_BUMP;
        if (lvl < 50) return GATE_CAP_49;
        return GATE_FINALE;
    }

    char const* GateName(Gate g)
    {
        switch (g)
        {
            case GATE_CAP_19:      return "Cap 19 (PvP intro)";
            case GATE_CAP_29:      return "Cap 29 (Group up)";
            case GATE_CAP_39:      return "Cap 39 (Halfway hump)";
            case GATE_CAP_40_BUMP: return "Cap 40 (Speed bump)";
            case GATE_CAP_49:      return "Cap 49 (Last major hump)";
            case GATE_FINALE:      return "Finale (MC attune)";
            case GATE_COMPLETE:    return "Complete";
        }
        return "?";
    }

    uint32 FirstAidSkill(Player* p)
    {
        if (!p) return 0;
        return p->GetSkillValue(SKILL_FIRST_AID);
    }

    uint32 WSGCompleted(Player* p)        { return p ? p->GetPlayerSetting(SETTINGS_SOURCE, SETTING_WSG_COUNT).value      : 0; }
    uint32 ABCompleted(Player* p)         { return p ? p->GetPlayerSetting(SETTINGS_SOURCE, SETTING_AB_COUNT).value       : 0; }
    uint32 Cap39AnyBGCompleted(Player* p) { return p ? p->GetPlayerSetting(SETTINGS_SOURCE, SETTING_CAP39_BG_COUNT).value : 0; }
    uint32 Cap49AnyBGCompleted(Player* p) { return p ? p->GetPlayerSetting(SETTINGS_SOURCE, SETTING_CAP49_BG_COUNT).value : 0; }
    uint32 AVCompleted(Player* p)         { return p ? p->GetPlayerSetting(SETTINGS_SOURCE, SETTING_AV_COUNT).value       : 0; }

    uint8 CapLevelFor(Gate g)
    {
        switch (g)
        {
            case GATE_CAP_19:      return 19;
            case GATE_CAP_29:      return 29;
            case GATE_CAP_39:      return 39;
            case GATE_CAP_40_BUMP: return 40;
            case GATE_CAP_49:      return 49;
            case GATE_FINALE:      return 0;  // 60+, no XP cap; gate blocks MC attune only
            case GATE_COMPLETE:    return 0;
        }
        return 0;
    }

    bool GateSatisfied(Player* p, Gate g)
    {
        if (!p) return false;
        Config const& c = s_cfg;

        // A category check is satisfied when the toggle is off OR the counter
        // meets the requirement. Lets `Require.PvP = 0` bulk-skip PvP across
        // every gate without zeroing each per-gate threshold.
        auto pvp  = [&](uint32 cur, uint32 req) { return !c.requirePvP         || cur >= req; };
        auto dung = [&](uint32 cur, uint32 req) { return !c.requireDungeons    || cur >= req; };
        auto fa   = [&](uint32 cur, uint32 req) { return !c.requireFirstAid    || cur >= req; };
        auto expl = [&](uint32 cur, uint32 req) { return !c.requireExploration || cur >= req; };

        switch (g)
        {
            case GATE_CAP_19:
                return pvp(WSGCompleted(p), c.cap19_wsg);
            case GATE_CAP_29:
                return pvp (ABCompleted(p),        c.cap29_ab)
                    && dung(Cap29DungeonsDone(p),  c.cap29_dungeons)
                    && fa  (FirstAidSkill(p),      c.cap29_firstAid);
            case GATE_CAP_39:
                return pvp (Cap39AnyBGCompleted(p), c.cap39_bg)
                    && dung(Cap39SMWingsDone(p),    c.cap39_smWings)
                    && fa  (FirstAidSkill(p),       c.cap39_firstAid);
            case GATE_CAP_40_BUMP:
                return expl(FlightPathsDiscovered(p), c.cap40_flightPaths)
                    && expl(FullZonesExplored(p),     c.cap40_fullZones);
            case GATE_CAP_49:
                return pvp (Cap49AnyBGCompleted(p), c.cap49_bg)
                    && dung(Cap49DungeonsDone(p),   c.cap49_dungeons)
                    && fa  (FirstAidSkill(p),       c.cap49_firstAid);
            case GATE_FINALE:
                return pvp (AVCompleted(p),        c.finale_av)
                    && dung(FinaleDungeonsDone(p), c.finale_dungeons)
                    && expl(CapitalsVisited(p),    c.finale_capitals)
                    && fa  (FirstAidSkill(p),      c.finale_firstAid);
            case GATE_COMPLETE:
                return true;
        }
        return false;
    }

    void CreditCurrentGateBG(Player* p)
    {
        if (!p) return;
        if (IsBot(p)) return;
        Gate g = CurrentGate(p);

        SettingIndex idx;
        uint32 threshold;
        char const* label;
        switch (g)
        {
            case GATE_CAP_19: idx = SETTING_WSG_COUNT;      threshold = s_cfg.cap19_wsg;  label = "Warsong Gulch";  break;
            case GATE_CAP_29: idx = SETTING_AB_COUNT;       threshold = s_cfg.cap29_ab;   label = "Arathi Basin";   break;
            case GATE_CAP_39: idx = SETTING_CAP39_BG_COUNT; threshold = s_cfg.cap39_bg;   label = "Battleground";   break;
            case GATE_CAP_49: idx = SETTING_CAP49_BG_COUNT; threshold = s_cfg.cap49_bg;   label = "Battleground";   break;
            case GATE_FINALE: idx = SETTING_AV_COUNT;       threshold = s_cfg.finale_av;  label = "Alterac Valley"; break;
            default: return;  // GATE_CAP_40_BUMP / GATE_COMPLETE — no BG req
        }

        uint32 cur  = p->GetPlayerSetting(SETTINGS_SOURCE, idx).value;
        uint32 next = cur + 1;
        p->UpdatePlayerSetting(SETTINGS_SOURCE, idx, next);

        ChatHandler ch(p->GetSession());
        ch.PSendSysMessage("|cff4CFF00ILP|r: {} {}/{}.", label, next, threshold);

        // Announce gate release only when this credit just pushed the gate over.
        bool crossedThis = (cur < threshold && next >= threshold);
        if (crossedThis && GateSatisfied(p, g))
        {
            if (g == GATE_FINALE)
                CheckJourneyComplete(p);  // handles its own announce; CapLevelFor(FINALE) == 0
            else
                ch.PSendSysMessage("|cffFFFF00All {} requirements met — you may now level beyond {}.|r",
                                   GateName(g), CapLevelFor(g));
        }
    }

    static uint32 PopcountMask(Player* p, SettingIndex idx)
    {
        if (!p) return 0;
        uint32 mask = p->GetPlayerSetting(SETTINGS_SOURCE, idx).value;
        return static_cast<uint32>(__builtin_popcount(mask));
    }

    uint32 Cap29DungeonsDone(Player* p)  { return PopcountMask(p, SETTING_CAP29_DUNGEON_MASK);  }
    uint32 Cap39SMWingsDone(Player* p)   { return PopcountMask(p, SETTING_CAP39_SM_MASK);       }

    uint32 Cap49DungeonsDone(Player* p)  { return PopcountMask(p, SETTING_CAP49_DUNGEON_MASK);  }

    uint32 FinaleDungeonsDone(Player* p) { return PopcountMask(p, SETTING_FINALE_DUNGEON_MASK); }
    uint32 CapitalsVisited(Player* p)    { return PopcountMask(p, SETTING_CAPITALS_MASK);       }

    uint32 FlightPathsDiscovered(Player* p)
    {
        if (!p) return 0;
        uint32 n = 0;
        for (std::uint32_t nodeId : s_flightPathIds)
            if (nodeId != 0 && p->m_taxi.IsTaximaskNodeKnown(nodeId))
                ++n;
        return n;
    }

    // A zone is "fully explored" when its "Explore X Zone" achievement
    // would have fired — i.e. every criterion in s_exploreCriteriaFlags is
    // satisfied (each criterion = match-any across its areatable exploreFlags).
    // Reads the per-character explored bitmask, never the achievement system,
    // so account-shared achievements don't pre-credit alts.
    uint32 FullZonesExplored(Player* p)
    {
        if (!p) return 0;
        uint32 done = 0;
        for (std::uint32_t achId : s_exploreAchievementIds)
        {
            if (achId == 0) continue;
            auto it = s_exploreCriteriaFlags.find(achId);
            if (it == s_exploreCriteriaFlags.end() || it->second.empty()) continue;
            bool allCriteriaMet = true;
            for (auto const& crit : it->second)
            {
                bool any = false;
                for (std::uint32_t flag : crit)
                {
                    std::uint32_t off  = flag / 32;
                    std::uint32_t mask = 1u << (flag % 32);
                    if (off >= PLAYER_EXPLORED_ZONES_SIZE) continue;
                    if (p->GetUInt32Value(PLAYER_EXPLORED_ZONES_1 + off) & mask) { any = true; break; }
                }
                if (!any) { allCriteriaMet = false; break; }
            }
            if (allCriteriaMet) ++done;
        }
        return done;
    }

    void CheckJourneyComplete(Player* p)
    {
        if (!p) return;
        if (IsBot(p)) return;
        if (!s_cfg.enable) return;
        if (!IsJourney(p) || IsComplete(p)) return;
        if (CurrentGate(p) != GATE_FINALE) return;
        if (!GateSatisfied(p, GATE_FINALE)) return;

        SetComplete(p, true);

        ChatHandler ch(p->GetSession());
        ch.PSendSysMessage("|cffFFD700ILP|r: Journey complete. The Molten Core attunement is open to you.");
    }

    void EnforceMCBackstop(Player* p)
    {
        constexpr std::uint32_t MC_MAP_ID = 409;
        if (!p) return;
        if (IsBot(p)) return;
        if (!s_cfg.enable) return;
        if (p->GetMapId() != MC_MAP_ID) return;
        if (IsComplete(p)) return;
        if (!IsJourney(p)) return;

        ChatHandler(p->GetSession()).PSendSysMessage(
            "|cffFFD700ILP|r: The Molten Core rejects you — your journey is not yet complete.");
        p->TeleportTo(p->m_homebindMapId, p->m_homebindX, p->m_homebindY, p->m_homebindZ, p->GetOrientation());
    }

    void CreditDungeonBoss(Player* p, std::uint32_t creatureEntry)
    {
        if (!p) return;
        if (IsBot(p)) return;
        if (!s_cfg.enable) return;
        if (!IsJourney(p) || IsComplete(p)) return;

        auto it = s_bossMap.find(creatureEntry);
        if (it == s_bossMap.end()) return;

        Gate bossGate = it->second.first;
        std::uint8_t bit = it->second.second;

        // Boss only credits when it matches the player's current gate. Killing
        // Mograine at 60 doesn't retroactively fill the Cap 39 bit; killing
        // Drakkisath at 35 doesn't pre-fill the Finale bit either.
        if (CurrentGate(p) != bossGate) return;

        SettingIndex idx = MaskIndexFor(bossGate);
        if (idx == SETTING_JOURNEY) return;

        uint32 mask = p->GetPlayerSetting(SETTINGS_SOURCE, idx).value;
        uint32 b = 1u << bit;
        if (mask & b) return;  // already credited this boss

        uint32 newMask = mask | b;
        p->UpdatePlayerSetting(SETTINGS_SOURCE, idx, newMask);

        uint32 done = static_cast<uint32>(__builtin_popcount(newMask));
        uint32 need = DungeonReqFor(bossGate);

        ChatHandler ch(p->GetSession());
        ch.PSendSysMessage("|cff4CFF00ILP|r: {} {}/{}.", DungeonLabelFor(bossGate), done, need);

        if (done >= need && GateSatisfied(p, bossGate))
        {
            if (bossGate == GATE_FINALE)
                CheckJourneyComplete(p);  // handles its own announce; CapLevelFor(FINALE) == 0
            else
                ch.PSendSysMessage("|cffFFFF00All {} requirements met — you may now level beyond {}.|r",
                                   GateName(bossGate), CapLevelFor(bossGate));
        }
    }

    void CreditCapitalVisit(Player* p, std::uint32_t zoneId)
    {
        if (!p) return;
        if (IsBot(p)) return;
        if (!s_cfg.enable) return;
        if (!IsJourney(p) || IsComplete(p)) return;

        auto it = s_capitalBitOf.find(zoneId);
        if (it == s_capitalBitOf.end()) return;

        std::uint8_t bit = it->second;
        std::uint32_t mask = p->GetPlayerSetting(SETTINGS_SOURCE, SETTING_CAPITALS_MASK).value;
        std::uint32_t b    = 1u << bit;
        if (mask & b) return;  // already visited

        std::uint32_t newMask = mask | b;
        p->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_CAPITALS_MASK, newMask);

        std::uint32_t done = static_cast<std::uint32_t>(__builtin_popcount(newMask));
        std::uint32_t need = s_cfg.finale_capitals;

        ChatHandler ch(p->GetSession());
        ch.PSendSysMessage("|cff4CFF00ILP|r: Capitals visited {}/{}.", done, need);

        // Capitals can be banked early — CheckJourneyComplete self-gates on
        // the player actually being at GATE_FINALE, so calling it unconditionally
        // is fine and matches the other credit hooks' pattern.
        CheckJourneyComplete(p);
    }
}

// -------------------------------------------------------------------------------------------------
// PlayerScript — announce on login. XP cap + counter increments wire in next.
// -------------------------------------------------------------------------------------------------

class IndividualLevelingProgressionPlayer : public PlayerScript
{
public:
    IndividualLevelingProgressionPlayer() : PlayerScript("IndividualLevelingProgressionPlayer", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_CREATURE_KILL,
        PLAYERHOOK_ON_UPDATE_ZONE,
        PLAYERHOOK_ON_MAP_CHANGED,
        PLAYERHOOK_ON_UPDATE_SKILL
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!ILP::Cfg().enable || !player) return;
        if (ILP::IsBot(player)) return;
        if (ILP::Cfg().announce)
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the |cff4CFF00ILP|r module. Type |cff4CFF00.ilp status|r to view your journey.");
        // Catches passive completion paths ILP doesn't hook (e.g. First Aid
        // skill-ups crossing 300 between sessions) and self-heals drift.
        ILP::CheckJourneyComplete(player);
        // Logged out inside MC, came back not-complete -> kick out.
        ILP::EnforceMCBackstop(player);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        ILP::EnforceMCBackstop(player);
    }

    // First Aid is the only pillar without its own credit hook — skill ticks
    // happen silently in AC's skill system. Without this hook:
    //   1. At Finale: journey doesn't flip to complete until the next
    //      login/credit-event after FA crosses 300.
    //   2. At Cap 29/39/49: the gate-release announce ("you may now level
    //      beyond X") never fires when FA is the closing pillar — the gate
    //      DOES release for XP purposes (GateSatisfied is re-evaluated live
    //      on every XP grant), the player just doesn't see the message until
    //      a later credit event re-triggers it.
    // CheckJourneyComplete self-gates on Finale + GateSatisfied so it's a
    // cheap no-op outside Finale. The non-Finale path uses (value, newValue)
    // to detect the FA-threshold crossing — announce-once is naturally a
    // skill-tick edge event, no RELEASED_MASK bit needed.
    void OnPlayerUpdateSkill(Player* player, uint32 skillId, uint32 value,
                             uint32 /*max*/, uint32 /*step*/, uint32 newValue) override
    {
        if (!ILP::Cfg().enable || !player) return;
        if (skillId != SKILL_FIRST_AID) return;
        if (ILP::IsBot(player)) return;
        if (!ILP::IsJourney(player) || ILP::IsComplete(player)) return;

        ILP::CheckJourneyComplete(player);

        if (!ILP::Cfg().requireFirstAid) return;

        ILP::Gate g = ILP::CurrentGate(player);
        uint32 req = 0;
        switch (g)
        {
            case ILP::GATE_CAP_29: req = ILP::Cfg().cap29_firstAid; break;
            case ILP::GATE_CAP_39: req = ILP::Cfg().cap39_firstAid; break;
            case ILP::GATE_CAP_49: req = ILP::Cfg().cap49_firstAid; break;
            default: return;  // Cap 19 / Cap 40 bump / Finale / Complete — no announce here
        }
        if (req == 0) return;
        if (!(value < req && newValue >= req)) return;
        if (!ILP::GateSatisfied(player, g)) return;

        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffFFFF00All {} requirements met — you may now level beyond {}.|r",
            ILP::GateName(g), ILP::CapLevelFor(g));
    }

    // The cap bites at the ding boundary (SPEC §2): the player fills the bar
    // normally at the cap level but the level-up into the next level is held
    // until the gate is satisfied. Player::GiveXP loops internally to handle
    // multi-level dings WITHOUT re-firing this hook, so a single large XP grant
    // can cascade through every gate unless we clamp the *total budget* up
    // front. We also can't just check the current gate — if it's satisfied but
    // the next one isn't, the multi-ding loop would carry the player past it.
    // So we walk forward to the first unsatisfied gate at or after current and
    // clamp to its peg (cap level, bar at next_level_xp - 1).
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (!ILP::Cfg().enable || !player) return;
        if (ILP::IsBot(player)) return;
        if (!ILP::IsJourney(player) || ILP::IsComplete(player)) return;

        // Find first unsatisfied gate at or after the player's current gate.
        // Skip gates with CapLevelFor==0 (Finale carries no XP cap). If every
        // gate from here through Finale is satisfied, no clamp.
        uint8 stopLevel = 0;
        ILP::Gate g = ILP::CurrentGate(player);
        while (g < ILP::GATE_COMPLETE)
        {
            uint8 cl = ILP::CapLevelFor(g);
            if (cl != 0 && !ILP::GateSatisfied(player, g)) { stopLevel = cl; break; }
            g = static_cast<ILP::Gate>(g + 1);
        }
        if (stopLevel == 0) return;

        uint8 curLevel = player->GetLevel();
        if (curLevel > stopLevel) return;    // already past — don't touch

        uint32 curXP    = player->GetUInt32Value(PLAYER_XP);
        uint32 needed   = player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
        if (needed == 0) { amount = 0; return; }

        // Compute XP budget from (curLevel, curXP) to the peg (stopLevel, needed_at_stop - 1).
        uint64 budget = 0;
        if (curLevel == stopLevel)
        {
            budget = (curXP + 1 >= needed) ? 0u : (needed - 1 - curXP);
        }
        else
        {
            budget = needed - curXP;                          // ding to curLevel+1
            for (uint8 l = curLevel + 1; l < stopLevel; ++l)
                budget += sObjectMgr->GetXPForLevel(l);       // ding from l to l+1
            uint32 needed_at_stop = sObjectMgr->GetXPForLevel(stopLevel);
            if (needed_at_stop > 0)
                budget += needed_at_stop - 1;                 // fill the stop bar to peg
        }

        if (amount > budget)
            amount = static_cast<uint32>(budget);
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/) override
    {
        if (!ILP::Cfg().enable || !player) return;
        ILP::CreditCapitalVisit(player, newZone);
    }

    // Shared credit per SPEC §5: the killer plus every grouped journey-on
    // member at reward distance gets the bit. CreditDungeonBoss itself filters
    // by current-gate / journey / already-credited, so this hook stays simple.
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (!ILP::Cfg().enable || !killer || !killed) return;

        std::uint32_t entry = killed->GetEntry();
        // Cheap early-out: no tracked boss with this entry, skip the work.
        // (s_bossMap lookup is O(1) but the gate/journey checks downstream
        // aren't, and most creature kills won't match.)
        ILP::CreditDungeonBoss(killer, entry);

        if (Group* gr = killer->GetGroup())
        {
            for (GroupReference* itr = gr->GetFirstMember(); itr; itr = itr->next())
            {
                Player* m = itr->GetSource();
                if (!m || m == killer) continue;
                if (!m->IsInWorld()) continue;
                if (!m->IsAtGroupRewardDistance(killed)) continue;
                ILP::CreditDungeonBoss(m, entry);
            }
        }
    }
};

// -------------------------------------------------------------------------------------------------
// WorldScript — load config on startup and on `.reload config`.
// -------------------------------------------------------------------------------------------------

class IndividualLevelingProgressionWorld : public WorldScript
{
public:
    IndividualLevelingProgressionWorld() : WorldScript("IndividualLevelingProgressionWorld", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        ILP::LoadConfig();
    }

    void OnBeforeWorldInitialized() override
    {
        ILP::RefreshExploreData();
    }
};

// -------------------------------------------------------------------------------------------------
// AllBattlegroundScript — count Warsong Gulch completions for Cap 19.
// Shared credit (SPEC §5): every journey-on, not-complete player in the BG at end gets credited.
// -------------------------------------------------------------------------------------------------

class IndividualLevelingProgressionBG : public AllBattlegroundScript
{
public:
    IndividualLevelingProgressionBG() : AllBattlegroundScript("IndividualLevelingProgressionBG", {
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END
    }) { }

    void OnBattlegroundEnd(Battleground* bg, TeamId /*winnerTeam*/) override
    {
        if (!ILP::Cfg().enable || !bg) return;
        BattlegroundTypeId const t = bg->GetBgTypeID(true);

        for (auto const& kv : bg->GetPlayers())
        {
            Player* p = kv.second;
            if (!p || !p->IsInWorld()) continue;
            if (!ILP::IsJourney(p) || ILP::IsComplete(p)) continue;

            // Per SPEC §4 each gate accepts a specific BG type (or any BG for
            // Cap 39/49). Cap 19 takes only WSG; Cap 29 only AB; Finale only AV.
            // Bracket scoping is automatic because each cap's level range only
            // matches its own bracket.
            bool counts = false;
            switch (ILP::CurrentGate(p))
            {
                case ILP::GATE_CAP_19: counts = (t == BATTLEGROUND_WS); break;
                case ILP::GATE_CAP_29: counts = (t == BATTLEGROUND_AB); break;
                case ILP::GATE_CAP_39: counts = true;                   break;
                case ILP::GATE_CAP_49: counts = true;                   break;
                case ILP::GATE_FINALE: counts = (t == BATTLEGROUND_AV); break;
                default: break;
            }
            if (counts) ILP::CreditCurrentGateBG(p);
        }
    }
};

// -------------------------------------------------------------------------------------------------
// UnitScript backstop: when a pet, guardian, totem, elemental, or NPC ally
// lands the killing blow, killer isn't a Player and OnPlayerCreatureKill at
// Unit.cpp:14303 never fires. We catch those via OnUnitDeath and credit the
// tap holder (Creature::GetLootRecipient — set by upstream tap rules when the
// creature first takes player damage). CreditDungeonBoss's bitmask dedup makes
// it safe when both hooks fire for the same kill.
// -------------------------------------------------------------------------------------------------

class IndividualLevelingProgressionUnit : public UnitScript
{
public:
    IndividualLevelingProgressionUnit() : UnitScript("IndividualLevelingProgressionUnit") { }

    void OnUnitDeath(Unit* victim, Unit* /*killer*/) override
    {
        if (!ILP::Cfg().enable || !victim) return;
        Creature* creature = victim->ToCreature();
        if (!creature) return;
        Player* tapper = creature->GetLootRecipient();
        if (!tapper) return;
        uint32 entry = creature->GetEntry();
        ILP::CreditDungeonBoss(tapper, entry);
        if (Group* gr = tapper->GetGroup())
        {
            for (GroupReference* itr = gr->GetFirstMember(); itr; itr = itr->next())
            {
                Player* m = itr->GetSource();
                if (!m || m == tapper) continue;
                if (!m->IsInWorld()) continue;
                if (!m->IsAtGroupRewardDistance(creature)) continue;
                ILP::CreditDungeonBoss(m, entry);
            }
        }
    }
};

// -------------------------------------------------------------------------------------------------
// Lothos Riftwaker (entry 14387) — primary MC attune gate per SPEC §6.
//
// Lothos is the only NPC who starts/ends the "Attunement to the Core" quest
// (7487/7848 — the two faction variants). For a character still on the journey
// with the Finale gate not yet cleared, we suppress his quest list entirely
// and show a refusal line. Already-complete characters and characters not on a
// journey get the vanilla NPC behavior. No conf toggle — when the module is
// enabled, the gate is on. Set `IndividualLevelingProgression.Enable = 0` to disable.
// -------------------------------------------------------------------------------------------------

class LothosRiftwakerGate : public CreatureScript
{
public:
    LothosRiftwakerGate() : CreatureScript("npc_lothos_riftwaker_ilp_gate") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!ILP::Cfg().enable || !player || !creature) return false;
        if (ILP::IsBot(player))       return false;  // bot — fall through to vanilla gossip
        if (ILP::IsComplete(player))  return false;  // journey done — let Lothos behave normally
        if (!ILP::IsJourney(player))  return false;  // off-journey alt — also normal

        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cffFFD700Lothos Riftwaker|r looks you over and shakes his head. "
            "\"You are not yet ready for the Core, traveler. Complete your journey first.\"");
        player->PlayerTalkClass->SendCloseGossip();
        return true;  // suppress default quest list
    }
};

extern void RegisterIndividualLevelingProgressionCommandScript();

void AddIndividualLevelingProgressionScripts()
{
    new IndividualLevelingProgressionWorld();
    new IndividualLevelingProgressionPlayer();
    new IndividualLevelingProgressionBG();
    new IndividualLevelingProgressionUnit();
    new LothosRiftwakerGate();
    RegisterIndividualLevelingProgressionCommandScript();
}
