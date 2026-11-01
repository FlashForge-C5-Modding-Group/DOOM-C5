# Doom for the Creator 5

Doom on the touchscreen, with sound on the buzzer. It's built on [doomgeneric](https://github.com/ozkl/doomgeneric) (GPLv2) and uses the free [Freedoom](https://freedoom.github.io/) game data (BSD).

## Install

Build the package with `package.sh`, copy `dist/c5doom/` to `/usr/data/doom/` on the printer, then run:

```sh
/usr/data/doom/doom.sh
```

To use another game file, put it in the same folder and run `DOOM_WAD=doom1.wad /usr/data/doom/doom.sh`.

`doom.sh` stops GrumpyScreen while the game runs and brings it back on exit. It won't start during a print, and it quits the game if a print starts. To exit, use MENU > Quit Game > YES.

## Controls

The game fills the screen, and the controls are drawn over it:

```
                                 +------+-----+-----+
       +----+                    | MENU | MAP | YES |
       | ^  |                    +------+-----+-----+
  +----+----+----+               |  OK  | WPN | USE |
  | <  |    | >  |               +------+-----+-----+
  +----+----+----+               |       FIRE       |
       | v  |                    +------------------+
       +----+
```

- **Cross:** up and down walk, left and right turn. Its corners press two directions at once, so you can slide a finger across it.
- **WPN:** picks the next weapon slot on each press.
- **Menus:** the cross moves, OK selects, and YES answers prompts.

What the touch panel sends is recorded in `/tmp/c5doom-touch.log`, up to its first 20,000 events. Set `DOOM_TOUCH_LOG` to use another path.

## Keyboard

USB keyboards work, plugged in before or during the game. The keys are classic Doom:

| Key | Action |
|---|---|
| Arrows | Walk and turn |
| Ctrl | Fire |
| Space | Use |
| Shift | Run |
| Alt + left or right | Strafe |
| `,` and `.` | Strafe left and right |
| 1 to 7 | Pick a weapon |
| Tab | Map |
| Esc | Menu |
| Enter | Select in menus |
| Y and N | Answer prompts |

## Sound

The buzzer plays one tone at a time. Effects use the game file's PC speaker sounds, and music plays its highest note. An effect interrupts the music while it plays. Tones are set by running `cmd_pwm`, the same tool Klipper's buzzer uses, so very fast changes get skipped.

## Settings

Set these as environment variables before `doom.sh`:

| Variable | Default | Meaning |
|---|---|---|
| `DOOM_ROTATE` | `1` | `3` turns the picture and touch upside down |
| `DOOM_BEEP_LEVEL` | `60` | Buzzer loudness, 0 to 100 |
| `DOOM_WAD` | `freedoom1.wad` | Game file |

## Build

`package.sh` runs on Linux or WSL. It needs the `mips32el--musl--stable-2024.02-1` toolchain in `~/toolchains`, and `python3` and `curl`. It builds with `build.sh`, downloads Freedoom, and puts everything in `dist/c5doom/`. The doomgeneric source is in `third_party/`, with `i_sound.c` changed to drop SDL and turn music on.

To run on the PC with a simulated screen instead, use `build.sh host`.
