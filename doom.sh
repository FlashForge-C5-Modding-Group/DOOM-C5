#!/bin/sh
# Runs Doom on the Creator 5 touchscreen in place of GrumpyScreen, and puts
# GrumpyScreen back afterwards. Won't start during a print, and quits if one starts.
cd "$(dirname "$0")" || exit 1
MOONRAKER=http://127.0.0.1:7125

printing() {
    curl -fs -m 3 "$MOONRAKER/printer/objects/query?print_stats=state" 2>/dev/null |
        grep -q -E '"state": *"(printing|paused)"'
}

if printing; then
    echo "A print is running, not starting Doom" >&2
    exit 1
fi

TOUCH=$(awk '
    /^N: Name=/ { name = tolower($0) }
    /^H: Handlers=/ && name ~ /goodix|touch/ {
        for (i = 2; i <= NF; i++)
            if ($i ~ /^event[0-9]+$/) { print "/dev/input/" $i; exit }
    }' /proc/bus/input/devices 2>/dev/null)
export DOOM_TOUCH=${TOUCH:-/dev/input/event2}
export HOME="$PWD"
export DOOM_TOUCH_LOG=${DOOM_TOUCH_LOG:-/tmp/c5doom-touch.log}

# Hold the loop that restarts GrumpyScreen, then stop GrumpyScreen itself.
# Letting the loop go again is all it takes to bring the screen back.
LOOP=$(pgrep -f 40-grumpyscreen.sh | head -n 1)
WATCHER=

restore() {
    [ -n "$WATCHER" ] && kill "$WATCHER" 2>/dev/null
    cmd_pwm set_wc pc12 1 0 2>/dev/null
    cmd_pwm disable_channels pc12 2>/dev/null
    [ -n "$LOOP" ] && kill -CONT "$LOOP" 2>/dev/null
}
trap restore EXIT
trap 'exit 0' INT TERM

[ -n "$LOOP" ] && kill -STOP "$LOOP"
for pid in $(pgrep -f /usr/data/grumpyscreen/grumpyscreen); do
    case "$(readlink "/proc/$pid/exe")" in
        */grumpyscreen*) kill "$pid" ;;
    esac
done
sleep 1

# Below Klipper's priority, so the game can't starve the printer
nice -n 10 ./c5doom -iwad "${DOOM_WAD:-freedoom1.wad}" "$@" &
DOOM=$!

( while kill -0 "$DOOM" 2>/dev/null; do
      if printing; then kill "$DOOM"; break; fi
      sleep 3
  done ) &
WATCHER=$!

wait "$DOOM"
