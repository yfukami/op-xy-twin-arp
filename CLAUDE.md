# OP-XY Note Player Rules

- **SDK location:** the Open-XY SDK lives at `../open-xy`. Before writing code, read `sdk/include/opxy/player.hpp`, the "Note players" section of `docs/api.md`, and `../op-xy-euclidean-arp` (a finished, device-tested player).
- **Target:** embedded C++ for the Analog Devices Blackfin (OP-XY OS 1.2.1). No heap allocation (`new`, `malloc`, `std::vector`, `std::string`) in `player.cpp`. Keep all state in `State` (at most 2048 bytes, valid when zeroed). The host tests may use the standard library.
- **Verify after every code edit:**
  `../open-xy/.venv/bin/python ../open-xy/sdk.py test --machine machines/twin_arp --cxx /opt/homebrew/opt/llvm/bin/clang++`
  Add `--firmware` before committing, to compile for Blackfin as well.
- **No browser preview:** `sdk.py preview` does not support players. Review the screen with `python3 tools/gen_panel.py --state 1,7 --out <file>.svg`, then test on the device.
- **Git hygiene:** commit to this repository only when the user asks. Never modify files inside `../open-xy`.
- **Machine:** Twin Arp (`twin_arp`), two arpeggiators in parallel. The ID is final once it is in the catalog.

## Device-verified lessons (from Euclidean Arp, OS 1.2.1)

- **Player ID:** the player menu shows the machine ID, not its name, and cut it at 12 characters. Keep IDs at 12 or fewer, and settle the ID before publishing, because it is permanent in the catalog.
- **Screen artwork:** draw it with straight-line `<path>` elements (M/L/Z). The device drew none of a page's `<circle>` elements, even though the SDK's validator accepts them. Artwork must stay below y=38, where the SDK draws the header.
- **Header values:** each value has about 53 px at 20 px Univers Next Pro Light. Prefer short lowercase captions such as `up/dn`, because uppercase words like `CHORD` overlapped the next column. Captions come from `Display.values`, and enum labels in `definition.json` cannot contain `/`.
- **Page-1 encoder 4:** a non-enum control there becomes stock's on/off toggle, so use an enum. Enums move one choice per detent.
- **Divide guard:** `opxy::divide::quotient16` breaks when the compiler can prove both operands equal. Keep the `n == d ? 1 :` guard in `divide()`.
- **Player clock:** `update()` is drained before every player step (240 per quarter note) and after every press and release, at most eight calls each. Steps run with the transport stopped.
- **Releasing roots:** an empty Replace releases a root permanently. To silence a held key but keep it, Replace it with a far-future queued tone (delay 65535), renewed before it would sound.
- **Sequenced chords:** the notes of one sequenced step arrive one at a time, often highest first. Choose a chord's first note at the first step drain, not inside `press()`.
- **Saved projects:** these store each control's choice as a word. When a control's choice count changes, choose counts and an order so the old choices decode to the same options, and test it.
- **Catalog:** a player with two pages (`"press": "page"`) needs `"sdk": "1.16"`. When upstream `main` lags behind that, submit the catalog pull request to `staging`.

## Working with the user

The user tests every change on their own OP-XY and reports numbered issues.
- **Fixes:** batch them until the user asks to commit.
- **Device builds:** go to `~/Library/Application Support/OPXY-SDK/private/builds/<name>-NNN`, with `--layout` from the previous build. When the user asks, copy them with `sdk.py device push`. The user then exits file transfer, writes the OS with track 5 and restarts.
- **QA:** guide it in short numbered rounds with exact settings and pass criteria, and wait for results between rounds.
