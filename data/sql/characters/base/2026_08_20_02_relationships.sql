CREATE TABLE IF NOT EXISTS `mod_bot_minds_relationship` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    `bot_guid` BIGINT UNSIGNED NOT NULL,
    `other_guid` BIGINT UNSIGNED NOT NULL,
    `other_is_bot` TINYINT NOT NULL DEFAULT 0,
    `affinity` FLOAT NOT NULL DEFAULT 0.0,
    `reason` VARCHAR(255) NULL,
    `interaction_count` INT UNSIGNED NOT NULL DEFAULT 0,
    `last_updated` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE KEY `uq_bot_other` (`bot_guid`, `other_guid`),
    INDEX `idx_bot_guid` (`bot_guid`),
    INDEX `idx_other_guid` (`other_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
