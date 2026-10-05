# fake-gta-clone-in-the-desert

i want to create a gta clone set in the uae in the deserts of liwa but be able to play this on a psp so im using an ai to help me but if you want to help me just message me on instagram on twitter my username on both is 

@S1RB4T

# Liwa Sandbox - build notes

Files: `main.c` (whole game), `Makefile`.

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
- Options: choose a starting area (Liwa Dunes, City, or Airport), rebind controls, select a player outfit, or adjust camera sensitivity. Use Up/Down to select, Left/Right or X to change values, and O to return.

## If the build fails
- Linker error mentioning `msx`: the HUD font symbol is not exported by your libpspdebug. Tell me and I will embed a font.
- Any other error: send me the first 10 lines of the compiler output.

## Controls (defaults, remappable in the menu)
- Nub: move / steer.  X sprint / accelerate.  O fire / brake.  [] jump / handbrake.  /\ enter-exit / shop.
- L hold: free look (tap: recenter).  R hold: lock-on (sniper: first-person aim).  D-pad L/R: weapon, lock target, plane rudder.
- SELECT: camera distance.  START: photo mode (X shoot, START exit).
- In a car: L + nub left/right looks out the windows, L + O drive-by.
- Planes: nub = roll/pitch, X/O throttle up/down, D-pad L/R rudder.

## Missions (placeholders)
Walk onto a yellow beacon on foot with no wanted level. Three starter missions: Tail the Cruiser,
Paparazzi, Respray Job. Replace them with your own in the "PLACEHOLDER MISSIONS" block of `main.c`.
