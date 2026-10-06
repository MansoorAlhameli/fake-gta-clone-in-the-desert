# Liwa Sandbox - build notes

Files: `main.c` (gameplay and rendering), `VehicleSystem.h/.cpp` (input and vehicle math helpers), `Makefile`.

## Build on macOS
1. Install the toolchain (pspdev): prebuilt release from github.com/pspdev/pspdev, or build from source.
2. Put this in `~/.zshrc`, restart Terminal:
   ```
   export PSPDEV="$HOME/pspdev"
   export PATH="$PATH:$PSPDEV/bin"
   ```
3. In this folder run `make`. You get `EBOOT.PBP`.

## Run
- Emulator: open `EBOOT.PBP` in PPSSPP (ppsspp.org).
- Real PSP (custom firmware): copy the folder to `ms0:/PSP/GAME/LiwaSandbox/`
  (the folder name matters: story-mode save data goes to `ms0:/PSP/GAME/LiwaSandbox/save.dat`).
- Main menu: use Up/Down to select Story Mode, Free Roam, Gallery, Options, or Quit; press X to confirm.
- Options: choose a starting area (City is the default; Liwa Dunes and Airport are also available), rebind controls, select a player outfit, or adjust camera sensitivity. Use Up/Down to select, Left/Right or X to change values, and O to return.

## If the build fails
- Linker error mentioning `msx`: the HUD font symbol is not exported by your libpspdebug. Tell me and I will embed a font.
- Any other error: send me the first 10 lines of the compiler output.

## Controls (defaults, remappable in the menu)
- Nub: move / steer.  X sprint / accelerate.  [] brake / reverse.  O fire / handbrake.  /\ enter-exit / shop.
- L hold: free look (tap: recenter).  R hold: lock-on (sniper: first-person aim).  D-pad L/R: weapon, lock target, plane rudder.  D-pad Down: honk while driving.
- SELECT: camera distance.  START: photo mode (X shoot, START exit).
- In a car: L + nub left/right looks out the windows, L + O drive-by.
- Planes: nub = roll/pitch, X/O throttle up/down, D-pad L/R rudder.
- On foot, Triangle enters a nearby vehicle; while driving, stop and press Triangle to exit. Pedestrians use the placeholder character and yield to approaching traffic. The city has visible gun shops, a hospital, and a police department; being wasted or busted shows a full-screen message before respawning at the nearest matching service.
- The city starts with a small pedestrian crowd, including walkers that circle clear blocks, plus randomized traffic on a marked road loop. Traffic obeys timed signals and stop signs, cruises slowly on local streets, and moves slightly faster on the arterial. Rendering is capped at 30 FPS while input and simulation continue at each display refresh.
- The open world starts at 08:00 and advances at one game minute per real second. Weather periodically shifts between clear, cloudy, sandstorm, and rain; it changes sky/fog presentation, and rain reduces vehicle grip. Up to 24 ambient/mission pedestrians share a fixed pool: ambient pedestrians choose destinations, flee gunfire or nearby fast vehicles, and are despawned when they are more than 340 m away. New city pedestrians appear outside the near-camera range. The wanted system supports up to six stars.

## Story missions
Walk onto a yellow mission beacon on foot with no wanted level to start. The ten missions use the
existing vehicle/NPC pools and mission state machine:

1. **Sands of Arrival** - board the border SUV, visit the safehouse, then reach the compound.
2. **Desert Courier** - honk near the runner at the oasis and deliver them to the date farm.
3. **Dune Retaliation** - clear the camp using unarmed melee, then return to the safehouse
   to unlock a stronger melee attack.
4. **Ambush at the Oasis** - clear the construction site, take the buggy, respray it, and park it.
5. **Sandstorm Rescue** - obtain a car, collect the crew before the 180-second timer expires,
   lose the wanted level, and deliver them.
6. **The Trapped Buggy** - escape the police and take the buggy to the scrap-yard crusher.
7. **Oasis Shake Down** - wear the Tan outfit, board the van, and honk at seven vendors before
   the 300-second timer expires; eliminate rival waves when they attack.
8. **Escaping the Dunes** - board the Armored SUV, evade police, and reach the estate to unlock
   the existing assault-rifle slot as the Micro-SMG stand-in.
9. **Eye in the Dunes** - collect the camera, tail the merchant truck, take two photos, and return.
10. **Clash at the Oasis** - clear the foot patrols and both armed trucks.

Mission progress and cash are saved in Story Mode. Some specified vehicles and interactive
locations are represented by the game's existing vehicle types and world coordinates; passenger
pickups, weapon unlocks, and the crusher are mission-state events rather than separate interior
systems. Radio playback is not included yet: this project has no audio-streaming manager or bundled
music assets, so the radio/audio stubs from the sample have not been exposed as nonfunctional controls.
