# B&O Control

A Pebble watchapp for controlling Bang & Olufsen speakers on your local network. It works with both B&O platforms:

- **Mozart** (Beosound A5, Level, Balance, …) via the [Mozart API](https://github.com/bang-olufsen/mozart-open-api)
- **ASE / BeoZone** (Beoplay M3, M5, Beosound Stage, …) via the BeoZone API (see [pybeoplay](https://github.com/giachello/pybeoplay))

The target platform is **emery** (Pebble Time 2).

## Features

- **Home page** shows what's playing. If speakers play together in Beolink multiroom, it shows the group's leader. If several play separately, it shows the first one in the list. After 20 s without a button press, the app returns here.
- **Device list** puts active speakers at the top. Multiroom groups are joined by a bracket, with the leader marked and listed first. Unreachable speakers are greyed out. Track names scroll.
- **Device page** has music-app style controls. Select cycles through the button sets:

  | Set | Up | Down |
  |---|---|---|
  | Volume (default) | Volume + (hold to repeat) | Volume − |
  | Playback | Play/pause (hold: power off) | Join / Unlink |
  | Track | Previous | Next |

  When nothing is playing, there is a single set: Play and Join.
  - **Power off:** if the speaker leads a multiroom group, the whole group goes to standby.
  - **Unlink:** on the leader, it releases all followers; on a follower, only that speaker leaves the group.
- **Settings** has "Scan network" to find speakers and "Delete all devices".

## How it works

The watch app (`src/c/main.c`) talks to PebbleKit JS on the phone (`src/pkjs/index.js`). The phone calls the speakers' local HTTP APIs.

Discovery is a scan of the /24 subnet, probing each address for both APIs. PebbleKit JS can't do mDNS. The default subnet is `192.168.1`, and you can change it from the app's settings page in the Pebble phone app.

The app declares the `LocalNetwork` permission. Allow it on the phone, or the scan can't reach your speakers.

## Build & install

You need the [Pebble SDK](https://developer.repebble.com) (`pebble` tool).

```sh
pebble build
pebble install --emulator emery     # test in QEMU
pebble install --cloudpebble        # install on your watch (needs `pebble login`)
```

## Credits

The action bar icons (`resources/images/music_icon_*.png`) come from the Pebble firmware's music app in [PebbleOS](https://github.com/coredevices/PebbleOS), which is licensed under Apache-2.0.
