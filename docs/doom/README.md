# Doom on VeRsitileOS

The classic Doom engine is built as `/bin/doom` with VeRsitileOS video,
keyboard, mouse, timing, filesystem, and process adapters. From the terminal,
run it with a user-supplied IWAD:

```text
doom -iwad /path/to/doom2.wad
```

The ISO includes the FreeDoom IWADs at `/usr/share/doom/freedoom1.wad` and
`/usr/share/doom/freedoom2.wad`; run `doom` without arguments to start with
the default FreeDoom content. Doom supports keyboard input and uses mouse input
when a mouse device is available. You can also select an IWAD explicitly:

```text
doom -iwad /usr/share/doom/freedoom2.wad
```

Launch Doom from the terminal with `/bin/doom`.

The engine looks for standard IWAD filenames in `/usr/share/doom` when
`-iwad` is omitted. Other IWADs can be supplied with `-iwad` if legally
obtained. The included FreeDoom game data is distributed under the BSD
license reproduced in `/usr/share/doom/COPYING.FREEDOOM`.

This initial port uses Doom's software renderer and VeRsitileOS framebuffer
blits. Audio output and multiplayer networking are not implemented; game
sound is disabled.

The source and its included license are in
[`port/DOOM-master`](../../port/DOOM-master/). Preserve the upstream license
and copyright notices when modifying or redistributing the engine.
