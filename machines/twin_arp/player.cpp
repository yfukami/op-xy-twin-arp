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
const u32 kDefaultChordWindow = 9;     // player steps, about 19 ms at 120 BPM

const u32 kRates = 9;
enum Style { Up, Down, UpDown, DownUp, Order, Random, Off };
const u32 kStyles = 7;
const u32 kTransposes = 49;  // -24..+24 semitones

struct Slot {
    u8 root, velocity;
    u32 order;
};

struct Arp {
    u8 pool_pitch[kPool], pool_slot[kPool];
    u32 pool_count, pool_style, pool_octaves;
    u32 cursor, tick, gate_left;
    u8 pool_dirty, returning, has_last, last_pitch;
    u8 carrier;            // the slot carrying its sounding note, plus one; 0 when silent
    u8 pitch, velocity;    // the note it sounds (or sounded last)
    u8 has_note, start_played;
};

struct State {
    Slot slots[kSlots];
    Arp arps[kArps];
    u32 order_counter, rearm, rng, start_wait;
    u8 active, dirty, event_pending, draining, drain_calls, started, regroup;
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

// After the pool changes, continue from the note last played, so that a key
// joining the arpeggio neither repeats that note nor skips one.
inline void anchor_cursor(Arp& a, u32 style) {
    const u32 n = a.pool_count;
    if (!a.has_last || !n || style == Random) return;
    const u32 last = a.last_pitch;
    if (style == Order) {
        for (u32 i = 0; i < n; ++i)
            if (a.pool_pitch[i] == last) a.cursor = i + 1;
        return;
    }
    u32 below = 0;
    while (below < n && a.pool_pitch[below] < last) ++below;
    const u32 above = below < n && a.pool_pitch[below] == last ? below + 1 : below;
    if (style == Up) {
        a.cursor = above;
    } else if (style == Down) {
        a.cursor = below ? n - below : 0;
    } else {
        // DN/UP is UP/DN over the pool read from the top.
        const u32 before = style == UpDown ? below : n - above;
        const u32 after = style == UpDown ? above : n - below;
        if (a.returning && n > 1) a.cursor = before ? 2 * n - 2 - (before - 1) : 1;
        else a.cursor = after;
    }
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
    anchor_cursor(a, style);
}

inline void ensure_pool(const State& s, Arp& a, u32 style, u32 octaves) {
    if (a.pool_dirty || a.pool_style != style || a.pool_octaves != octaves) rebuild_pool(s, a, style, octaves);
}

inline u32 next_pool_index(State& s, Arp& a, u32 style) {
    const u32 n = a.pool_count;
    if (style == Random) return ((next_random(s) >> 16) * n) >> 16;
    const u32 period = (style == UpDown || style == DownUp) && n > 1 ? 2 * n - 2 : n;
    if (a.cursor >= period) a.cursor = 0;
    const u32 at = a.cursor++;
    if (style == Down) return n - 1 - at;
    a.returning = at >= n;
    const u32 bounced = at < n ? at : period - at;
    return style == DownUp ? n - 1 - bounced : bounced;
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

inline void start(State& s, u32 which, u32 owner, u32 pitch, u32 rate) {
    stop(s, which);
    Arp& a = s.arps[which];
    const u32 carrier = choose_carrier(s, which, owner);
    a.carrier = static_cast<u8>(carrier + 1);
    a.pitch = static_cast<u8>(pitch);
    a.velocity = s.slots[owner].velocity;
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

// Plays arp `which`'s next note; a note outside MIDI's range is skipped, but
// the arpeggio still moves on. Returns whether it sounded.
inline bool trigger(State& s, u32 which, const Params& p) {
    Arp& a = s.arps[which];
    stop(s, which);
    if (p.style[which] == Off) return false;
    ensure_pool(s, a, p.style[which], p.octaves[which]);
    if (!a.pool_count) return false;
    const u32 at = next_pool_index(s, a, p.style[which]);
    const u32 slot = a.pool_slot[at], pitch = a.pool_pitch[at];
    a.last_pitch = static_cast<u8>(pitch);
    a.has_last = 1;
    u32 note;
    if (!transposed(pitch, p.transpose[which], note)) return false;
    start(s, which, slot, note, p.rate[which]);
    return true;
}

// A key landed just after the first step: choose that step's note again with it included.
inline void regroup(State& s, u32 which, const Params& p) {
    Arp& a = s.arps[which];
    if (p.style[which] == Random || p.style[which] == Off) return;
    a.has_last = 0;
    a.pool_dirty = 1;
    a.cursor = a.returning = 0;
    ensure_pool(s, a, p.style[which], p.octaves[which]);
    if (!a.pool_count) return;
    const u32 at = next_pool_index(s, a, p.style[which]);
    const u32 slot = a.pool_slot[at], pitch = a.pool_pitch[at];
    a.last_pitch = static_cast<u8>(pitch);
    a.has_last = 1;
    u32 note;
    if (!a.start_played || !transposed(pitch, p.transpose[which], note)) return;
    if (a.carrier && a.pitch == note) return;  // already the right note
    start(s, which, slot, note, p.rate[which]);
}

// How long after the first note a late key can still change it: about 20 ms,
// because hand-played chords never land together. Always under half the
// faster arp's step, so it cannot reach either arp's next step.
inline u32 chord_window(u32 bpm_x10, u32 rate) {
    u32 window = bpm_x10 ? divide(bpm_x10, 125) : kDefaultChordWindow;
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
    if (!s.started) {  // every key that arrived with the first is already held
        s.started = 1;
        s.start_wait = chord_window(c.bpm_x10, smaller(p.rate[0], p.rate[1]));
        for (u32 i = 0; i < kArps; ++i) s.arps[i].start_played = trigger(s, i, p);
        return;
    }
    for (u32 i = 0; i < kArps; ++i) advance(s, i, p);
    if (++s.rearm >= kRearmInterval) {
        s.rearm = 0;
        s.dirty |= static_cast<u8>(s.active & ~carrying(s));
    }
    if (!s.start_wait) return;
    --s.start_wait;
    if (s.regroup) {
        s.regroup = 0;
        for (u32 i = 0; i < kArps; ++i) regroup(s, i, p);
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
    if (!first) {
        if (s.start_wait) s.regroup = 1;
        return true;
    }
    for (u32 i = 0; i < kArps; ++i) {
        Arp& a = s.arps[i];
        a.cursor = a.tick = a.gate_left = 0;
        a.carrier = a.has_note = a.has_last = a.returning = a.start_played = 0;
    }
    s.rearm = s.start_wait = 0;
    s.started = s.regroup = s.dirty = 0;
    if (!s.rng) s.rng = 0x9e3779b9u ^ (note << 8) ^ key.velocity;
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
        Tone& tone = change.plan.tones[change.plan.count++];
        tone.pitch = a.pitch;
        tone.velocity = a.velocity;
        tone.delay = 0;
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
    static const char styles[kStyles][6] = {"up", "down", "up/dn", "dn/up", "order", "rand", "off"};
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
        if (p.style[i] == Off || !a.has_note) {
            append(d.text, sizeof d.text, at, "-");
            continue;
        }
        d.lit |= 1u << (12 * i + modulo(a.pitch, 12));
        append_note(d.text, sizeof d.text, at, a.pitch);
    }
}
}  // namespace twin_arp
