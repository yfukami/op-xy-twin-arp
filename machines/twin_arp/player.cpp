// Twin Arp: two arpeggiators playing the same held notes, each with its own
// RATE, STYLE, OCTAVE and TRANSPOSE. Arp A is on page 1, arp B on page 2.
//
// A player has no note-on/note-off output and no clock callback, so (as in
// Euclidean Arp, device-tested):
// - each sounding note lives in the chord record of a held key, its carrier;
//   a record's notes all restart when it is replaced, so the two arps use
//   different carriers whenever two or more keys are held;
// - a held key that carries nothing keeps its record with a far-future queued
//   tone, because an empty Replace would release the root for good;
// - update() is drained before every player step and after every press and
//   release, so press and release mark their drain and the others are the clock.
#include <opxy/divide.hpp>
#include <opxy/player.hpp>

namespace twin_arp {
using opxy::i32;
using opxy::u8;
using opxy::u16;
using opxy::u32;
using opxy::player::Controls;
using opxy::player::Display;
using opxy::player::Plan;
using opxy::player::Tone;
using opxy::player::Update;

const u32 kSlots = opxy::player::max_tones;  // the SDK holds at most eight roots
const u32 kArps = 2;
const u32 kOctaves = 4;
const u32 kPool = kSlots * kOctaves;
const u32 kDrainLimit = 8;             // the SDK's updates per drain
const u16 kParkedDelay = 65535;        // player steps before a parked tone would sound
const u32 kRearmInterval = 32768;      // parked tones are renewed well before that
const u32 kDefaultChordWindow = 7;     // player steps, about 15 ms at 120 BPM

const u32 kRates = 9;
// The patterns, shared with Chain Arp, Seed Seq and Euclidean Arp, in this order;
// OFF silences an arp.
enum Pattern {
    Off, Up, Down, UpDown, DownUp, UpDownBoth, DownUpBoth, Order, Converge, Diverge,
    Pinky, Thumb, Stair, StairUpFalling, StairDownFalling, StairDownRising,
    Shuffle, Random, Drunk, Chord, ChordThenUp
};
const u32 kPatterns = 21;
const u32 kStyles = kPatterns;
const u32 kChordHit = 0xff;  // next_index's answer for a step that plays every held note

// The random styles' memory: SHUF's order, DRUNK's place and the generator.
struct Walker {
    u8 perm[kPool];
    u32 perm_count, last, drunk, rng;
    u8 drunk_placed;
};
const u32 kTransposes = 49;  // -24..+24 semitones

struct Slot {
    u8 root, velocity;
    u32 order;
};

struct Arp {
    u8 pool_pitch[kPool], pool_slot[kPool];
    u8 tone_pitch[kSlots], tone_velocity[kSlots];  // what it sounds (or sounded last), highest first
    Walker walker;
    u32 pool_count, pool_style, pool_octaves;
    u32 step, played_length;  // the pattern's next position, and its cycle's length when it last played
    u32 tick, gate_left;
    u8 pool_dirty, tone_count;
    u8 carrier;            // the slot carrying its notes, plus one; 0 when silent
    u8 has_note;
};

struct State {
    Slot slots[kSlots];
    Arp arps[kArps];
    u32 order_counter, rearm, rng, start_wait;
    u8 active, dirty, event_pending, draining, drain_calls, started, waiting;
};

struct Params {
    u32 rate[kArps], style[kArps], octaves[kArps];
    i32 transpose[kArps];
};

inline u32 smaller(u32 a, u32 b) { return a < b ? a : b; }
// quotient16's divide steps fail when both operands share a register, which the
// compiler chooses when it can prove them equal (100 / 100 gave the "e" in "e00%").
inline u32 divide(u32 n, u32 d) { return n == d ? 1 : opxy::divide::quotient16(n, d); }
inline u32 modulo(u32 n, u32 d) { return n - d * divide(n, d); }

inline Params read_params(const Controls& c) {
    using opxy::player::choice;
    static const u16 rates[kRates] = {320, 240, 160, 120, 80, 60, 40, 30, 20};
    Params p;
    for (u32 i = 0; i < kArps; ++i) {
        const i32* v = c.value + 4 * i;
        p.rate[i] = rates[choice(v[0], kRates)];
        p.style[i] = choice(v[1], kStyles);
        p.octaves[i] = 1 + choice(v[2], kOctaves);
        p.transpose[i] = static_cast<i32>(choice(v[3], kTransposes)) - 24;
    }
    return p;
}

inline u32 next_random(State& s) {
    u32 x = s.rng ? s.rng : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s.rng = x;
    return x;
}

inline u32 find_slot(const State& s, u32 note) {
    for (u32 i = 0; i < kSlots; ++i)
        if ((s.active >> i & 1) && s.slots[i].root == note) return i;
    return kSlots;
}

inline void add_to_pool(Arp& a, u32 pitch, u32 slot) {
    for (u32 i = 0; i < a.pool_count; ++i)
        if (a.pool_pitch[i] == pitch) return;
    a.pool_pitch[a.pool_count] = static_cast<u8>(pitch);
    a.pool_slot[a.pool_count] = static_cast<u8>(slot);
    ++a.pool_count;
}

// Runs only when the held keys, STYLE or OCTAVE changed; bounded by kPool.
inline void rebuild_pool(const State& s, Arp& a, u32 style, u32 octaves) {
    u8 ordered[kSlots];
    u32 count = 0;
    for (u32 i = 0; i < kSlots; ++i) {
        if (!(s.active >> i & 1)) continue;
        u32 at = count++;
        while (at && s.slots[ordered[at - 1]].order > s.slots[i].order) {
            ordered[at] = ordered[at - 1];
            --at;
        }
        ordered[at] = static_cast<u8>(i);
    }
    a.pool_count = 0;
    for (u32 octave = 0; octave < octaves; ++octave)
        for (u32 i = 0; i < count; ++i) {
            const u32 pitch = s.slots[ordered[i]].root + 12 * octave;
            if (pitch <= 127) add_to_pool(a, pitch, ordered[i]);
        }
    if (style != Order)
        for (u32 i = 1; i < a.pool_count; ++i) {
            const u8 pitch = a.pool_pitch[i], slot = a.pool_slot[i];
            u32 at = i;
            for (; at && a.pool_pitch[at - 1] > pitch; --at) {
                a.pool_pitch[at] = a.pool_pitch[at - 1];
                a.pool_slot[at] = a.pool_slot[at - 1];
            }
            a.pool_pitch[at] = pitch;
            a.pool_slot[at] = slot;
        }
    a.pool_style = style;
    a.pool_octaves = octaves;
    a.pool_dirty = 0;
}

inline void ensure_pool(const State& s, Arp& a, u32 style, u32 octaves) {
    if (a.pool_dirty || a.pool_style != style || a.pool_octaves != octaves) rebuild_pool(s, a, style, octaves);
}

// ---- Patterns, as in Chain Arp: pure functions of the position and N, the note count ----

inline u32 cycle_length(u32 pattern, u32 n) {
    if (!n) return 0;
    if (n == 1) return 1;
    switch (pattern) {
    case UpDown:
    case DownUp: return 2 * n - 2;
    case UpDownBoth:
    case DownUpBoth: return 2 * n;
    case Pinky:
    case Thumb: return 2 * (n - 1);
    case Stair:
    case StairUpFalling:
    case StairDownFalling:
    case StairDownRising: return n < 3 ? n : 3 * (n - 2);
    case ChordThenUp: return 1 + n;
    default: return n;
    }
}

inline u32 walker_random(u32& rng) {
    u32 x = rng ? rng : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng = x;
    return x;
}

inline u32 random_below(u32& rng, u32 n) { return ((walker_random(rng) >> 16) * n) >> 16; }

inline u32 converge(u32 step, u32 n) { return (step & 1) ? n - 1 - (step >> 1) : (step >> 1); }

// SHUF: every note once per cycle, never starting on the note that ended the last one.
inline void shuffle(Walker& w, u32 n) {
    for (u32 i = 0; i < n; ++i) w.perm[i] = static_cast<u8>(i);
    for (u32 i = n - 1; i > 0; --i) {
        const u32 j = random_below(w.rng, i + 1);
        const u8 swap = w.perm[i];
        w.perm[i] = w.perm[j];
        w.perm[j] = swap;
    }
    if (n > 1 && w.perm[0] == w.last) {
        const u8 swap = w.perm[0];
        w.perm[0] = w.perm[1];
        w.perm[1] = swap;
    }
    w.perm_count = n;
}

// The 0-based index into the note list at `step` of a cycle, or kChordHit.
inline u32 next_index(u32 pattern, u32 step, u32 n, Walker& w) {
    if (n <= 1) return 0;
    switch (pattern) {
    case Up:
    case Order: return step;
    case Down: return n - 1 - step;
    case UpDown: return step < n ? step : 2 * n - 2 - step;
    case DownUp: return n - 1 - (step < n ? step : 2 * n - 2 - step);
    case UpDownBoth: return step < n ? step : 2 * n - 1 - step;
    case DownUpBoth: return step < n ? n - 1 - step : step - n;
    case Converge: return converge(step, n);
    case Diverge: return converge(n - 1 - step, n);
    case Pinky: return (step & 1) ? n - 1 : step >> 1;
    case Thumb: return (step & 1) ? (step + 1) >> 1 : 0;
    // The stairs: groups of three, the staircase climbing up or down one note per
    // group, the notes in each group rising or falling. Under 3 notes: UP or DOWN.
    case Stair: return n < 3 ? step : divide(step, 3) + modulo(step, 3);
    case StairUpFalling: return n < 3 ? step : divide(step, 3) + 2 - modulo(step, 3);
    case StairDownFalling: return n < 3 ? n - 1 - step : n - 1 - divide(step, 3) - modulo(step, 3);
    case StairDownRising: return n < 3 ? n - 1 - step : n - 3 - divide(step, 3) + modulo(step, 3);
    case Shuffle: {
        if (!step || w.perm_count != n) shuffle(w, n);
        w.last = w.perm[step];
        return w.last;
    }
    case Random: return random_below(w.rng, n);
    case Drunk: {
        if (!w.drunk_placed || w.drunk >= n) {
            w.drunk = random_below(w.rng, n);
            w.drunk_placed = 1;
        } else if (walker_random(w.rng) & 0x10000u) {
            w.drunk = w.drunk + 1 < n ? w.drunk + 1 : w.drunk - 1;
        } else {
            w.drunk = w.drunk ? w.drunk - 1 : 1;
        }
        return w.drunk;
    }
    case Chord: return kChordHit;
    case ChordThenUp: return step ? step - 1 : kChordHit;
    default: return 0;
    }
}

// The note arp `which` sounds ends: its carrier's record is replaced without it.
inline void stop(State& s, u32 which) {
    Arp& a = s.arps[which];
    if (!a.carrier) return;
    s.dirty |= static_cast<u8>(1u << (a.carrier - 1));
    a.carrier = 0;
}

// The key that carries arp `which`'s note: the key that produced it, unless the
// other arp sounds on that key, then any other held key the other arp leaves free.
inline u32 choose_carrier(const State& s, u32 which, u32 owner) {
    const u32 other = s.arps[1 - which].carrier;
    if (other != owner + 1) return owner;
    for (u32 i = 0; i < kSlots; ++i)
        if ((s.active >> i & 1) && other != i + 1) return i;
    return owner;  // a single key carries both notes
}

// Sounds arp `which`'s tones, already chosen, from a key.
inline void start(State& s, u32 which, u32 owner, u32 rate) {
    Arp& a = s.arps[which];
    const u32 carrier = choose_carrier(s, which, owner);
    a.carrier = static_cast<u8>(carrier + 1);
    a.has_note = 1;
    a.gate_left = rate / 2 ? rate / 2 : 1;  // GATE is fixed at 50%
    s.dirty |= static_cast<u8>(1u << carrier);
}

inline bool transposed(u32 pitch, i32 transpose, u32& out) {
    const i32 shifted = static_cast<i32>(pitch) + transpose;
    if (shifted < 0 || shifted > 127) return false;
    out = static_cast<u32>(shifted);
    return true;
}

// Plays arp `which`'s next note, or chord. A note outside MIDI's range is
// skipped, but the arpeggio still moves on. If the held keys change, the
// position carries on into the new note list, as in Chain Arp. Returns whether
// anything sounded.
inline bool trigger(State& s, u32 which, const Params& p) {
    Arp& a = s.arps[which];
    stop(s, which);
    const u32 style = p.style[which];
    if (style == Off) return false;
    ensure_pool(s, a, style, p.octaves[which]);
    const u32 n = a.pool_count;
    if (!n) return false;
    const u32 length = cycle_length(style, n);
    if (a.step >= length) a.step = a.step == a.played_length ? 0 : modulo(a.step, length);
    const u32 at = a.step++;
    a.played_length = length;
    const u32 index = next_index(style, at, n, a.walker);
    u32 owner = kSlots, note;
    a.tone_count = 0;
    if (index == kChordHit) {
        // Every held key, CHORD climbing OCTAVE one octave per hit, from one record,
        // highest note first and lowest last, so a monophonic instrument keeps the
        // lowest note.
        const u32 shift = 12 * (style == Chord ? modulo(at, p.octaves[which]) : 0);
        for (u32 i = 0; i < kSlots; ++i) {
            if (!(s.active >> i & 1) || !transposed(s.slots[i].root + shift, p.transpose[which], note)) continue;
            u32 j = a.tone_count++;
            for (; j && a.tone_pitch[j - 1] < note; --j) {
                a.tone_pitch[j] = a.tone_pitch[j - 1];
                a.tone_velocity[j] = a.tone_velocity[j - 1];
            }
            a.tone_pitch[j] = static_cast<u8>(note);
            a.tone_velocity[j] = s.slots[i].velocity;
            if (owner == kSlots || s.slots[i].root < s.slots[owner].root) owner = i;
        }
        if (owner == kSlots) return false;
    } else {
        owner = a.pool_slot[index];
        if (!transposed(a.pool_pitch[index], p.transpose[which], note)) return false;
        a.tone_pitch[0] = static_cast<u8>(note);
        a.tone_velocity[0] = s.slots[owner].velocity;
        a.tone_count = 1;
    }
    start(s, which, owner, p.rate[which]);
    return true;
}

inline u32 count_bits(u32 mask) {
    u32 count = 0;
    for (; mask; mask &= mask - 1) ++count;
    return count;
}

// How long a key that arrives alone waits for the rest of a hand-played chord:
// about 15 ms (a player step lasts 2,500,000 / bpm_x10 microseconds), always
// under half the faster arp's step, so it cannot reach either arp's next step.
inline u32 chord_window(u32 bpm_x10, u32 rate) {
    u32 window = bpm_x10 ? divide(bpm_x10 * 3, 500) : kDefaultChordWindow;
    if (window < 1) window = 1;
    return smaller(window, rate / 2);
}

inline void advance(State& s, u32 which, const Params& p) {
    Arp& a = s.arps[which];
    if (a.gate_left && --a.gate_left == 0) stop(s, which);
    if (++a.tick < p.rate[which]) return;
    a.tick = 0;
    trigger(s, which, p);
}

inline u8 carrying(const State& s) {
    u8 mask = 0;
    for (u32 i = 0; i < kArps; ++i)
        if (s.arps[i].carrier) mask = static_cast<u8>(mask | (1u << (s.arps[i].carrier - 1)));
    return mask;
}

inline void clock_step(State& s, const Controls& c) {
    if (!s.active) return;
    const Params p = read_params(c);
    if (!s.started) {
        s.started = 1;
        // Keys that arrived together (a sequenced chord) play at once. A key that
        // arrived alone waits about 15 ms for the rest of a hand-played chord, so
        // the first notes are the right ones and nothing is struck twice (choosing
        // them again when a lower key landed sounded on the device as a two-note
        // stab). The steps still count from the first key.
        if (count_bits(s.active) >= 2) {
            for (u32 i = 0; i < kArps; ++i) trigger(s, i, p);
        } else {
            s.waiting = 1;
            s.start_wait = chord_window(c.bpm_x10, smaller(p.rate[0], p.rate[1]));
        }
        return;
    }
    if (s.waiting) {
        for (u32 i = 0; i < kArps; ++i) ++s.arps[i].tick;
        if (--s.start_wait) return;
        s.waiting = 0;
        for (u32 i = 0; i < kArps; ++i) trigger(s, i, p);
        return;
    }
    for (u32 i = 0; i < kArps; ++i) advance(s, i, p);
    if (++s.rearm >= kRearmInterval) {
        s.rearm = 0;
        s.dirty |= static_cast<u8>(s.active & ~carrying(s));
    }
}

inline u32 lowest_bit(u32 mask) {
    u32 i = 0;
    while (!(mask >> i & 1)) ++i;
    return i;
}

inline void park(Tone& tone, u32 root) {
    tone.pitch = static_cast<u8>(root);
    tone.velocity = 1;
    tone.delay = kParkedDelay;
}

bool press(State& s, const Controls&, u32 note, u32 velocity, Plan& plan) {
    plan.count = 0;
    if (note > 127) return false;
    s.event_pending = 1;
    const bool first = !s.active;
    u32 slot = find_slot(s, note);
    if (slot == kSlots) {
        if (s.active == 0xff) return true;  // the SDK cannot hold a ninth root
        slot = lowest_bit(static_cast<u8>(~s.active));
    }
    const u8 bit = static_cast<u8>(1u << slot);
    Slot& key = s.slots[slot];
    key.root = static_cast<u8>(note);
    key.velocity = static_cast<u8>(velocity < 1 ? 1 : velocity > 127 ? 127 : velocity);
    key.order = ++s.order_counter;
    s.active |= bit;
    s.dirty &= static_cast<u8>(~bit);
    for (u32 i = 0; i < kArps; ++i) {
        Arp& a = s.arps[i];
        a.pool_dirty = 1;
        if (a.carrier == slot + 1) a.carrier = 0;  // the plan replaces anything this root held
    }
    plan.count = 1;
    park(plan.tones[0], note);
    if (!first) return true;
    for (u32 i = 0; i < kArps; ++i) {
        Arp& a = s.arps[i];
        a.step = a.played_length = a.tick = a.gate_left = 0;
        a.carrier = a.has_note = a.tone_count = 0;
    }
    s.rearm = s.start_wait = 0;
    s.started = s.waiting = s.dirty = 0;
    if (!s.rng) s.rng = 0x9e3779b9u ^ (note << 8) ^ key.velocity;
    for (u32 i = 0; i < kArps; ++i) {  // the random styles start somewhere new each phrase
        Walker& w = s.arps[i].walker;
        w.rng = next_random(s);
        w.perm_count = 0;
        w.last = 0xff;
        w.drunk_placed = 0;
    }
    return true;
}

bool release(State& s, u32 note) {
    s.event_pending = 1;
    const u32 slot = find_slot(s, note);
    if (slot == kSlots) return true;  // a key press() could not hold
    const u8 keep = static_cast<u8>(~(1u << slot));
    s.active &= keep;
    s.dirty &= keep;
    for (u32 i = 0; i < kArps; ++i) {
        Arp& a = s.arps[i];
        a.pool_dirty = 1;
        if (a.carrier == slot + 1) a.carrier = 0;  // stock silences what it carried
        if (!s.active) a.gate_left = 0;
    }
    return false;  // stock releases this root's notes
}

bool update(State& s, const Controls& c, Update& change) {
    if (!s.draining) {
        s.draining = 1;
        s.drain_calls = 0;
        if (s.event_pending) s.event_pending = 0;
        else clock_step(s, c);
    }
    s.dirty &= s.active;
    if (!s.dirty) {
        s.draining = 0;
        return false;
    }
    // Keys going silent first, so an older note-off cannot follow a new note.
    const u8 carried = carrying(s);
    const u8 silent = static_cast<u8>(s.dirty & ~carried);
    const u32 slot = lowest_bit(silent ? silent : s.dirty);
    s.dirty &= static_cast<u8>(~(1u << slot));
    change.mode = opxy::player::Replace;
    change.root = s.slots[slot].root;
    change.plan.count = 0;
    for (u32 i = 0; i < kArps; ++i) {
        const Arp& a = s.arps[i];
        if (a.carrier != slot + 1) continue;
        for (u32 j = 0; j < a.tone_count && change.plan.count < opxy::player::max_tones; ++j) {
            Tone& tone = change.plan.tones[change.plan.count++];
            tone.pitch = a.tone_pitch[j];
            tone.velocity = a.tone_velocity[j];
            tone.delay = 0;
        }
    }
    if (!change.plan.count) {
        change.plan.count = 1;
        park(change.plan.tones[0], change.root);
    }
    if (++s.drain_calls >= kDrainLimit) s.draining = 0;  // the SDK stops calling here
    return true;
}

// Stock reset and all-notes-off: stock has already silenced every record.
void reset(State& s) { __builtin_memset(&s, 0, sizeof s); }

inline void append(char* out, u32 size, u32& at, const char* text) {
    while (*text && at + 1 < size) out[at++] = *text++;
    out[at] = 0;
}

inline void append_number(char* out, u32 size, u32& at, u32 value) {
    char digits[4] = {0, 0, 0, 0};
    u32 count = 0;
    if (value >= 100) digits[count++] = static_cast<char>('0' + divide(value, 100));
    if (value >= 10) digits[count++] = static_cast<char>('0' + modulo(divide(value, 10), 10));
    digits[count] = static_cast<char>('0' + modulo(value, 10));
    append(out, size, at, digits);
}

inline void append_note(char* out, u32 size, u32& at, u32 pitch) {
    static const char names[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const u32 octave = divide(pitch, 12);
    append(out, size, at, names[pitch - 12 * octave]);
    if (octave) append_number(out, size, at, octave - 1);
    else append(out, size, at, "-1");
}

void display(const State& s, const Controls& c, Display& d) {
    static const char rates[kRates][6] = {"1/2t", "1/4", "1/4t", "1/8", "1/8t", "1/16", "1/16t", "1/32", "1/32t"};
    // Lower case: a header value has under 60 px, and "DOWN" would reach the next title.
    static const char styles[kPatterns][6] = {"off", "up", "down", "up/dn", "dn/up", "updn+", "dnup+", "order", "conv",
                                              "divrg", "pinky", "thumb", "stu+", "stu-", "std-", "std+", "shuf", "rand",
                                              "drunk", "chord", "chd/1"};
    const Params p = read_params(c);
    for (u32 i = 0; i < kArps; ++i) {
        char (*v)[8] = d.values + 4 * i;
        u32 at = 0;
        append(v[0], sizeof v[0], at, rates[opxy::player::choice(c.value[4 * i], kRates)]);
        at = 0;
        append(v[1], sizeof v[1], at, styles[p.style[i]]);
        at = 0;
        append_number(v[2], sizeof v[2], at, p.octaves[i]);
        at = 0;
        if (p.transpose[i]) append(v[3], sizeof v[3], at, p.transpose[i] > 0 ? "+" : "-");
        append_number(v[3], sizeof v[3], at, static_cast<u32>(p.transpose[i] < 0 ? -p.transpose[i] : p.transpose[i]));
    }

    // Dots 0..11: arp A's lane, one per pitch class; dots 12..23: arp B's.
    u32 at = 0;
    if (!s.active) {
        append(d.text, sizeof d.text, at, "ready");
        return;
    }
    for (u32 i = 0; i < kArps; ++i) {
        const Arp& a = s.arps[i];
        if (i) append(d.text, sizeof d.text, at, "  ");
        if (p.style[i] == Off || !a.has_note || !a.tone_count) {
            append(d.text, sizeof d.text, at, "-");
            continue;
        }
        for (u32 j = 0; j < a.tone_count; ++j) d.lit |= 1u << (12 * i + modulo(a.tone_pitch[j], 12));
        append_note(d.text, sizeof d.text, at, a.tone_pitch[a.tone_count - 1]);  // a chord shows its lowest note
    }
}
}  // namespace twin_arp
