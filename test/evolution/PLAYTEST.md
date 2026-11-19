# Windows playtest, 19 November 2026

Tested gameplay revision `dd27eef50f4d6e92a301d1a4cf7044c4dba3fa37`. The public Windows x64 archive had SHA-256 `ad611a5f371abe241772e9e8abfadc78569be5cda6c7e57e644547c71dd39bac`. The client loaded private Heroes III Complete files from a separate local directory. The verification driver selected cards, used the animated BattleAI screen and accepted each battle result. The original game files were never uploaded.

Two stock runs on seed 2048 produced fifteen victories each. Wave number, victory, roster and cumulative casualties matched on every wave, including bosses 5, 10 and 15. Ignatius started with 24 imps. The final post-victory roster was 459 with 106 enemy casualties. The initial run used animation speed 6, and the replay used 12.

| Case | How the input differed from the distributed game | Observed result |
| --- | --- | --- |
| Stock replay | Unmodified catalog | Feeding Frenzy appeared on the player's stack. Battle logs recorded melee attacks and projectile animations. |
| Mirror | Six original mutations eligible | Blood Mirror contributed additional fire-shield and life-drain entries. The client played all 15 waves. |
| Fortress | Four original mutations eligible | The before-attacked defense trigger appeared alongside extra retaliation and defense before attack. Combat emitted stack-effect packets. The client played all 15 waves. |
| Growth | Harvest, Quickening and Scythe eligible | The map contained speed, adjacent attacks and a growing damage bonus. The client played all 15 waves. |
| Defeat | Three original mutations eligible and the fifth-wave boss received high damage, health and speed | Wave 5 ended with a loss. A new process in the same profile produced the same five battle reports, beginning with a wave-1 victory and a post-victory roster of 31. |

`windows-run.json` contains compact observations from the local client and the generated wave maps. The roster reported on defeat is the count entering the battle. Animation and network packet counts are diagnostic observations. They do not measure the marginal damage of an individual mutation.

## Reproduce

For a normal game use the instructions in `EVOLUTION-RU.md` on the gameplay branch. On a Windows development machine with Python 3.11 or newer, extract the GitHub Actions artifact once. Give the test script the inner portable ZIP, the path of a legally owned Heroes III Complete installation and a new output directory.

```powershell
python VerifyPackage.py EvolutionArena-Windows-x64.zip "D:\Games\Heroes III Complete" .\qa-stock --case replay --seed 2048
```

The other supported cases are `mirror`, `fortress`, `growth` and `defeat`. Each creates a fresh private copy. Keep the generated profiles local. The game verification exit code is 0 for reaching wave 15, 1 for defeat and 2 for an interrupted check. The Python runner returns success for the expected defeat in the loss fixture.

The tests do not exercise human tactical input, the player-facing defeat dialog or hours-long balance. Native combat still has fixed-width numbers. Extremely large reflected or drained damage can overflow legacy percentage calculations.
