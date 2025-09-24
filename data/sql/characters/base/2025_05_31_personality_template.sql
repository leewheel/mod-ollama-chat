CREATE TABLE IF NOT EXISTS `mod_ollama_chat_personality_templates` (
  `key` VARCHAR(64) NOT NULL PRIMARY KEY,
  `prompt` TEXT NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;


