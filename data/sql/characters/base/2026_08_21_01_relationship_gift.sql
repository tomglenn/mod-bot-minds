-- Gift cooldowns have to survive a restart, otherwise bouncing the server resets
-- everyone's generosity. This belongs on the relationship because that is what it
-- is: a fact about the two of them.
--
-- Plain ADD COLUMN: the updater applies each file once, tracked by name and hash,
-- and MySQL has no ADD COLUMN IF NOT EXISTS.
ALTER TABLE `mod_bot_minds_relationship`
    ADD COLUMN `last_gift_at` DATETIME NULL AFTER `interaction_count`;
