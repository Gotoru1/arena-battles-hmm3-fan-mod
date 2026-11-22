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
        elif effect["type"] == "EVOLUTION_TRAIT":
            self.assertTrue(subtype.isdecimal() and int(subtype) > 0)
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
            self.assertEqual(key, "trait" + effect["subtype"])
            original = design[int(effect["subtype"]) - 1]
            for field in ("name", "text", "rarity"):
                self.assertEqual(original[field], entry[field])

    def test_every_mutation_is_named_and_drawn(self):
        content = ROOT / "Mods/arenabattles/Content"
        texts = json.loads((content / "config/arenabattles/english.json").read_text(encoding="utf-8"))
        for key, entry in DATA["mutations"].items():
            effect, = entry["effects"]
            number = effect["subtype"]
            self.assertEqual(effect["description"], "@arenabattles.evolution.trait." + number)
            self.assertEqual(texts[effect["description"][1:]], "{%s}\n%s" % (entry["name"], entry["text"]))
            self.assertEqual(effect["icon"], "evolution/trait" + number)
            self.assertTrue((content / "Data" / (effect["icon"] + ".png")).is_file(), key)
        reactions = (ROOT / "server/battles/EvolutionReactions.inc").read_text(encoding="utf-8")
        statuses = re.search(r'names\[\]=\{([^}]*)\}', reactions).group(1).replace('"', "").split(",")
        self.assertEqual(len(statuses), reactions.count("{BonusType::", reactions.index("ailments()"), reactions.index("return all;")))
        for name in statuses:
            self.assertIn("arenabattles.evolution.status." + name, texts)
            self.assertTrue((content / "Data/evolution" / ("status_%s.png" % name)).is_file(), name)
        # Battlefield pictures: every referenced animation and every frame in it must ship with the mod.
        animations = {"evoMark_" + look for look in re.findall(r'"(blood|trap|shadow|ash|spores|mycelium|portal|curse|territory)"', reactions)}
        self.assertEqual(len(animations), 9)
        animations |= {name.replace("evoMark_", "evoMarkIn_") for name in animations}
        creatures = json.loads((content / "config/evolutionCreatures.json").read_text(encoding="utf-8"))
        for creature in creatures.values():
            graphics = creature["graphics"]
            if graphics["animation"].startswith("evo"):
                animations.add(graphics["animation"][:-4])
                for size in ("iconLarge", "iconSmall"):
                    self.assertTrue((content / "Data" / (graphics[size] + ".png")).is_file(), graphics[size])
        for name in animations:
            sheet = json.loads((content / "Sprites" / (name + ".json")).read_text(encoding="utf-8"))
            groups = {sequence["group"]: sequence["frames"] for sequence in sheet["sequences"]}
            self.assertTrue(groups[0])
            for frames in groups.values():
                for frame in frames:
                    self.assertTrue((content / "Sprites" / sheet["basepath"] / frame).is_file(), frame)
            if not name.startswith("evoMark"):
                # The battle renderer refuses a creature without these and plays the rest on demand.
                self.assertTrue({0, 1, 2, 3, 4, 5, 7, 8, 11, 12, 13, 20, 21} <= groups.keys(), name)

    def test_arena_layouts_surround_the_centre(self):
        text = (ROOT / "config/gameConfig.json").read_text(encoding="utf-8")
        text = re.sub(r"//[^\n]*", "", text)
        text = re.sub(r",(\s*[}\]])", r"\1", text)
        layouts = json.loads(text)["settings"]["combat"]["layouts"]
        for name, centre, edge in (("evolutionCenterAttacker", "attacker", "defender"), ("evolutionCenterDefender", "defender", "attacker")):
            layout = layouts[name]
            machines = set(layout["attackerWarMachines"]) | set(layout["defenderWarMachines"])
            for form in ("UnitsLoose", "UnitsTight"):
                for count in range(1, 8):
                    middle = layout[centre + form][count - 1]
                    flanks = layout[edge + form][count - 1]
                    self.assertEqual((len(middle), len(flanks)), (count, count))
                    self.assertEqual(len(set(middle) | set(flanks)), 2 * count)
                    self.assertFalse(machines & (set(middle) | set(flanks)))
                    for cell in middle:
                        self.assertIn(cell % 17, (7, 8, 9))
                        self.assertTrue(2 <= cell // 17 <= 8)
                    for cell in flanks:
                        self.assertIn(cell % 17, (1, 15))
                    if count > 1:
                        self.assertEqual({cell % 17 for cell in flanks}, {1, 15})

    def test_storage_keys_and_reserved_names(self):
        reactions = (ROOT / "server/battles/EvolutionReactions.inc").read_text(encoding="utf-8")
        # A reserve spans three consecutive bonus subtypes; nothing else may use the next two.
        keys = {int(k) for k in re.findall(r"[sS]etReserve\([^;]*?,(5\d{5})\)", reactions)}
        keys |= {int(k) for k in re.findall(r"reserve\(\w+,(5\d{5})\)", reactions)} | {500247}
        used = {int(n) for n in re.findall(r"\b(5\d{5})\b", reactions)}
        self.assertFalse([(key, key + limb) for key in keys for limb in (1, 2) if key + limb in used])
        # Names that the Windows headers define as macros break the MSVC build as identifiers.
        code = re.sub(r"//[^\n]*", "", reactions)
        for word in ("near", "far", "small", "interface"):
            self.assertIsNone(re.search(r"(?<![\w:.>])" + word + r"(?![\w(<])", code), word)

    def test_army_rewards(self):
        self.assertEqual(len(DATA["rewards"]),10)
        native = (ROOT/"config/artifacts.json").read_text(encoding="utf-8")
        for key, rule in DATA["rewards"].items():
            self.assertTrue(key.startswith("reward.") and key not in DATA["mutations"])
            self.assertTrue(rule["name"] and rule["text"] and rule["category"])
            if key.startswith("reward.artifact."):
                self.assertRegex(native, r'"'+re.escape(key.split(".")[-1])+r'"\s*:')

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
