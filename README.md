# Twin Arp

An Open-XY note player for the OP-XY (OS 1.2.1, SDK 1.16): two arpeggiators that play the same held notes at once, each with its own rate, style, octave range and transpose. It was inspired by the parallel mode of Reason's Dual Arpeggio.

## Controls

Page 1 is arp A, and page 2 is arp B. Press any encoder to switch pages.

| Encoder | Each arp |
|---|---|
| 1 | RATE: 1/2t to 1/32t, triplets included |
| 2 | STYLE: up, down, up/dn, dn/up, order, rand, or off to silence that arp |
| 3 | OCTAVE: 1 to 4 |
| 4 | TRANS: -24 to +24 semitones |

- **Defaults:** A plays 1/16 up, and B plays 1/8 up, an octave lower, so a new track gives you a lead over a bass straight away.
- **Timing:** both arps start on the first key and run on their own clocks.
- **Gate:** fixed at half a step.
- **Out of range:** a note transposed past MIDI's range is skipped, and that arp moves on.
- **One key held:** each sounding note lives with a held key. With two or more keys held, the two arps always use different keys. With only one key held, both arps share it, so a new note from one arp strikes the other arp's sounding note again. This is a limit of the OP-XY's player API.

## Screen

- **Lanes:** two rows of 12 dots, one per note name from C to B. Arp A is on top and arp B below, and each row lights the note its arp is playing.
- **Centre text:** both notes, for example `C5  C4`, with `-` for an arp that is off.

## Development

```sh
../open-xy/.venv/bin/python ../open-xy/sdk.py test --machine machines/twin_arp --cxx /opt/homebrew/opt/llvm/bin/clang++
../open-xy/.venv/bin/python ../open-xy/sdk.py test --machine machines/twin_arp --cxx /opt/homebrew/opt/llvm/bin/clang++ --firmware
```

`tests/harness.hpp` stands in for the OP-XY in host tests. To review the screen, run `python3 tools/gen_panel.py --state 1,7 --out /tmp/lanes.svg`. Device checks are in [QA.md](QA.md).

## License

MIT. See [LICENSE](LICENSE).
