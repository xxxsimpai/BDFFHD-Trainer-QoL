# BDFFHD Trainer + Native QoL Patch

An all-in-one Windows x64 trainer and native game-file patch for **Bravely Default Flying Fairy HD Remaster**.

The patch applies directly to supported game files without a mod loader, so Steam achievements remain enabled.

## Install

1. Extract the Portable ZIP.
2. Run `BDFFHDNativePatch.exe` once, choose the game's `GameAssembly.dll`, and let it update the game files. It keeps `.bdffhd-backup` copies beside every changed file.
3. Run `BDFFHDTrainer.exe` when you want the trainer overlay. It attaches to an open game, launches Steam-managed installs through Steam, or directly starts non-Steam installs. It remembers the installation path.

The native gameplay and QoL changes remain in the game files after installation. After patching, start the game through Steam or launch it with BDFFHDTrainer.exe. The native patcher is not needed during play, and no BepInEx, mod loader, or background patch service is installed. The trainer itself is still required for its live menu and cheats.

The patcher supports verified GameAssembly builds only and stops without changing files if the build does not match. If the game updates, restore the backup or use Steam's file verification before applying a compatible patch. The accessory/shop edits target the English/Common data tables.

## Native changes

- Weapon M.Atk now uses the equipped weapon's job correction, like the game's physical-attack calculation. The native hook preserves the game's original ability check and applies to player characters and friend summons. Inspired by Demojay's [Enable MAtk Weapon Proficiency](https://www.nexusmods.com/bravelydefaultflyingfairyhdremaster/mods/5); this package uses a native GameAssembly patch, not their BepInEx plugin.
- Field-bubble and black-dialogue voice delays are removed; party-chat auto-advance is set to 0.5 seconds.
- Adds **Scouter**, a 20 pg accessory sold by the Adventurer once the Trader Village shop reaches Rank 1. While equipped by any party member, it automatically reveals full enemy information, including HP and weaknesses. It reuses a reserved dummy row and no longer takes a battle-item slot.
- Adds a separate **Petal Token** listing to Florem's shop for 500 pg; the Potion listing remains available.
- Twelve custom accessories are added progressively to Norende's **Accessory Shop** as it upgrades:
  - **Level 5:** Might Charm (+30 physical attack, +15 STR) and Arcanist Charm (+30 magic attack, +100 MP).
  - **Level 6:** Bulwark Charm (+50 to both defenses, +100 HP) and Assassin's Charm (+30 evasion, +15 speed).
  - **Level 7:** Healer's Charm (+30 speed, +100 MP) and Deadeye Charm (+50 aim, +15 DEX).
  - **Level 8:** Kleptomaniac's Charm (+30 AGI and its own Rob Blind effect while equipped, stacking separately with the learned ability for up to four independent steal rolls) and Angel's Charm (immunity to all status ailments, including Stop).
  - **Level 9:** Elemental's Charm (absorbs all seven elements).
  - **Level 10:** Grace of Gods (1,000,000 pg; keeps its wearer at 3 BP and waives BP costs, pausing during the Brave/Default tutorial and resuming afterward).
  - **Level 11:** Charm of Omnipotence (combines the six stat charms, Angel's, and Elemental's effects) and Charm of the Limit Breaker (damage cap of 99,999 without Limit Break or 999,999 with it).
  - **Prices:** Most charms cost 50,000 pg; Charm of Omnipotence and Charm of the Limit Breaker cost 500,000 pg each; Grace of Gods costs 1,000,000 pg.
- Charm entries reuse built-in dummy rows, keeping the item table within the game's 600-entry runtime limit. This also migrates the earlier 604-row release table back below the game's fixed inventory-array limit.
- Kleptomaniac uses its own cloned support-ability ID, so the charm effect and learned Rob Blind can each trigger the game's native common/rare item selection independently. With both active, each steal can make four rolls.
- Freelancer learns the regular **Steal** command at job level 1, so it is available from the beginning. Early field enemies can drop Potions commonly and Ethers rarely, while their existing drops—including Antidotes and Eye Drops—are retained. The asterisk bosses also have new steal rewards: Hi-Potions are the common steal, with a job-themed rare weapon or shield; the Spellfencer boss keeps Rune Blade as its common steal.
- The boss rare steals use the game's existing items: Defender (Knight), Wizard's Rod (Black Mage), Kunai (Thief), Sage's Staff (White Mage), Toxic Claws (Monk), Blessed Shield (Merchant), and Magic Knife (Spellfencer).

## Trainer

- F1 shows or hides the overlay. The window can be resized and moved; its size and position are remembered.
- The close button asks whether to unload the trainer or hide the menu. Hidden-menu hotkeys show a brief status toast at the top center of the screen.
- The charcoal overlay defaults to 35% transparency. Settings and default values are stored in `features.ini`.
- The **XP/Job** page combines rate controls and maxers, with separate **XP** and **Job** subtabs. **Stats** has **Party-wide** and **Individual** subtabs.
- Party slots are numbered 1–4; friend slots 1–20; computer-friend templates 1–32.
- Stat inputs default to 0, friendship to 255, Norende population to 999, XP to 2×, JP to 5×, consumables to 10, and weapons/armor/accessories to 4. Defaults can be changed in Settings or `features.ini`.
- Friend maxers update level/job level, displayed friend power, population, friendship, and the saved battle-stat snapshot for individual friends or all friends. The game-file patch keeps Abilink rows in the same order as the friend slots used by its link action. Save in-game after applying permanent changes.
- The Jobs page has party-wide and individual JP and job-level controls, plus a control to set one selected job on one selected character. Norende can set all shop/building levels or unlock everything.
- Item search lists matching items in a scrollable results area and labels their specific game category (for example, sword, hat, helmet, consumable, or asterisk).
- The message area reports toggle state and whether an action has been used since launch. Hover over **?** for the hotkey list.

### Hotkeys

| Key | Action |
| --- | --- |
| F1 | Show/hide overlay |
| F2 | Cure party status ailments |
| F3 / F4 | Fill party HP / MP |
| F5 / F6 | Toggle infinite HP / MP |
| F7 / F8 | Toggle infinite BP / guaranteed stealing |
| F9 / F10 | Apply XP / JP multiplier |
| F11 | Apply both multipliers |
| F12 | Reset XP/JP rates |

F2–F12 and NumPad 0–9 assignments can be changed in Settings. Each key can run any available menu option; the hotkey tooltip updates to show the current assignments.

Infinite HP, MP, BP, and stealing remain active across battles.

## Build

Requires Windows x64 and Visual Studio 2022 / Build Tools with the C++ workload.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

`tools/NativeGamePatch.cpp` builds the one-time native game-file patcher. The adjacent `tools/native_matk.asm` documents the injected native helper; keep its generated byte array in `NativeGamePatch.cpp` in sync when changing that helper. `tools/native_matk_patch.py` is the developer patch-generation utility and is not required by players.

## Distribution notes

The release archives do not contain the original game executable, GameAssembly, save files, or full game data tables. The one-time installer patches the user's existing files and creates backups. Use the game and trainer in offline single-player; live trainer features require the trainer to be open.

Existing friend-menu navigation behavior can vary by controller/game state. If the in-game Friends page stops responding, exit to the guest menu and save; this trainer does not replace the game's Friends page UI.

## Third-party code

MinHook 1.3.4 is compiled into the trainer payload. Its BSD license notice is included in `MinHook-LICENSE.txt`, and its source/license are in `third_party/minhook`. Upstream: https://github.com/TsudaKageyu/minhook

## Inspiration and mod credits

The following community mods informed feature ideas and compatibility references. The patcher reimplements the Scouter, Petal Token, M.Atk, and charm/shop changes natively; it does not include their mod archives or code.

- Demojay: [Summoner Job Tweaks](https://www.nexusmods.com/bravelydefaultflyingfairyhdremaster/mods/1), [Magnifying Glass](https://www.nexusmods.com/bravelydefaultflyingfairyhdremaster/mods/3) (our item is named **Scouter**), [Enable MAtk Weapon Proficiency](https://www.nexusmods.com/bravelydefaultflyingfairyhdremaster/mods/5), [Black Resonance Support Ability Buff](https://www.nexusmods.com/bravelydefaultflyingfairyhdremaster/mods/6), and [Buyable Petal Tokens](https://www.nexusmods.com/bravelydefaultflyingfairyhdremaster/mods/7).
- thefkyouguy: [Single Point Abilities](https://www.nexusmods.com/bravelydefaultflyingfairyhdremaster/mods/8).
