# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify the shipped mutation data against this checkout's engine identifiers."""
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = json.loads((ROOT / "Mods/arenabattles/Content/config/evolution.json").read_text(encoding="utf-8"))
BONUSES = set(re.findall(r"BONUS_NAME\((\w+)\)", (ROOT / "lib/bonuses/BonusEnum.h").read_text()))
REGISTRY = (ROOT / "lib/modding/IdentifierStorage.cpp").read_text(encoding="utf-8")
SUBTYPES = set(re.findall(r'"bonusSubtype",\s*"(\w+)"', REGISTRY))


class CatalogTests(unittest.TestCase):
    def check_effect(self, effect, inside_event=False):
        self.assertIn(effect["type"], BONUSES)
        subtype = effect.get("subtype")
        if effect["type"] in {"SPELL_AFTER_ATTACK","SPELL_BEFORE_ATTACK"}:
            school, name = subtype.split(".")
            self.assertEqual(school, "spell")
            self.assertTrue(any(re.search(r'^\s*"' + re.escape(name) + r'"\s*:', p.read_text(encoding="utf-8"), re.M) for p in (ROOT/"config/spells").glob("*.json")))
        elif effect["type"] == "PRIMARY_SKILL":
            self.assertIn(subtype, {"primarySkill.attack", "primarySkill.defence"})
        elif subtype:
            self.assertIn(subtype, SUBTYPES)
        if inside_event:
            self.assertNotEqual(effect["type"], "ON_COMBAT_EVENT")
            self.assertIn(effect["duration"], {"ONE_BATTLE","UNTIL_ATTACK","UNTIL_OWN_ATTACK","UNTIL_BEING_ATTACKED","STACK_GETS_TURN"})
        if effect["type"] == "ON_COMBAT_EVENT":
            actions = effect["addInfo"]["effect"]
            self.assertTrue(actions)
            for action in actions:
                self.assertEqual(action["action"], "bonus")
                self.check_effect(action["bonus"], True)
        else:
            self.assertGreater(effect["val"], 0)

    def test_design_and_active_pool(self):
        design = json.loads((ROOT / "Mods/arenabattles/Content/config/evolution-design.json").read_text(encoding="utf-8"))
        self.assertEqual([row["id"] for row in design], list(range(1,101)))
        self.assertEqual([sum(row["rarity"] == r for row in design) for r in range(4)], [25,30,30,15])
        self.assertEqual(len({row["name"] for row in design}),100)
        self.assertEqual({item["rarity"] for item in DATA["mutations"].values()}, set(range(4)))
        for key, entry in DATA["mutations"].items():
            effect, = entry["effects"]
            self.check_effect(effect)
            self.assertEqual(effect["type"], "EVOLUTION_TRAIT")
            self.assertEqual(key, "trait" + str(effect["val"]))
            original = design[effect["val"] - 1]
            for field in ("name", "text", "rarity"):
                self.assertEqual(original[field], entry[field])

    def test_brood_creatures(self):
        mod = ROOT / "Mods/arenabattles"
        path, = json.loads((mod/"mod.json").read_text(encoding="utf-8"))["creatures"]
        defs = json.loads((mod/"Content"/path).read_text(encoding="utf-8"))
        self.assertEqual(defs["evolutionEgg"]["hitPoints"], 1)
        self.assertEqual(defs["evolutionEgg"]["abilities"]["incubation"]["type"], "NOT_ACTIVE")
        self.assertGreater(defs["evolutionLarva"]["speed"], 0)
        self.assertTrue(all(creature["special"] for creature in defs.values()))

    def test_encounter_catalog(self):
        self.assertFalse(DATA["synergies"])
        for pair in DATA["schools"]:
            self.assertEqual(len(set(pair)), 2)
            self.assertTrue(set(pair) <= DATA["mutations"].keys())
        bosses = [DATA["boss"], *DATA["otherBosses"]]
        self.assertEqual(len({boss["name"] for boss in bosses}), 10)
        for entry in bosses + DATA["anomalies"]:
            for effect in entry["effects"]:
                self.check_effect(effect)

    def test_boss_ramps_after_hit(self):
        effect, = DATA["boss"]["effects"]
        self.check_effect(effect)
        self.assertEqual(effect["subtype"], "combatEventAfterAttacked")
        self.assertEqual(effect["addInfo"]["effect"][0]["bonus"]["type"], "CREATURE_DAMAGE")


if __name__ == "__main__":
    unittest.main()
