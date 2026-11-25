# Device checklist

Run every check on one OP-XY (OS 1.2.1), top to bottom, in one project, on the exact commit you will publish. Host tests cannot answer these questions. Add the player's own checks under "Musical features".

**Setup:** one track with a simple sustaining synth patch, with Twin Arp as its player, at its defaults: A on 1/16, up, 1 octave, 0; B on 1/8, up, 1 octave, -12. Hold or step-sequence C, E, G unless a check says otherwise.

## Core timing

1. **Transport stopped.** Hold a key with the transport stopped. Pass: the player works as it does while playing.
2. **Transport playing.** Press play and hold a key against a drum at 80, 120 and 160 BPM. Pass: the timing stays locked.
3. **Long hold.** Hold three keys for five minutes. Pass: no stray or stuck notes, and no clicks.

## Notes stop cleanly

4. **Releasing keys one at a time.** Pass: each released key's notes stop, and nothing hangs.
5. **Stopping a sequence.** Sequence notes, press play, then stop mid-note. Pass: silence, nothing hangs.
6. **Switching track and project while holding keys.** Pass: nothing hangs, and the next key starts cleanly.
7. **Nine keys.** Pass: eight play, and the ninth has no effect, including on release.

## Controls and screen

8. **Full range of every control.** Pass: one value per detent, and no caption touches the next column.
9. **Page switching,** if the player has two pages. Pass: the header switches, and all values read correctly.
10. **Screen artwork.** Pass: every shape shows, below the header and legible.
11. **Saving.** Set every control to an unusual value, save, switch project and back. Pass: all values return.
12. **Name.** Pass: the player menu shows the ID in full.

## Musical features

13. **Two arps.** Pass: A plays C E G on sixteenths, B plays C E G an octave down on eighths, both from the first step.
14. **Independent rates.** A `1/8t` against B `1/8`, with a drum on quarter notes. Pass: three A notes and two B notes per beat, locked for several bars.
15. **Styles and off.** Try every STYLE on each arp. Pass: each plays as the README's Patterns table says, `chord` and `chd/1` sound the whole chord together and light every note of it in that arp's lane, and `off` silences only that arp.
16. **Transpose.** A `+12`, B `-24`. Pass: both shift. Push A to `+24` with high keys: notes past the top are skipped without hanging.
17. **No re-strikes.** Hold two or more keys with a long, sustaining patch. Pass: one arp's notes never cut or re-strike the other's.
18. **One key.** Hold a single key. Pass: both arps play, though each new note may re-strike the other arp's note, and nothing hangs.
19. **Screen.** Pass: the two lanes light the note each arp plays, and the centre shows both notes, with `-` for an arp that is off.
20. **Hand-played chords.** Play C, E, G by hand several times, at different speeds. Pass: both arps usually start on C. A very quick double attack is acceptable when C lands last.

## Results

2026-11-23 to 2026-11-24, OS 1.2.1, all 20 checks passed on build `all-006`. The build was made from commit `9fb4456` plus the STYLE label change to `u/d+`, `d/u+` and `thmb`.
