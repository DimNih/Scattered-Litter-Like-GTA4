# Rubbish GTA IV 64

AML mod for **GTA: San Andreas Android** that adds roadside litter and loose paper around the player. Paper can drift continuously, flutter down under gravity, settle on the ground, and lift again when a nearby running ped or moving vehicle disturbs it.

> **Status:** experimental. Tested on GTA SA Android 2.10 ARM64 by the project author. Compatibility with other game/AML versions may differ.

## Screenshots

| Roadside litter | Ground paper | Paper on terrain |
| --- | --- | --- |
| ![Roadside litter near CJ](review/review1.jpeg) | ![Litter placed around the player](review/review2.jpeg) | ![Paper lying on the ground near player](review/review3.jpeg) |

## Features

- Spawns litter around the player, using road nodes and nearby open ground such as grass and roadside areas.
- Spawns a configurable crowd of paper sheets that drift, tumble, and fall under gravity.
- Nearby pedestrians and vehicles can kick paper back into the air; ordinary rubbish has its own lower count limit.
- Paper mix is configurable, with some sheets staying airborne and others landing on the ground.
- Paper collision is disabled, so it does not block or damage passing vehicles.
- Varies built-in model choice, heading, and object scale.
- Limits the number of spawned objects and removes tracked objects outside the cleanup radius.
- Supports configuration through an AML `.ini` file.

## Requirements

- GTA: San Andreas Android 2.10
- AML (Android Mod Loader) New
- ARM64bit

## Installation

1. Copy `libRubbishGtaIV64.so` into the AML mods folder for the game.
2. Copy `RubbishGtaIV64.ini` to the AML configuration folder.
3. Launch the game. If it behaves unexpectedly, check the game's Logcat output.

## Configuration

The sample configuration is in [`RubbishGtaIV64.ini`](RubbishGtaIV64.ini). Main settings:

| Section | Setting | Default | Description |
| --- | --- | ---: | --- |
| `General` | `Enabled` | `1` | Enable or disable litter spawning. |
| `General` | `MaxObjects` | `100` | Total tracked object limit. |
| `General` | `MaxRubbishObjects` | `30` | Limit for ordinary road and ground litter. |
| `General` | `MinSpawnDistance` | `28.0` | Minimum player-to-litter distance. |
| `General` | `SpawnRadius` | `70.0` | Maximum spawn radius. |
| `General` | `MinObjectSpacing` | `2.5` | Minimum spacing between litter pieces. |
| `General` | `RoadLateralRange` | `16.0` | Maximum offset from road-path locations. |
| `General` | `CleanupRadius` | `90.0` | Objects beyond this distance are removed. |
| `General` | `SpawnIntervalMs` | `1300` | Delay between ordinary litter spawns. |
| `Models` | `PaperModelId` | `1577` | Game model ID used for paper. |
| `Paper` | `MaxPaperObjects` | `55` | Maximum tracked paper sheets. |
| `Paper` | `SpawnIntervalMs` | `450` | Delay between paper spawn attempts. |
| `Paper` | `ContinuousFlightPercent` | `35` | Chance for paper to keep riding the wind instead of falling. |

Values are clamped by the mod. Ordinary litter uses road-path nodes and nearby ground queries, so placement depends on the terrain and available road nodes.
