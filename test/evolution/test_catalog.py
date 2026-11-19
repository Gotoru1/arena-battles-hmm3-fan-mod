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

    def test_mutations(self):
        mutations = DATA["mutations"]
        self.assertGreaterEqual(len(mutations), 80)
        self.assertEqual({entry['rarity'] for entry in mutations.values()}, {0, 1, 2, 3})
        for key, entry in mutations.items():
            with self.subTest(mutation=key):
                self.assertTrue(entry["effects"] or key in {"mass", "harvest", "kinship", "ossuary", "nursery", "underdog", "seedbank", "remnant", "tithe", "graft", "spoils", "molting", "incubator"})
                self.assertTrue(entry["name"] and entry["text"])
                if "awaken" in entry:
                    self.assertEqual(entry["awaken"]["rank"], 2)
                    self.assertTrue(entry["alternate"]["effects"] and entry["alternate"]["text"])
                    self.assertNotEqual(entry["alternate"]["name"], entry["awaken"]["name"])
                for effect in entry["effects"] + entry.get("awaken", {}).get("effects", []) + entry.get("alternate", {}).get("effects", []):
                    self.check_effect(effect)

    def test_hidden_combinations(self):
        actual = {tuple(sorted(s["requires"])): s for s in DATA["synergies"]}
        expected = {("fission", "flurry"): "FEROCITY", ("barbs", "leech"): "FIRE_SHIELD",
                    ("carapace", "riposte"): "ON_COMBAT_EVENT"}
        for pair, bonus in expected.items():
            with self.subTest(pair=pair):
                self.assertTrue(set(pair) <= DATA["mutations"].keys())
                effects = actual[pair]["effects"]
                self.assertIn(bonus, {e["type"] for e in effects})
                for e in effects:
                    self.check_effect(e)

    def test_rotation_and_recipe_effects(self):
        bosses = [DATA["boss"], *DATA["otherBosses"]]
        self.assertEqual(len({b["name"] for b in bosses}), 10)
        for entry in bosses + DATA["synergies"] + DATA["anomalies"]:
            for effect in entry["effects"]:
                self.check_effect(effect)
        recipes = {tuple(sorted(entry["requires"])) for entry in DATA["synergies"]}
        for school in DATA["schools"]:
            self.assertIn(tuple(sorted(school)), recipes)
            self.assertTrue(all(DATA["mutations"][key]["effects"] for key in school))
        for recipe in DATA["synergies"]:
            self.assertTrue(set(recipe["requires"]) <= DATA["mutations"].keys())
        self.assertEqual(len({tuple(sorted(s["requires"])) for s in DATA["synergies"]}), 30)

    def test_boss_ramps_after_hit(self):
        effect, = DATA["boss"]["effects"]
        self.check_effect(effect)
        self.assertEqual(effect["subtype"], "combatEventAfterAttacked")
        self.assertEqual(effect["addInfo"]["effect"][0]["bonus"]["type"], "CREATURE_DAMAGE")


if __name__ == "__main__":
    unittest.main()
