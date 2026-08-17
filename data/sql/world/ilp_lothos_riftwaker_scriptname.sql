-- Bind the Lothos Riftwaker MC-attune gate (mod-individual-leveling-progression).
-- The C++ side registers a CreatureScript named npc_lothos_riftwaker_ilp_gate;
-- AC dispatches creature callbacks by matching creature_template.ScriptName.
-- Lothos has AIName = SmartAI for ambient behavior; ScriptName and AIName are
-- independent slots, so SmartAI continues to handle his idle/combat AI while
-- the new ScriptName routes gossip + quest hooks through our gate.
UPDATE `creature_template`
   SET `ScriptName` = 'npc_lothos_riftwaker_ilp_gate'
 WHERE `entry` = 14387;
