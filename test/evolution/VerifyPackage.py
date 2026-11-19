# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the shipped Windows client with a smaller reward pool in a private QA profile."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import zipfile

POOLS = {
    "replay": None,
    "mirror": ["barbs", "leech", "rage", "sweep", "stride", "haste"],
    "fortress": ["carapace", "riposte", "stride", "rage"],
    "growth": ["harvest", "haste", "sweep"],
    "defeat": ["vigor", "haste", "harvest"],
}


def record_map(path):
    with zipfile.ZipFile(path) as archive:
        read = lambda name: json.loads(re.sub(r"(?m)^\s*//[^\r\n]*", "", archive.read(name).decode()))
        objects = read("objects.json")
        header = read("header.json")
    return {"seed": header.get("evolutionSeed"), "army": [
        [u for u in hero["options"]["army"] if u.get("amount")] for hero in objects if hero.get("type") == "hero"
    ]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("package", type=Path, help="The inner portable ZIP containing VCMI_client.exe")
    parser.add_argument("resources", type=Path, help="Heroes III Complete root containing Data and MP3")
    parser.add_argument("output", type=Path, help="New, empty case directory")
    parser.add_argument("--case", choices=POOLS, default="replay")
    parser.add_argument("--seed", type=int, default=2048)
    parser.add_argument("--timeout", type=float, default=720, help="Maximum seconds to wait")
    args = parser.parse_args()
    case = args.output.resolve()
    case.mkdir(parents=True, exist_ok=False)
    game = case / "game"
    profile = case / "profile"
    with zipfile.ZipFile(args.package) as archive:
        archive.extractall(game)
    profile.mkdir()
    # Only copy base-game media. Never link the installed data or copy a user's saves.
    (profile / "Data").mkdir()
    for name in ("h3bitmap.lod", "h3sprite.lod", "h3ab_bmp.lod", "h3ab_spr.lod",
                 "Heroes3.snd", "h3ab_ahd.snd", "VIDEO.VID", "h3ab_ahd.vid"):
        shutil.copy2(args.resources / "Data" / name, profile / "Data" / name)
    if (args.resources / "MP3").is_dir():
        shutil.copytree(args.resources / "MP3", profile / "MP3")
    (profile / "Maps").mkdir()
    (profile / "config").mkdir()
    (profile / "config/settings.json").write_text(json.dumps({"general": {"sound": 0, "music": 0}, "video": {"fullscreen": False, "showIntro": False}, "battle": {"speedFactor": 12}}))
    (game / "config/dirs.json").write_text(json.dumps({"userDataPath": str(profile)}))
    catalog = game / "Mods/arenabattles/Content/config/evolution.json"
    data = json.loads(catalog.read_bytes())
    if POOLS[args.case]:
        data["mutations"] = {key: data["mutations"][key] for key in POOLS[args.case]}
    if args.case == "defeat":
        # Keep the normal loss path and force an unwinnable encounter on wave five.
        data["boss"]["effects"].extend([{"type": "STACKS_SPEED", "val": 100}, {"type": "STACK_HEALTH", "val": 100000}, {"type": "CREATURE_DAMAGE", "subtype": "creatureDamageBoth", "val": 100000}])
    if POOLS[args.case]:
        catalog.write_text(json.dumps(data))
    map_file = profile / "Maps/EvolutionArena.vmap"
    previous = None
    deadline = time.monotonic() + args.timeout
    report = case / "maps.jsonl"
    env = dict(os.environ, EVOLUTION_SEED=str(args.seed))
    info = subprocess.STARTUPINFO()
    info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    info.wShowWindow = 0
    with (case / "stdout.txt").open("wb") as out, (case / "stderr.txt").open("wb") as err:
        game_proc = subprocess.Popen([str(game / "VCMI_client.exe"), "--evolution-check", "--nointro"], cwd=game, env=env, startupinfo=info, stdout=out, stderr=err)
        try:
            while game_proc.poll() is None:
                if time.monotonic() > deadline:
                    raise TimeoutError("Client exceeded the configured time budget")
                try:
                    digest = hashlib.sha256(map_file.read_bytes()).hexdigest()
                    if digest != previous:
                        snapshot = record_map(map_file)
                        with report.open("a", encoding="utf-8") as stream:
                            stream.write(json.dumps(snapshot) + "\n")
                        previous = digest
                except (OSError, ValueError, zipfile.BadZipFile):
                    pass  # The client replaces this file between waves.
                time.sleep(0.2)
        finally:
            if game_proc.poll() is None:
                game_proc.terminate()
                game_proc.wait()
    print(f"{args.case} seed {args.seed}: client exit {game_proc.returncode}", flush=True)
    log = (profile / "logs/VCMI_Client_log.txt").read_text(encoding="utf-8", errors="replace")
    rows = re.findall(r"Evolution playtest: wave (\d+), victory (\d), roster (\d+), casualties (\d+)", log)
    expected = 5 if args.case == "defeat" else 15
    sequence = [int(row[0]) for row in rows]
    if sequence != list(range(1, expected + 1)):
        raise RuntimeError(f"Unexpected wave sequence: {sequence}")
    for wave, victory, roster, casualties in rows:
        should_win = args.case != "defeat" or int(wave) != expected
        if bool(int(victory)) != should_win:
            raise RuntimeError(f"Unexpected outcome at wave {wave}")
    snapshots = [json.loads(line) for line in report.read_text().splitlines()]
    if len(snapshots) != expected:
        raise RuntimeError(f"Expected {expected} battle maps, captured {len(snapshots)}")
    start = snapshots[0]["army"][0][0]
    if start["amount"] != 24 or start.get("evolutionEffects"):
        raise RuntimeError("New attempt inherited a roster or a mutation")
    for wave, snapshot in enumerate(snapshots, 1):
        enemies = snapshot["army"][1]
        expected_boss = []
        if wave % 5 == 0:
            rotation = [data["boss"], *data["otherBosses"]]
            expected_boss = rotation[(wave // 5 - 1) % len(rotation)]["effects"]
        if enemies[0].get("evolutionEffects", []) != expected_boss or any(unit.get("evolutionEffects") for unit in enemies[1:]):
            raise RuntimeError(f"Unexpected boss or escort effects on wave {wave}")
    effects = [snap["army"][0][0].get("evolutionEffects", []) for snap in snapshots]
    if args.case == "fortress" and not any(e.get("subtype") == "combatEventBeforeAttacked" for group in effects for e in group):
        raise RuntimeError("Living Fortress was not observed")
    if args.case == "mirror" and not any(
        sum(e["type"] == "FIRE_SHIELD" for e in group) >= 2
        and sum(e["type"] == "LIFE_DRAIN" for e in group) >= 2 for group in effects
    ):
        raise RuntimeError("Blood Mirror was not observed")
    return int(game_proc.returncode != (1 if args.case == "defeat" else 0))


if __name__ == "__main__":
    raise SystemExit(main())
