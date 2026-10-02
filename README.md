# GameCube Menu C

Place IPL files in `Files/GameCube_BIOS/{USA,EUR,JAP}/IPL.bin`.

```sh
git submodule update --init --recursive
cmake -S . -B Files/build
cmake --build Files/build --parallel
Files/build/gamecube-menu
```

Metal is the macOS default. For GLES2, configure with `-DGC_BACKEND=gles2`;
EGL, GLES2 and X11 system development libraries are required on Linux.

```sh
Files/build/gamecube-menu --ipl Files/input/IPL.bin --region USA
Files/build/gamecube-menu --region EUR
Files/build/gamecube-menu --region JAP
Files/build/gamecube-menu --skip-startup
Files/build/gamecube-menu --delaystart
Files/build/gamecube-menu --delaystart 5
Files/build/gamecube-menu --delaystart --step
Files/build/gamecube-menu --frames 120
Files/build/gamecube-menu --startup-sound 0
Files/build/gamecube-menu --startup-sound 1
Files/build/gamecube-menu --startup-sound 2
Files/build/gamecube-menu --boot-state normal
Files/build/gamecube-menu --boot-state notice
Files/build/gamecube-menu --boot-state lost
Files/build/gamecube-menu --card-a Files/input-a.raw --card-b Files/input-b.raw
Files/build/gamecube-menu --noinsert a
Files/build/gamecube-menu --noinsert b
Files/build/gamecube-menu --noinsert ab
Files/build/gamecube-menu --disc Files/game.iso
Files/build/gamecube-menu --config Files/config.ini
Files/build/gamecube-menu --help
```

Arrow keys select. A/Enter confirms; B/Escape/Backspace cancels; S starts.
D inserts/ejects test media; E toggles the test error; Z selects the alternate
startup sound. With `--step`, comma rewinds, period advances, and Space plays/pauses.
Bare `--delaystart` waits for A, B, or an arrow key.

`Files/config.ini` is created automatically. Two empty cards are the default.
To enable dummy saves and scrolling:

```ini
[cards]
slot_a=present
slot_b=present
dummy_a=true
dummy_b=true
dummy_count_a=25
dummy_count_b=25
```

Slot values accept `present` or `absent`; counts accept 0–59.
`dummy_count=25` sets both counts. Imported cards use local shadow copies.

```sh
Files/build/gamecube-render --output Files/frame.ppm --page cube --time 2
Files/build/gamecube-render --output Files/calendar.ppm --region EUR --language en --page calendar
Files/build/gamecube-render --output Files/cards.ppm --card-a Files/input-a.raw --card-b Files/input-b.raw --page cards
Files/build/gamecube-render --output Files/disc.ppm --disc Files/game.iso --page disc
Files/build/gamecube-render --output Files/startup.ppm --ipl Files/input/IPL.bin --page startup --time 1
Files/build/gc-audio-tool --ipl Files/input/IPL.bin --output Files/audio.wav --seconds 10 --rate 48000 --startup 0
Files/build/gc-audio-tool --ipl Files/input/IPL.bin --output Files/event.wav --event 6
Files/build/gc-ipl-tool inspect Files/input/IPL.bin
Files/build/gc-ipl-tool prepare Files/input/IPL.bin Files/recovered
```

Render pages: `startup`, `cube`, `calendar-face`, `options-face`, `cards-face`, `disc-face`,
`calendar`, `options`, `cards`, `card-action`, `card-confirm`, `disc`.
Languages: `en`, `de`, `fr`, `es`, `it`, `nl`, `ja` (region dependent).

CC0-1.0. See [LICENSE](LICENSE).
