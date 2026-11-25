#include <cstring>
#include "harness.hpp"

enum { R2T, R4, R4T, R8, R8T, R16, R16T, R32, R32T };
typedef std::vector<u32> Notes;

// Sets one arp's four controls: arp 0 is A (page 1), arp 1 is B (page 2).
static void configure(Host& h, u32 arp, u32 rate, u32 style, u32 octaves, int transpose) {
    h.set(4 * arp + 0, rate, machine::kRates);
    h.set(4 * arp + 1, style, machine::kStyles);
    h.set(4 * arp + 2, octaves - 1, 4);
    h.set(4 * arp + 3, static_cast<u32>(transpose + 24), machine::kTransposes);
}

// The defaults: A at 1/16 up, B at 1/8 up an octave down.
static void defaults(Host& h) {
    configure(h, 0, R16, machine::Up, 1, 0);
    configure(h, 1, R8, machine::Up, 1, -12);
}

// Note-ons in one pitch range, with their times.
static std::vector<Event> ons(const Host& h, u32 low, u32 high) {
    std::vector<Event> out;
    for (const Event& e : h.events)
        if (e.on && e.pitch >= low && e.pitch <= high) out.push_back(e);
    return out;
}

static Notes pitches(const std::vector<Event>& events) {
    Notes out;
    for (const Event& e : events) out.push_back(e.pitch);
    return out;
}

static void both_arps_play() {
    // A sequenced chord in any order: A plays it at 1/16, B an octave down at 1/8,
    // both from the first step.
    const Notes orders[3] = {{60, 64, 67}, {67, 64, 60}, {64, 67, 60}};
    for (const Notes& keys : orders) {
        Host h;
        defaults(h);
        for (u32 key : keys) h.press(key);
        h.step(120 * 6);
        const std::vector<Event> a = ons(h, 60, 127), b = ons(h, 0, 59);
        require(pitches(a) == Notes({60, 64, 67, 60, 64, 67, 60, 64, 67, 60, 64, 67}));
        require(pitches(b) == Notes({48, 52, 55, 48, 52, 55}));
        for (u32 i = 0; i < a.size(); ++i) require(a[i].time == 60 * i);
        for (u32 i = 0; i < b.size(); ++i) require(b[i].time == 120 * i);
        require(h.stale_updates == 0 && h.largest_drain <= 8);
    }
}

static void gates_and_idle_keys() {
    // GATE is fixed at half a step: A's notes last 30 player steps, B's 60.
    Host h;
    defaults(h);
    h.press(60); h.press(64);
    h.step(240 * 2);
    std::vector<int> started(128, -1);
    u32 checked = 0;
    for (const Event& e : h.events) {
        if (e.on) { started[e.pitch] = static_cast<int>(e.time); continue; }
        require(started[e.pitch] >= 0);
        require(e.time - started[e.pitch] == (e.pitch >= 60 ? 30u : 60u));
        ++checked;
    }
    require(checked > 8);

    // Keys that carry nothing for a long time never sound their parked placeholder.
    Host idle;
    // Key 120 is transposed past MIDI's range, so it never carries a note.
    configure(idle, 0, R2T, machine::Up, 1, 12);
    configure(idle, 1, R8, machine::Off, 1, 0);
    idle.press(60); idle.press(120);
    idle.step(140000);
    require(!ons(idle, 0, 127).empty());
    for (const Event& e : ons(idle, 0, 127)) require(e.velocity != 1);  // parked tones have velocity 1
    require(idle.stale_updates == 0);
}

static void independent_rates() {
    // A triplet against a straight rate, for many bars, without drift.
    Host h;
    configure(h, 0, R8T, machine::Up, 1, 0);
    configure(h, 1, R8, machine::Up, 1, -12);
    h.press(60); h.press(64);
    h.step(240 * 32);
    const std::vector<Event> a = ons(h, 60, 127), b = ons(h, 0, 59);
    require(a.size() == 96 && b.size() == 64);
    for (u32 i = 0; i < a.size(); ++i) require(a[i].time == 80 * i);
    for (u32 i = 0; i < b.size(); ++i) require(b[i].time == 120 * i);
}

static void styles_and_off() {
    Host h;
    configure(h, 0, R16, machine::UpDown, 2, 0);
    configure(h, 1, R16, machine::Down, 1, -24);
    h.press(60); h.press(67);
    h.step(60 * 6);
    require(pitches(ons(h, 60, 127)) == Notes({60, 67, 72, 79, 72, 67}));
    require(pitches(ons(h, 0, 59)) == Notes({43, 36, 43, 36, 43, 36}));

    // OFF silences only its own arp.
    for (u32 muted = 0; muted < 2; ++muted) {
        Host one;
        defaults(one);
        configure(one, muted, muted ? R8 : R16, machine::Off, 1, muted ? -12 : 0);
        one.press(60); one.press(64);
        one.step(600);
        require(ons(one, 0, 59).empty() == (muted == 1));
        require(ons(one, 60, 127).empty() == (muted == 0));
    }
}

// Arp A alone, every step a hit, on C E G held in the order E G C.
static Notes play_a(u32 style, u32 count) {
    Host h;
    configure(h, 0, R16, style, 1, 0);
    configure(h, 1, R16, machine::Off, 1, 0);
    h.press(64); h.press(67); h.press(60);
    h.step(60 * (count - 1) + 1);
    return pitches(ons(h, 0, 127));
}

// Every shared pattern, as Chain Arp, Seed Arp and Euclidean Arp play it.
static void shared_patterns() {
    struct Case { u32 style; Notes notes; };
    const Case cases[] = {
        {machine::Up, {60, 64, 67, 60}}, {machine::Down, {67, 64, 60, 67}},
        {machine::UpDown, {60, 64, 67, 64, 60}}, {machine::DownUp, {67, 64, 60, 64, 67}},
        {machine::UpDownBoth, {60, 64, 67, 67, 64, 60, 60}}, {machine::DownUpBoth, {67, 64, 60, 60, 64, 67, 67}},
        {machine::Order, {64, 67, 60, 64}},
        {machine::Converge, {60, 67, 64, 60}}, {machine::Diverge, {64, 67, 60, 64}},
        {machine::Pinky, {60, 67, 64, 67, 60}}, {machine::Thumb, {60, 64, 60, 67, 60}},
        {machine::Stair, {60, 64, 67, 60}}, {machine::StairUpFalling, {67, 64, 60, 67}},
        {machine::StairDownFalling, {67, 64, 60, 67}}, {machine::StairDownRising, {60, 64, 67, 60}},
    };
    for (const Case& c : cases) require(play_a(c.style, static_cast<u32>(c.notes.size())) == c.notes);
    for (u32 style : {u32(machine::Shuffle), u32(machine::Random), u32(machine::Drunk)})
        for (u32 pitch : play_a(style, 60)) require(pitch == 60 || pitch == 64 || pitch == 67);

    // CHORD: the whole chord from one record, highest first and lowest last, at
    // the same moment, while B carries on with its own notes on other keys.
    Host h;
    configure(h, 0, R8, machine::Chord, 1, 0);
    configure(h, 1, R16, machine::Up, 1, -12);
    h.press(60); h.press(64); h.press(67);
    h.step(240 + 1);
    const std::vector<Event> a = ons(h, 60, 127), b = ons(h, 0, 59);
    require(pitches(a) == Notes({67, 64, 60, 67, 64, 60, 67, 64, 60}));
    for (u32 i = 0; i < a.size(); ++i) require(a[i].time == 120 * (i / 3));
    require(pitches(b) == Notes({48, 52, 55, 48, 52}));
    require(h.stale_updates == 0 && h.largest_drain <= 8);
    // CHD/1: the chord, then up; a chord shows its lowest note.
    require(play_a(machine::ChordThenUp, 4) == Notes({67, 64, 60, 60, 64, 67}));
    machine::Display shown = {};
    machine::display(h.state, h.controls, shown);
    require(std::strncmp(shown.text, "C4", 2) == 0);
}

static void transpose() {
    Host h;
    configure(h, 0, R16, machine::Up, 1, 24);
    configure(h, 1, R16, machine::Up, 1, -24);
    h.press(60); h.press(64);
    h.step(60 * 4);
    require(pitches(ons(h, 72, 127)) == Notes({84, 88, 84, 88}));
    require(pitches(ons(h, 0, 50)) == Notes({36, 40, 36, 40}));

    // A note pushed past MIDI's range is skipped, and the arpeggio moves on.
    Host top;
    configure(top, 0, R16, machine::Up, 1, 24);
    configure(top, 1, R16, machine::Off, 1, 0);
    top.press(100); top.press(110);
    top.step(60 * 4);
    require(pitches(top.events.empty() ? std::vector<Event>() : ons(top, 0, 127)) == Notes({124, 124}));
    require(ons(top, 0, 127)[0].time == 0 && ons(top, 0, 127)[1].time == 120);
}

static void carriers() {
    // Two or more keys: the arps never share a key, so no note is struck again
    // while it sounds. Every note-on is a step of its own arp.
    for (u32 keys = 2; keys <= 4; ++keys) {
        Host h;
        configure(h, 0, R16, machine::Random, 2, 0);
        configure(h, 1, R8T, machine::Random, 2, -24);
        for (u32 k = 0; k < keys; ++k) h.press(60 + 3 * k);
        h.step(240 * 40);
        for (const Event& e : ons(h, 60, 127)) require(e.time % 60 == 0);
        for (const Event& e : ons(h, 0, 59)) require(e.time % 80 == 0);
        require(ons(h, 0, 59).size() == 120 && ons(h, 60, 127).size() == 160);
        require(h.stale_updates == 0 && h.largest_drain <= 8);
    }
    // One key: both arps still play, sharing it, and nothing hangs.
    Host one;
    defaults(one);
    one.press(60);
    one.step(120 * 8);
    require(!ons(one, 60, 127).empty() && !ons(one, 0, 59).empty());
    one.release(60);
    require(one.sounding() == 0);
}

static void hand_played_chords() {
    // A key arriving alone waits about 15 ms (7 steps without a tempo) for the rest
    // of a hand-played chord: both arps then start on its lowest note, and nothing
    // is struck twice. The steps still count from the first key.
    const u32 wait = machine::kDefaultChordWindow;
    Host h;
    defaults(h);
    h.press(67);
    h.step(2);
    h.press(64);
    h.press(60);
    h.step(wait);
    require(pitches(ons(h, 60, 127)) == Notes({60}) && pitches(ons(h, 0, 59)) == Notes({48}));
    require(ons(h, 0, 127)[0].time == wait && h.sounding() == 2);
    h.step(60 - wait);
    require(ons(h, 60, 127).back().pitch == 64 && ons(h, 60, 127).back().time == 60);

    // Keys arriving together start at once; a lone key waits.
    Host together;
    defaults(together);
    together.press(60); together.press(64);
    together.step(1);
    require(ons(together, 0, 127).size() == 2 && ons(together, 0, 127)[0].time == 0);
    Host alone;
    defaults(alone);
    alone.press(60);
    alone.step(wait);
    require(alone.events.empty());
    alone.step(1);
    require(ons(alone, 0, 127).size() == 2 && ons(alone, 0, 127)[0].time == wait);

    // About 15 ms at any tempo, under half the faster arp's step.
    require(machine::chord_window(1200, 60) == 7 && machine::chord_window(600, 60) == 3);
    require(machine::chord_window(3000, 20) == 10 && machine::chord_window(0, 60) == 7);
}

static void cleanup() {
    Host h;
    defaults(h);
    for (u32 i = 0; i < 9; ++i) h.press(60 + i);
    require(h.passed_through == 0 && h.held == 8 && !h.find(68));
    h.release(68);
    require(h.state.active == 0xff);
    h.step(500);
    // Release keys one by one in the middle of notes.
    for (u32 i = 0; i < 8; ++i) {
        h.release(60 + i);
        h.step(37);
        require(h.stale_updates == 0);
    }
    require(h.sounding() == 0 && h.state.active == 0);
    h.press(62); h.press(65);
    h.step(30);
    h.all_notes_off();
    require(h.sounding() == 0);
    h.step(300);
    require(h.sounding() == 0 && h.largest_drain <= 8);
}

static void screen() {
    Host h;
    configure(h, 0, R16T, machine::DownUp, 3, -7);
    configure(h, 1, R2T, machine::Off, 4, 24);
    player::Display idle = {};
    machine::display(h.state, h.controls, idle);
    const char* const expected[8] = {"1/16t", "dn/up", "3", "-7", "1/2t", "off", "4", "+24"};
    for (u32 i = 0; i < 8; ++i) {
        require(std::memchr(idle.values[i], 0, sizeof idle.values[i]) != nullptr);
        require(std::strcmp(idle.values[i], expected[i]) == 0);
    }
    require(std::strcmp(idle.text, "ready") == 0 && idle.lit == 0);

    Host p;
    defaults(p);
    p.press(61); p.press(67);
    p.step(1);
    player::Display on = {};
    machine::display(p.state, p.controls, on);
    require(std::strcmp(on.text, "C#4  C#3") == 0);
    require(on.lit == ((1u << 1) | (1u << (12 + 1))));
    configure(p, 1, R8, machine::Off, 1, -12);
    player::Display muted = {};
    machine::display(p.state, p.controls, muted);
    require(std::strcmp(muted.text, "C#4  -") == 0 && muted.lit == (1u << 1));
}

int main() {
    static_assert(sizeof(machine::State) <= player::state_bytes, "State fits the SDK's area");
    both_arps_play();
    gates_and_idle_keys();
    independent_rates();
    styles_and_off();
    shared_patterns();
    transpose();
    carriers();
    hand_played_chords();
    cleanup();
    screen();
    std::puts("twin_arp: all checks passed");
    return 0;
}
