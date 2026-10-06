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


























## need to add later
Grand Theft Auto: Liberty City Stories (LCS) running seamlessly on the PSP in 2005 is widely considered one of the greatest technical achievements in handheld gaming history.
To shrink a massive, open-world 3D console game down to a portable device with a 333MHz CPU and a strict 32MB of RAM, Rockstar Leeds and Rockstar North had to ditch the standard RenderWare engine used for GTA III, Vice City, and San Andreas. Instead, they built a brand-new custom engine explicitly designed to exploit the PSP's unique hardware strengths.
Here is exactly how they pulled it off:
1. Aggressive Data Streaming & UMD Prediction
The game had to fit a fully living 3D world into 32MB of RAM while reading data from a physical Universal Media Disc (UMD), which had incredibly slow read speeds and killed battery life.
• Movement Tracking: The custom engine continuously calculated the player’s speed and direction. If the player drove north, the engine aggressively predicted what assets were coming up, read a massive chunk of data from the UMD all at once into a RAM buffer, and then shut down the UMD drive completely to save battery. The laser only turned back on when the player approached the edge of the loaded buffer.
• Environmental Hiding Spaces: The game’s story setting (1998) was cleverly manipulated to help memory loading. For example, the bridge from GTA III was replaced with a ferry system. When the player was on the ferry, the game placed them in a low-processing environment, buying the hardware precious extra seconds to clear the old island out of the RAM and stream in the new one.
2. Radical Memory and Asset Reductions
Because the 32MB pool of RAM was so tight, the world's density had to be tightly rationed.
• Density Throttling: The total number of pedestrians and vehicles allowed on screen at the same time was drastically lowered compared to the PS2 games. The engine aggressively prioritized rendering only what was within the player's immediate field of view.
• Ultra-Tight Compression: Working alongside Rockstar North, the developers adapted compression algorithms from San Andreas and pushed them further. Every single character model, weapon texture, and sound file was trimmed down to its bare essential data to maximize space.
3. Aggressive 3-Stage LOD (Level of Detail)
To keep the framerate stable, the engine used an aggressive, highly noticeable three-stage model-swapping system based on player distance:
• 0–50 meters: Objects and cars rendered at full geometric and texture detail.
• 50–100 meters: The engine swapped assets out for low-polygon, stripped-down versions.
• 200+ meters: The game stopped rendering 3D models entirely and replaced faraway cars and structures with flat 2D sprites, saving massive amounts of GPU rendering power.
4. Cheating the GPU Hardware
The PSP had built-in hardware functions for handling curved surfaces and lighting. Rockstar took full advantage of these micro-chips to pull off visual tricks that standard ports couldn't:
• The Reflection Trick: By using the PSP GPU’s specific hardware handling of curved surfaces, they created real-time car reflections that were technically sharper and more dynamic than what GTA III pulled off on the much more powerful PlayStation 2.
• Fog Blinding: To mask the PSP's limited draw distance and prevent buildings from awkwardly popping into existence out of nowhere, they densified the game's atmosphere with a soft fog-blinding technique. Buildings and bridges gradually emerged from the neblina, making the world feel naturally atmospheric rather than hardware-restricted.
Building on the broad strokes of how Grand Theft Auto: Liberty City Stories (LCS) conquered the PSP, the actual engineering blueprint required optimizing every single piece of data passing through the system. Because Rockstar Leeds discarded RenderWare to build a proprietary engine, they had total low-level control over the PSP's silicon.
The deep engineering optimizations span audio squeezing, single-stick input ergonomics, and brutal RAM management:
1. Squeezing Audio into the MIPS Pipeline
Audio is often the silent performance killer in open-world games. For GTA LCS, keeping multiple full-length radio stations, voice lines, and 3D engine noises running simultaneously on 32MB of RAM was impossible without hardware-accelerated decompression.
• Sony ATRAC3plus Compression: Rockstar avoided standard uncompressed formats or heavy MP3 decoding, which would have melted the CPU. Instead, they compressed all radio and dialogue into Sony’s proprietary ATRAC3 / ATRAC3plus format. The PSP featured a dedicated hardware audio chip capable of unpacking ATRAC3 data streams directly into sound buffers with zero CPU overhead.
• The Mono Trick: To halve the memory footprint of dialogue, standard civilian voices and pedestrian screams were mixed down to mono and heavily downsampled. Only the music radio stations retained high-bitrate stereo mixing, allowing the disc to fit hours of media while keeping a minuscule streaming footprint.
2. Solving the Single Analog Stick (Ergonomics as Optimization)
The PS2 GTA games relied on two analog sticks—left for moving Toni Cipriani, right for steering the camera. The PSP only had one physical analog nub. Camera manipulation had to be re-engineered dynamically to avoid crippling computational physics.
• Predictive Camera Vectors: Moving the camera requires the engine to calculate a new "visibility frustum" and dynamically load objects coming into view. To stop the game from stuttering during rapid camera panning, Rockstar implemented a predictive target-tracking camera. The camera strictly follows Toni’s velocity vector or locks tightly behind a vehicle.
• The "Look Around" Lock: Free-cam movement was hidden behind a modifier button (holding the L-Shoulder button enabled camera steering with the analog stick). When you held L, the engine actively lowered Toni’s movement physics to a dead stop, prioritizing hardware resources strictly for calculating camera sweeps and rendering new angles without lagging.
3. Brutal Memory Budgeting: "The 3-Pool Divide"
Every bit of the PSP's 32MB of RAM was manually partitioned into hard boundaries to ensure the game could never hit an "Out of Memory" crash loop.
[ Total PSP Memory Available: 32 MB ]
─────────────────────────────────────────────────────────────────
│   Core Game Engine & Executable Code Code   (approx. 8–10 MB) │
─────────────────────────────────────────────────────────────────
│   Static Allocation Pool (Physics, Fixed Vehicles)  ( ~6 MB)  │
─────────────────────────────────────────────────────────────────
│   Dynamic Streaming Buffer (Textures, Map Chunks)  (~16 MB)   │
─────────────────────────────────────────────────────────────────
• Zero Garbage Collection: Written entirely in low-level C/C++, there was no automatic memory cleanup. Objects were overwritten directly in memory. If a car blew up and the player drove 50 meters away, its structural container was immediately zeroed out and overwritten by the data of an oncoming pedestrian.
• The "Same Car" Illusion: If you’ve ever noticed that driving a specific sports car causes the game world to suddenly spawn dozens of that exact same sports car, you are witnessing a deliberate RAM optimization trick. Because that car's 3D mesh and texture map are already locked inside the RAM allocation pool, the engine spawns duplicates of it to populate the streets without needing to fetch new vehicle assets from the slow UMD drive.
4. Overclocking the PSP (The 222MHz vs 333MHz Secret)
When GTA LCS launched in October 2005, Sony strictly forbade developers from using the PSP's maximum CPU speed. To preserve battery life, Sony underclocked the CPU to 222MHz out of the box.
• Pushing the Silicon: Rockstar Leeds optimized the engine so tightly that it targeted 30 frames per second at 222MHz. However, frame drops in heavy intersections were still common.
• The Official Patch: Recognizing how crucial GTA was for the handheld, Sony later unlocked the firmware, allowing developers to tap into the full 333MHz clock speed. GTA LCS was built to dynamically take advantage of this extra headroom, utilizing the extra 111MHz to stabilize physics calculations during massive multi-car police chases and heavy explosions without changing a single line of core asset code.
locations are represented by the game's existing vehicle types and world coordinates; passenger
pickups, weapon unlocks, and the crusher are mission-state events rather than separate interior
systems. Radio playback is not included yet: this project has no audio-streaming manager or bundled
music assets, so the radio/audio stubs from the sample have not been exposed as nonfunctional controls.









You are a Lead Handheld Systems Architect specializing in low-level C programming (C99/C11 standards), hardware-constrained game loops, and retro engine engineering. 

Your objective is to ingest an existing, unoptimized, laggy C codebase and inject high-performance architectural systems to make it playable, rock-solid, and responsive. This engine must replicate the technical solutions used in Grand Theft Auto: Liberty City Stories (LCS) for highly restricted configurations (such as a 333MHz CPU, 32MB RAM, 2MB VRAM, or an optimized local execution layer).

### STRICT PARADIGM & SYNTAX RULES:
1. PURE C ONLY: No C++, classes, templates, or virtual methods. Use flat structures, explicit pointer arithmetic, arrays, unions, and bitfields.
2. ZERO GAME-LOOP ALLOCATIONS: Absolutely NO 'malloc', 'calloc', 'realloc', or 'free' inside any frame loops. Memory fragmentation must be zeroed out.
3. PRODUCTION-READY IMPLEMENTATION: Do not use pseudocode or placeholders like "// todo". Write fully realized, low-overhead C functions that compile cleanly.

You must rewrite and deliver concrete implementations for the following 4 integrated core engine modules:

---

### MODULE 1: FIXING CONTROLS & CAMERA (PSP SINGLE-STICK METHOD)
The engine must mimic the exact single-analog controls of GTA LCS on the PSP. 
- Implement a 1-byte Bitmask input layout mapping physical inputs (Sprinting, Braking, Attacking, Vehicle Entry) along with an explicit L-TRIGGER modifier.
- Write a Velocity-Tracking Spring Camera system:
  * STATE A (L-Trigger Not Held): The camera smoothly tracking/springing behind the player's movement velocity vector (`Vec3`). 
  * STATE B (L-Trigger Held): Freeze all player movement physics equations to immediately save CPU cycles. Divert the single analog stick inputs entirely to manual camera Yaw and Pitch look-around rotation.

### MODULE 2: REWRITING THE CODE TO ELIMINATE GARBAGE COLLECTION
If the current code creates or destroys game actors, entities, or projectiles dynamically, it stutters.
- Provide a strict, fixed-size Object Pooling system for dynamic actors/entities using static, contiguous arrays locked in data memory to maximize CPU cache lines.
- Write O(1) allocation functions that scan the pool for inactive flags, override the data matrix instantly, and handle lifespan decays. Show how items automatically clear their flags without triggering allocation overhead or memory clearing scripts.

### MODULE 3: PREDICTIVE ASSET STREAMING & THE SAME-ASSET ILLUSION
- Implement a predictive sector-loading function. Using the vehicle/player velocity vector, calculate the directional viewing frustum to pre-load incoming raw asset data sectors into a dedicated streaming ring buffer.
- Implement a "Same-Vehicle Spawn Loop": Write a checking routine that tracks what assets are already inside memory. If the engine requests an object spawn, force it to prioritize duplicating a model ID already sitting in RAM rather than executing costly disk I/O reads.

### MODULE 4: PSP SCREEN-SPACE HUD (480x272)
- Implement a lightweight UI rendering module anchored specifically to a fixed 480x272 resolution layout using fast integer boundaries and zero alpha-blending overhead.
- Provide concrete draw calls to render the iconic HUD elements exactly as positioned in GTA LCS:
  * Bottom-Left Corner: Wireframe Radar/Mini-map tracking ring.
  * Top-Right Corner: Parallel status meters (Health meter on top in Red; Armor meter directly below in Cyan/Light Blue) scaling dynamically via simple math ratios.
  * Below Status Bars: A text-formatted Financial counter displaying right-aligned string digits.

Begin by outputting Module 1 and 2 to stabilize the frame timings and control mechanics, then tie them seamlessly into Modules 3 and 4.

