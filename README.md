# Twin Arp

An Open-XY note player for the OP-XY (OS 1.2.1, SDK 1.16): two arpeggiators that play the same held notes at once, each with its own rate, style, octave range and transpose. It was inspired by the parallel mode of Reason's Dual Arpeggio.

## Controls

Page 1 is arp A, and page 2 is arp B. Press any encoder to switch pages.

| Encoder | Each arp |
|---|---|
| 1 | RATE: 1/2t to 1/32t, triplets included |
| 2 | STYLE: off to silence that arp, then 20 patterns from up to chd/1 (see Patterns) |
| 3 | OCTAVE: 1 to 4 |
| 4 | TRANS: -24 to +24 semitones |

- **Defaults:** A plays 1/16 up, and B plays 1/8 up, an octave lower, so a new track gives you a lead over a bass straight away.
- **Timing:** both arps start on the first key and run on their own clocks. Keys that arrive together, such as a sequenced chord, start at once. A key that arrives on its own waits about 15 ms for the rest of a hand-played chord, so both arps start on its lowest note.
- **Gate:** fixed at half a step.
- **Out of range:** a note transposed past MIDI's range is skipped, and that arp moves on.
- **One key held:** each sounding note lives with a held key. With two or more keys held, the two arps always use different keys. With only one key held, both arps share it, so a new note from one arp strikes the other arp's sounding note again. This is a limit of the OP-XY's player API.

## Patterns

The patterns are the same as in Chain Arp and Seed Arp, in the same order. They work on the held notes, sorted low to high and repeated across OCTAVE. N is the number of notes in that list. The examples use C E G B held, with OCTAVE 1 (N = 4). Each arp keeps its own place in its pattern.

| Label | Behaviour | Cycle length (steps) | Example |
|---|---|---|---|
| off | Silences this arp | — | — |
| up | 1..N | N | C E G B |
| down | N..1 | N | B G E C |
| up/dn | 1..N..2, ends not repeated | 2N−2 | C E G B G E |
| dn/up | N..1..N−1, ends not repeated | 2N−2 | B G E C E G |
| u/d+ | 1..N then N..1, ends repeated | 2N | C E G B B G E C |
| d/u+ | N..1 then 1..N, ends repeated | 2N | B G E C C E G B |
| order | Notes in the order they were pressed, repeated per octave | N | (press order) |
| conv | Outside in: 1, N, 2, N−1, ... | N | C B E G |
| divrg | Inside out: CONV reversed | N | G E B C |
| pinky | Top note alternating with each other note: 1,N,2,N,...,N−1,N | 2(N−1) | C B E B G B |
| thmb | Bottom note alternating with each other note: 1,2,1,3,...,1,N | 2(N−1) | C E C G C B |
| stu+ | Groups of 3, rising, each starting one note higher (the original STAIR) | 3(N−2) | C E G, E G B |
| stu- | Groups of 3, falling, each starting one note higher | 3(N−2) | G E C, B G E |
| std- | Groups of 3, falling, each starting one note lower | 3(N−2) | B G E, G E C |
| std+ | Groups of 3, rising, each starting one note lower | 3(N−2) | E G B, C E G |
| shuf | Random order, each note once per cycle, never starting on the note that ended the last | N | (random) |
| rand | A random note each step, repeats allowed | N | (random) |
| drunk | A random walk up or down one note, turning back at the ends and carrying over between cycles | N | (random) |
| chord | All held notes together each step, moving up an octave per hit across OCTAVE | N | CEGB ×4 |
| chd/1 | One chord hit, then up through 1..N | 1 + N | CEGB C E G B |

- **Chords on a monophonic instrument:** CHORD and CHD/1 start each chord's notes highest first and lowest last, so an instrument that can play only one note keeps the lowest note. The whole chord comes from one key, normally the lowest, so releasing that key stops the chord until the next step. TRANS applies to every note of a chord.
- **One note held:** every pattern plays that note, with a cycle length of 1.
- **Two notes held:** `stu+` and `stu-` play as UP, `std-` and `std+` as DOWN. PINKY, THMB, UP/DN and DN/UP last 2 steps.
- **Saved projects:** STYLE grew from 7 to 21 choices. A project saved before that may open with a different STYLE.

## Screen

- **Lanes:** two rows of 12 dots, one per note name from C to B. Arp A is on top and arp B below, and each row lights the note its arp is playing, or every note of its chord.
- **Centre text:** both notes, for example `C5  C4`, with `-` for an arp that is off. A chord shows its lowest note.

## Development

```sh
../open-xy/.venv/bin/python ../open-xy/sdk.py test --machine machines/twin_arp --cxx /opt/homebrew/opt/llvm/bin/clang++
../open-xy/.venv/bin/python ../open-xy/sdk.py test --machine machines/twin_arp --cxx /opt/homebrew/opt/llvm/bin/clang++ --firmware
```

`tests/harness.hpp` stands in for the OP-XY in host tests. To review the screen, run `python3 tools/gen_panel.py --state 1,7 --out /tmp/lanes.svg`. Device checks are in [QA.md](QA.md).

## License

MIT. See [LICENSE](LICENSE).
