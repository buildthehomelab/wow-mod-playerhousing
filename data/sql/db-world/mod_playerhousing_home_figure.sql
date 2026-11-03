-- The home screen's figures (src/HomeScreen.cpp): the account's characters standing in the yard. The
-- module gives each one a body and a character's gear; the model here is only a placeholder. No name,
-- gossip or script, and it can't be attacked: clicking one only targets it, which the addon listens for.
-- It's in data/sql so it arrives by itself, because the home screen runs with the rest of the module's
-- world SQL (sql/db_world) not applied.
SET @FIGURE := 900204;

DELETE FROM `creature_template_model` WHERE `CreatureID` = @FIGURE;
DELETE FROM `creature_template` WHERE `entry` = @FIGURE;
INSERT INTO `creature_template`
(`entry`, `name`, `subname`, `gossip_menu_id`, `minlevel`, `maxlevel`, `faction`, `npcflag`, `unit_class`, `unit_flags`, `type`, `AIName`, `MovementType`, `RegenHealth`, `ScriptName`, `VerifiedBuild`) VALUES
(@FIGURE, '', '', 0, 1, 1, 35, 0, 1, 770, 7, '', 0, 1, '', 0);
INSERT INTO `creature_template_model`
(`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(@FIGURE, 0, 49, 1.0, 1.0, 0);
