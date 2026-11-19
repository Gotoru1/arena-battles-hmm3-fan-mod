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
        if effect["type"] == "PRIMARY_SKILL":
            self.assertIn(subtype, {"primarySkill.attack", "primarySkill.defence"})
        elif subtype:
            self.assertIn(subtype, SUBTYPES)
        if inside_event:
            self.assertIn(effect["type"], {"CREATURE_DAMAGE", "PRIMARY_SKILL"})
            self.assertEqual(effect["duration"], "ONE_BATTLE")
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
        self.assertGreaterEqual(len(mutations), 28)
        self.assertEqual({entry['rarity'] for entry in mutations.values()}, {0, 1, 2, 3})
        for key, entry in mutations.items():
            with self.subTest(mutation=key):
                self.assertTrue(entry["effects"] or key in {"mass", "harvest"})
                self.assertTrue(entry["name"] and entry["text"])
                for effect in entry["effects"]:
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
        self.assertEqual(len({b["name"] for b in bosses}), 3)
        for entry in bosses + DATA["synergies"]:
            for effect in entry["effects"]:
                self.check_effect(effect)
        for recipe in DATA["synergies"]:
            self.assertTrue(set(recipe["requires"]) <= DATA["mutations"].keys())
        self.assertEqual(len({tuple(sorted(s["requires"])) for s in DATA["synergies"]}), 7)

    def test_boss_ramps_after_hit(self):
        effect, = DATA["boss"]["effects"]
        self.check_effect(effect)
        self.assertEqual(effect["subtype"], "combatEventAfterAttacked")
        self.assertEqual(effect["addInfo"]["effect"][0]["bonus"]["type"], "CREATURE_DAMAGE")


if __name__ == "__main__":
    unittest.main()
