-- Generated personas used to carry a heroic backstory ("... walking the path of
-- the Crusader") and third-person speech styles ("speaks plainly and to the
-- point"). Both fed the mythic-hero voice the module no longer wants, and the
-- speech styles are now written in second person.
--
-- Drop the generated rows so they are rebuilt from the current templates the next
-- time each bot speaks. Hand-written personas do not match this pattern and are
-- left alone.
DELETE FROM `mod_bot_minds_persona` WHERE `backstory` LIKE '%walking the path of the%';
