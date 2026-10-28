<p align="center"><picture>
<source media="(prefers-color-scheme: dark)" srcset="frontend/branding/logo-dark.png">
<img src="frontend/branding/logo-light.png" alt="iONemu" width="640">
</picture></p>

# iONemu

**The first emulator for the Hasbro iON**, released on the console's 20th anniversary, 28 October 2026.

The Hasbro iON (2006) was a console with a camera instead of a controller: you stood in front of the TV and waved at
Blue, Dora, Batman and SpongeBob. iONemu plays its games on Windows, macOS and Linux. Your webcam is the controller,
just as it was on the console; without one, the mouse is your hand.

It runs all the currently available games for the system: Blue's Room, Dora the Explorer, Batman, SpongeBob
SquarePants, Danny Phantom and the Best of Nickelodeon sampler. LazyTown has not been dumped yet, so it has not been
tested.

More in the [wiki](../../wiki): the story of the console, where its games are preserved, dumping your discs, camera
tips, controls, troubleshooting.

## Download

Get the latest version from the [Releases page](../../releases/latest).

- **Windows** (10 or 11): unpack the zip anywhere and run `ionemu.exe`.
- **macOS** (13 or newer): open the dmg and drag iONemu to Applications. The app is not signed with an Apple certificate, so macOS blocks the first start: open it once, then click *Open Anyway* in System Settings > Privacy & Security.
- **Linux**: make the AppImage executable and run it (Ubuntu 22.04, Debian 12, Fedora 36 or newer).

iONemu checks for new versions once a day and installs them with one click. The General settings page turns this off.

## Playing

You need images of your own iON discs: a `.cue` with its `.bin` tracks, or an `.mdx`. No game data is included. The wiki's
[Finding The Games](../../wiki/Finding-The-Games) page lists where the known disc images are preserved.

1. Click **Add Disc Folder** and pick the folder with your disc images.
2. **Camera Setup**: choose your webcam and step back until your head fills the outline. Or skip it and play with the mouse.
3. **Double-click a cover.** The games react to movement, so wave at things. With the mouse, move it or hold the left
   button. Esc opens the menu.

A disc that was dumped with read errors shows a "!" badge; iONemu can repair it from your other discs.

To a computer an iON disc is an audio CD. Any tool that writes a raw `.bin` of every track with a `.cue` sheet, the
way Redump does, makes an image iONemu can read.

## Building from source

CMake 3.21 or newer, a C++20 compiler, Qt 6.5 or newer and Ninja:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

`ioncore/README.md` and `frontend/README.md` describe the code.

## Thanks

Planet Clue's video [This forgotten 2006 game console only has SIX games](https://www.youtube.com/watch?v=w_ccMeb33ls) is a good
introduction to the console, and the reason iONemu exists. Thanks also to everyone who dumped and preserved the
iON's discs, and to the MAME team, whose u'nSP and SPG2xx work iONemu builds on.

## Licence

Free software under the GNU GPL, version 3 or later. iONemu includes code derived from MAME (GPL-2.0-or-later and
BSD-3-Clause), uses Qt (LGPL-3.0),
FFmpeg (LGPL-2.1-or-later, in the release packages) and Lucide icons (ISC); see `frontend/licenses/THIRD_PARTY.txt`.

Hasbro, iON, Nickelodeon and the game titles are trademarks of their owners. iONemu is not affiliated with them and
contains no game data, firmware or artwork.
