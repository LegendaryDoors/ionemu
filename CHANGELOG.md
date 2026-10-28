# Changelog

## 0.1.0 (2026-10-28)

The first release, on the console's 20th anniversary.

**Emulation**
- The SPG220 system-on-chip at register level: the u'nSP CPU, the SPG2xx video unit with the iON's 480i output mode, the 24-channel sound unit, the camera motion block, the save chip. The firmware is reimplemented from the behaviour of the discs; no firmware dump is needed or used.
- All six dumped discs boot and play to completion: Blue's Room, Dora the Explorer, Batman, SpongeBob SquarePants, Danny Phantom and the Best of Nickelodeon sampler.
- Sound is complete, including the ADPCM voice codec of the Nick Jr. titles, matched against real-console recordings.
- Disc images are read directly: `.cue` with `.bin` tracks, `.mdx`, or a loose data track. Discs are identified by SHA-1; unknown dumps still play.
- A damaged dump (read errors in the data sectors) is detected exactly and can be repaired from its own duplicate blocks and from the other discs in the library.

**Launcher (iONemu)**
- A cover grid or list of the library, with a title card rendered from each disc's own intro; your own cover files and downloaded covers replace the cards.
- Webcam input through Qt Multimedia: the games see you and react to movement, as on the console. A camera setup page shows what the console would see, with the detector's own squares and a stand-in outline.
- Without a camera the mouse is the hand, and a customisable stand-in character (Simple or Detailed style, colours, hair) is what the games see; its preview is live, with the console's own motion squares, so the mouse control can be tried before a game.
- Help > iONemu Wiki and Where to Find the Games; an empty library links to the wiki page on where the disc images are preserved.
- Save states (five slots), in-game saves, screenshots, fast-forward, fullscreen, a composite video filter, rebindable keys, a motion overlay.
- An update check against the GitHub releases (daily at start, off in General; Help > Check for Updates), with a PCSX2-style dialog: versions and dates, download size, the notes, and one-click Download and Install on all three platforms (progress bar, then a restart), Skip This Version, Remind Me Later.
- Windows (zip), macOS (universal dmg, macOS 13+) and Linux (AppImage, glibc 2.35+); builds from source anywhere Qt 6.5 runs.
