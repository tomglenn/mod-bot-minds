-- Ranking a memory now weighs how often the bot has actually recalled it, not
-- just how salient it was when it was written. last_referenced alone only says
-- when it was last used, so the count has to be stored too, otherwise every
-- restart wipes out the evidence that a memory keeps coming up.
--
-- Plain ADD COLUMN: the updater applies each file once, tracked by name and hash,
-- and MySQL has no ADD COLUMN IF NOT EXISTS.
ALTER TABLE `mod_bot_minds_memory`
    ADD COLUMN `ref_count` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `last_referenced`;
