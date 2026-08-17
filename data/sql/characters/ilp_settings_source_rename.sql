-- Rename SETTINGS_SOURCE for the leveling-progression -> individual-leveling-progression
-- module rename. character_settings rows store the module's source string verbatim;
-- without this migration, every test/dev character would silently lose its stored
-- journey/complete/counter state because the new code looks up settings under the
-- new source string. Idempotent: re-running matches zero rows after the first apply.
UPDATE `character_settings`
   SET `source` = 'mod-individual-leveling-progression'
 WHERE `source` = 'mod-leveling-progression';
