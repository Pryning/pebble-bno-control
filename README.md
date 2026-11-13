# B&O Control for Pebble

A Pebble watchapp for controlling Bang & Olufsen speakers on your home network. You can see what's playing and change the volume, play/pause, skip, join or leave Beolink multiroom groups, and turn speakers off, all from your wrist.

It supports both B&O platforms:

- **Mozart** (Beosound A5, Level, Balance, Emerge, …)
- **ASE** (Beoplay M3, M5, Beosound Stage, Core, …)

Target platform: **emery** (Pebble Time 2).

> Unofficial project. Not affiliated with or endorsed by Bang & Olufsen.

## Features

- **Home page:** the controls for whatever is playing. If speakers play together in a Beolink group, it shows the group's leader. If several play separately, it shows the first one in the list. When nothing is playing, it says so. The app returns here after 20 s without a button press.
- **Switching speakers:** when several speakers or groups play separately, hold volume + / − on the home page to switch between them. The buttons then show ▲ / ▼ for 3 s, and a short press switches again. The home page remembers your choice.
- **Device list:** hold Select (•••) on any page. Active speakers come first, Beolink groups are joined by a bracket with the leader on top, and unreachable speakers are greyed out.
- **Controls** like the Pebble music app. Press Select to cycle the button sets:

  | Set | Up | Down |
  |---|---|---|
  | Volume (default) | Volume + | Volume − |
  | Playback | Play/pause, hold to power off | Join / Unlink |
  | Track | Previous | Next |

  - **Power off:** on a group leader, the whole group goes to standby.
  - **Unlink:** on a group leader, all followers are released; on a follower, only that speaker leaves.
  - **Play** on a Mozart speaker whose last source was a finished Beolink session switches back to its last own source (or B&O Radio) first.
- **Scrolling text:** titles that don't fit scroll through once, then stop to save battery.
- **Settings:** network scan with three modes, and a way to clear the device list.

  | Scan mode | What it scans | Typical time |
  |---|---|---|
  | Fast | only the most likely subnet | ~3 s |
  | Normal (default) | detected subnets one at a time, stops at the first with speakers | ~5 s |
  | Thorough | every detected subnet, longer timeouts | ~10–15 s per subnet |

## How it works

The watch app (`src/c/main.c`) talks to PebbleKit JS on the phone (`src/pkjs/index.js`), which calls the speakers' local HTTP APIs:

- **Mozart:** `http://<ip>/api/v1/…` (playback state and commands, volume, Beolink join/leave, standby)
- **ASE:** `http://<ip>:8080/BeoZone/…`, `/BeoDevice/…` and the `/BeoNotify/Notifications` stream

**Discovery:** PebbleKit JS can't use mDNS or read the phone's IP address, so the app finds the network by probing common router addresses and then scans those /24 subnets for both APIs. To force a specific subnet, set it on the app's settings page in the Pebble phone app.

**Permission:** the app needs the `LocalNetwork` permission to reach the speakers. Allow it when the Pebble app asks.

## Build & install

Requires the [Pebble SDK](https://developer.repebble.com) (`pebble` tool).

```sh
pebble build
pebble install --emulator emery     # try it in the emulator
pebble install --cloudpebble        # install on your watch (after `pebble login`)
```

## Credits

- **[bang-olufsen/mozart-open-api](https://github.com/bang-olufsen/mozart-open-api):** Bang & Olufsen's open API specification for the Mozart platform.
- **[giachello/pybeoplay](https://github.com/giachello/pybeoplay):** Python library for the ASE API. The source of the endpoints this app uses for older speakers.
- **[PebbleOS](https://github.com/coredevices/PebbleOS):** the action bar icons (`resources/images/music_icon_*.png`) come from the Pebble firmware's music app, licensed under Apache-2.0. See [resources/images/README.md](resources/images/README.md).

## License

Copyright (C) 2026 Tobias Pryning.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. See [LICENSE](LICENSE).

The Bang & Olufsen name and logo are trademarks of Bang & Olufsen A/S. They're used here only to identify the products the app works with.
