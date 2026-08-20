CREATE TABLE IF NOT EXISTS `mod_bot_minds_memory` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    `bot_guid` BIGINT UNSIGNED NOT NULL,
    `subject_guid` BIGINT UNSIGNED NULL,
    `kind` ENUM('event','fact','summary') NOT NULL DEFAULT 'event',
    `text` TEXT NOT NULL,
    `salience` FLOAT NOT NULL DEFAULT 0.5,
    `action_hint` VARCHAR(32) NULL,
    `action_state` ENUM('none','pending','done','failed') NOT NULL DEFAULT 'none',
    `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `last_referenced` DATETIME NULL,
    INDEX `idx_bot_guid` (`bot_guid`),
    INDEX `idx_bot_subject` (`bot_guid`, `subject_guid`),
    INDEX `idx_bot_salience` (`bot_guid`, `salience`),
    INDEX `idx_kind` (`kind`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
