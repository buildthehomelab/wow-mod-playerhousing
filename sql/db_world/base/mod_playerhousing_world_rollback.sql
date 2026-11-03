-- Removes everything mod-playerhousing added to the world database.
-- The creature table's entry column is id1 in older cores and id in newer ones.
SET @CREATURE_ENTRY := IF(EXISTS (SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE()
    AND TABLE_NAME = 'creature' AND COLUMN_NAME = 'id1'), 'id1', 'id');
SET @SQL := CONCAT('DELETE FROM `creature` WHERE `', @CREATURE_ENTRY, '` IN (900200, 900201, 900202, 900203)');
PREPARE housing_stmt FROM @SQL;
EXECUTE housing_stmt;
DEALLOCATE PREPARE housing_stmt;
DELETE FROM `creature_template_movement` WHERE `CreatureId` = 900203;
DELETE FROM `creature_template_model` WHERE `CreatureID` IN (900200, 900201, 900202, 900203, 900204);
DELETE FROM `creature_template` WHERE `entry` IN (900200, 900201, 900202, 900203, 900204);
DELETE FROM `npc_vendor` WHERE `entry` = 900200;
-- The ghosts' models and displays (curated pieces, then the catalog).
DELETE FROM `creaturedisplayinfo_dbc` WHERE `ID` BETWEEN 61100 AND 62999 OR `ID` BETWEEN 100000 AND 104999;
DELETE FROM `creaturemodeldata_dbc` WHERE `ID` BETWEEN 61100 AND 62999 OR `ID` BETWEEN 100000 AND 104999;
DELETE FROM `creature_model_info` WHERE `DisplayID` BETWEEN 61100 AND 62999 OR `DisplayID` BETWEEN 100000 AND 104999;
DELETE FROM `item_template` WHERE `entry` BETWEEN 901100 AND 901199 OR `entry` BETWEEN 902000 AND 902999 OR `entry` BETWEEN 940000 AND 944999;
DELETE FROM `gameobject` WHERE `id` = 903992;
DELETE FROM `gameobjectdisplayinfo_dbc` WHERE `ID` = 90000;
DELETE FROM `gameobject_template` WHERE `entry` IN (903990, 903991, 903992) OR `entry` BETWEEN 911100 AND 922999 OR `entry` BETWEEN 950000 AND 954999;
-- Figurines
DELETE FROM `creature_template_model` WHERE `CreatureID` BETWEEN 931100 AND 932999;
DELETE FROM `creature_template` WHERE `entry` BETWEEN 931100 AND 932999;
DELETE FROM `npc_text` WHERE `ID` BETWEEN 900300 AND 900319;
DELETE FROM `spell_script_names` WHERE `ScriptName` IN ('spell_playerhousing_place', 'spell_playerhousing_key');
DROP TABLE IF EXISTS `mod_playerhousing_piece_rule`;
DROP TABLE IF EXISTS `mod_playerhousing_piece`;
DROP TABLE IF EXISTS `mod_playerhousing_layout`;

DELETE FROM `creature_queststarter` WHERE `quest` BETWEEN 900400 AND 900404;
DELETE FROM `creature_questender` WHERE `quest` BETWEEN 900400 AND 900404;
DELETE FROM `quest_request_items` WHERE `ID` BETWEEN 900400 AND 900404;
DELETE FROM `quest_offer_reward` WHERE `ID` BETWEEN 900400 AND 900404;
DELETE FROM `quest_template_addon` WHERE `ID` BETWEEN 900400 AND 900404;
DELETE FROM `quest_template` WHERE `ID` BETWEEN 900400 AND 900404;
DELETE FROM `conditions` WHERE `SourceTypeOrReferenceId` = 19 AND `SourceEntry` BETWEEN 900400 AND 900404;
