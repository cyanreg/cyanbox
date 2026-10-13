/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The track queued to follow (rx_deck_queue_next) against the owner
 * changing its mind as the deck moves on into it, on the null backend with
 * its own thread, as the interface meets it: the next dropped
 * (rx_deck_clear_next), replaced by another, a new track loaded and
 * another queued after it, or a loop set in the old track. With MASTER
 * TEMPO off, the owner acts between the real-time side's move and the
 * callback's publication of it; with it on, inside the shifter's delay,
 * where what is heard is still the old track while the next is read. Then
 * the same with the owner's timing left to chance, which lands on both
 * sides of the move, and on both sides of the owner hearing of it.
 *
 * The owner learns of the move from the published state only; to act in
 * the window on purpose, the test peeks at the real-time side's track
 * (an atomic load of a field the real-time thread writes; a test's
 * liberty).
 *
 * Every buffer handed to the player lives in pages of its own, and the
 * release callback makes them inaccessible instead of freeing them: a
 * read after the release, on any thread, faults at once, with
 * AddressSanitizer or without. A buffer released twice, or never, is
 * counted. After every round the deck plays the track the owner says it
 * plays: the one it moved on into when the change came too late (the
 * owner adopts it, and lets go of what was to replace it), the one loaded
 * after it, never a track released; and a loop meant for the old track is
 * never played in the new one.
 *
 * Last, in manual mode, MASTER TEMPO switched on in the callback of no
 * frames that also moves the deck on: the crossfade's old sound read the
 * old track after the owner had let go of it. */

#include "rxbox/player.h"

#include <stdio.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "src/player/player_internal.h"

#define RATE    48000
#define PERIOD  256
#define ROUNDS  40

static int failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            failures++;                                               \
        }                                                             \
    } while (0)

/* ------------------------------------------------------------- buffers */

#define MAX_BUFS 4096

typedef struct Buf {
    RXAudio *audio;
    size_t   size;
    int      released;
} Buf;

static Buf bufs[MAX_BUFS];
static int nb_bufs;

static size_t page_round(size_t n)
{
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    return (n + pg - 1) / pg * pg;
}

/* A stereo buffer in pages of its own: a quiet tone, so a deck reading it
 * is never silent. */
static RXAudio *make_buf(double seconds)
{
    if (nb_bufs == MAX_BUFS)
        abort();
    RXAudio *a = calloc(1, sizeof(*a));
    if (!a)
        abort();
    a->rate = RATE;
    a->channels = 2;
    a->nb_frames = (int64_t)(seconds * RATE);
    size_t size = page_round(sizeof(float) * 2 * (size_t)a->nb_frames);
    a->samples = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a->samples == MAP_FAILED)
        abort();
    for (int64_t i = 0; i < a->nb_frames; i++)
        a->samples[2 * i] = a->samples[2 * i + 1] = 0.25f * (float)((i % 97) - 48) / 48.0f;
    bufs[nb_bufs++] = (Buf) { .audio = a, .size = size };
    return a;
}

static Buf *find(const RXAudio *a)
{
    for (int i = 0; i < nb_bufs; i++)
        if (bufs[i].audio == a)
            return &bufs[i];
    return NULL;
}

/* The pages go (their memory returned) and stay mapped without access:
 * whatever reads them from here on faults. */
static void on_release(void *opaque, RXAudio *a)
{
    Buf *b = find(a);
    CHECK(b, "released a buffer it was never given");
    if (!b)
        return;
    CHECK(!b->released, "a buffer released twice");
    b->released++;
    madvise(a->samples, b->size, MADV_DONTNEED);
    mprotect(a->samples, b->size, PROT_NONE);
}

static bool released(const RXAudio *a)
{
    Buf *b = find(a);
    return b && b->released;
}

/* -------------------------------------------------------------- helpers */

/* period < 0: manual mode. */
static RXPlayer *make_player(int period)
{
    RXPlayerOptions opt;
    rx_player_options_default(&opt);
    opt.backend = RX_AUDIO_NULL;
    opt.rate = RATE;
    opt.period = period;
    opt.release = on_release;
    RXPlayer *p = NULL;
    int ret = rx_player_create(&p, &opt);
    CHECK(ret == 0 && p, "create: %d", ret);
    if (!p)
        exit(1);
    return p;
}

static RXDeckState st(RXPlayer *p)
{
    RXDeckState s;
    rx_deck_state(p, 0, &s);
    return s;
}

static void sleep_us(int us)
{
    struct timespec ts = { .tv_sec = us / 1000000, .tv_nsec = (long)(us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

/* The track the real-time side reads, as it writes it. */
static uint64_t read_side(RXPlayer *p)
{
    return __atomic_load_n(&p->rt[0].src.id, __ATOMIC_RELAXED);
}

/* Waits (calling into the player, which is what lets it notice) until the
 * deck's state says `done`, up to two seconds. */
static bool wait_for(RXPlayer *p, bool (*done)(const RXDeckState *s, const void *arg),
                     const void *arg)
{
    for (int i = 0; i < 2000; i++) {
        RXDeckState s = st(p);
        if (done(&s, arg))
            return true;
        sleep_us(1000);
    }
    return false;
}

/* The deck moved on (heard), or stopped where it was. */
static bool moved_or_stopped(const RXDeckState *s, const void *arg)
{
    return s->advanced != *(const unsigned *)arg || !s->playing;
}

static bool settled(const RXDeckState *s, const void *arg)
{
    return !s->has_next;
}

/* Every command sent so far applied. */
static bool applied(const RXDeckState *s, const void *arg)
{
    return s->serial >= *(const uint64_t *)arg;
}

static bool past(const RXDeckState *s, const void *arg)
{
    return s->position > *(const double *)arg;
}

/* ---------------------------------------------------------------- rounds */

enum What {
    CLEAR,      /* rx_deck_clear_next */
    REPLACE,    /* rx_deck_queue_next of another */
    RELOAD,     /* rx_deck_load of another, then a next queued after it */
    LOOP,       /* rx_deck_set_loop around a's last frames */
};

static const char *names[] = { "clear", "replace", "load and queue", "loop" };

/* A buffer the player refused stays the test's: accounted as released. */
static void refused(RXAudio *a)
{
    on_release(NULL, a);
}

/* One round: A playing its last 20 ms with N queued; the owner's change of
 * mind right after the real-time side moved on into N (`peek`), or after
 * a random wait. Then what the deck plays is checked against what was
 * asked and when. */
static void round_once(RXPlayer *p, enum What what, bool mt, bool peek, unsigned seed)
{
    RXAudio *a = make_buf(0.25), *n = make_buf(0.5);
    RXAudio *m = what == REPLACE || what == RELOAD ? make_buf(0.5) : NULL;
    RXAudio *x = what == RELOAD ? make_buf(0.5) : NULL;
    CHECK(rx_deck_load(p, 0, a, NULL) == 0, "load a");
    CHECK(rx_deck_set_master_tempo(p, 0, mt), "master tempo");
    CHECK(rx_deck_queue_next(p, 0, n, NULL) == 0, "queue n");
    uint64_t a_id = p->own[0].cur.id, n_id = p->own[0].next.id;
    unsigned adv0 = st(p).advanced;
    rx_deck_play_from(p, 0, 0.25 - 0.02);

    /* The load applied: the deck reads a, or n already (MASTER TEMPO reads
     * its delay ahead, longer than what is left of a). */
    int64_t t0 = rx_time_us();
    while (read_side(p) != a_id && read_side(p) != n_id && rx_time_us() - t0 < 2000000)
        sleep_us(100);
    CHECK(read_side(p) == a_id || read_side(p) == n_id, "the load was never applied");
    if (peek) {
        t0 = rx_time_us();
        while (read_side(p) != n_id && rx_time_us() - t0 < 2000000)
            ;
        CHECK(read_side(p) == n_id, "the deck never moved on");
    } else {
        sleep_us((int)(seed % 30000));
    }

    int ret = 0;
    switch (what) {
    case CLEAR:
        rx_deck_clear_next(p, 0);
        break;
    case REPLACE:
        /* Refused when the owner knows already that the deck reads n and
         * does not hear it yet: too late to replace it, too early to
         * follow it. */
        ret = rx_deck_queue_next(p, 0, m, NULL);
        CHECK(ret == 0 || ret == RX_ERROR(EBUSY), "queue m: %d", ret);
        if (ret < 0)
            refused(m);
        break;
    case RELOAD:
        CHECK(rx_deck_load(p, 0, x, NULL) == 0, "load x");
        CHECK(rx_deck_queue_next(p, 0, m, NULL) == 0, "queue m after x");
        break;
    case LOOP:
        rx_deck_set_loop(p, 0, 0.01, 0.05, true);
        break;
    }
    /* What the owner followed when it acted: a, or n when it had heard
     * the move already (then a next queued follows n, and the deck plays
     * on into it after n; a loop is n's). */
    uint64_t followed = what == RELOAD ? 0 : p->own[0].cur.id;
    unsigned adv_act = p->own[0].advanced;

    const char *how = mt ? "master tempo" : "dry";
    if (what == LOOP) {
        /* The loop, around a's start while the deck plays a's end, is a's
         * when the deck still reads a: the playhead is past it, and the
         * deck plays on into n, the loop going with a. When the owner had
         * heard the move already, the loop is n's: n loops there. A loop
         * meant for a that comes after the deck moved on is dropped (by the
         * deck, or by the owner, knowing): either way n plays on through
         * those places, and the owner shows no loop once the move is
         * heard. */
        CHECK(wait_for(p, moved_or_stopped, &adv0), "loop, %s: did not move on", how);
        const RXAudio *on = rx_deck_audio(p, 0);
        if (followed == n_id) {
            sleep_us(100000);
            RXDeckState s = st(p);
            CHECK(on == n && s.loop_enabled && s.playing && s.position >= 0.01 &&
                  s.position <= 0.05, "loop, %s, after the move: n loops %d, playing %d at %f",
                  how, s.loop_enabled, s.playing, s.position);
        } else {
            double beyond = 0.07;
            CHECK(followed == a_id, "loop, %s: the owner followed neither a nor n", how);
            bool through = wait_for(p, past, &beyond);
            RXDeckState s = st(p);
            CHECK(on == n && through && !s.loop_enabled && s.advanced == adv0 + 1,
                  "loop, %s: on %s, loop %d at %f, moved on %u times", how, on == n ? "n" : "?",
                  s.loop_enabled, s.position, s.advanced - adv0);
        }
        CHECK(!released(on), "loop, %s: the deck's track is released", how);
        return;
    }
    if (what == RELOAD) {
        /* Whatever happened before the load (a move into n it heard of,
         * or not), the deck plays x, stopped at its start, with m queued,
         * and does not move on after the load: once the load is applied,
         * and a few callbacks later. */
        uint64_t sent = p->own[0].sent;
        CHECK(wait_for(p, applied, &sent), "%s, %s: the load was never applied", names[what],
              how);
        sleep_us(20000);
        RXDeckState s = st(p);
        CHECK(rx_deck_audio(p, 0) == x && s.has_next && !s.playing && s.advanced == adv_act &&
              adv_act - adv0 <= 1 && !released(x),
              "%s, %s: deck on %s, has_next %d, playing %d, advanced %u (%u at the load, %u "
              "before)", names[what], how, rx_deck_audio(p, 0) == x ? "x" : "another",
              s.has_next, s.playing, s.advanced, adv_act, adv0);
        return;
    }

    /* The deck went on into n (too late to drop it), or into m (it
     * replaced n in time), or stopped at a's end (dropped in time), or,
     * when the owner had heard the move into n, on into m after n;
     * whichever it was, the owner says so once it is heard, and nothing is
     * left queued. */
    CHECK(wait_for(p, settled, NULL), "%s, %s: still queued", names[what], how);
    CHECK(wait_for(p, moved_or_stopped, &adv0), "%s, %s: neither moved on nor stopped",
          names[what], how);
    RXDeckState s = st(p);
    unsigned moves = s.advanced - adv0;
    const RXAudio *on = rx_deck_audio(p, 0);
    const char *on_name = on == a ? "a" : on == n ? "n" : on == m ? "m" : "?";
    if (followed == n_id) {
        bool after_n = what == REPLACE && ret == 0;
        CHECK(moves == 1 + after_n && on == (after_n ? m : n),
              "%s, %s, after the move: the deck plays %s, moved on %u times", names[what], how,
              on_name, moves);
    } else {
        CHECK(followed == a_id, "%s, %s: the owner followed neither a nor n", names[what], how);
        if (peek)
            CHECK(moves == 1 && on == n, "%s, %s, too late: the deck plays %s, moved on %u times",
                  names[what], how, on_name, moves);
        if (moves)
            CHECK(moves == 1 && (on == n || (on == m && ret == 0)),
                  "%s, %s: moved on %u times, into %s", names[what], how, moves, on_name);
        else
            CHECK(on == a && !s.playing && what == CLEAR, "%s, %s: stayed on %s, playing %d",
                  names[what], how, on_name, s.playing);
    }
    CHECK(!released(on), "%s, %s: the deck's track is released", names[what], how);
}

static void run(enum What what, bool mt, bool peek)
{
    RXPlayer *p = make_player(PERIOD);
    int before = nb_bufs;
    for (int r = 0; r < ROUNDS; r++)
        round_once(p, what, mt, peek, (unsigned)r * 2654435761u);
    rx_player_destroy(p);
    int lost = 0, twice = 0;
    for (int i = before; i < nb_bufs; i++) {
        lost += !bufs[i].released;
        twice += bufs[i].released > 1;
    }
    CHECK(!lost && !twice, "%s, %s: %d buffers never released, %d twice", names[what],
          mt ? "master tempo" : "dry", lost, twice);
    printf("%s, %s, %s: %d rounds\n", names[what], mt ? "master tempo" : "dry",
           peek ? "right as it moves on" : "at random", ROUNDS);
}

/* Manual mode: the deck paused near the end of a with n queued, then, in
 * the one callback of no frames in which rx_player_null_advance applies
 * the commands, MASTER TEMPO switched on and the deck played from the end
 * (into n). The switch crossfades from the dry voices as they would have
 * played on, the pause's de-click tail in a among them; the owner hears
 * of the move in that same callback and lets go of a. The old sound is
 * rendered when the switch comes, and the tail's track is kept while the
 * deck says it reads it: a is read only while it is the deck's. */
static void test_manual_switch(void)
{
    RXPlayer *p = make_player(-PERIOD);
    int before = nb_bufs;
    RXAudio *a = make_buf(0.5), *n = make_buf(0.5);
    CHECK(rx_deck_load(p, 0, a, NULL) == 0, "load a");
    CHECK(rx_deck_queue_next(p, 0, n, NULL) == 0, "queue n");
    rx_deck_play_from(p, 0, 0.4);
    rx_player_null_advance(p, 4 * PERIOD);
    rx_deck_pause(p, 0);
    CHECK(rx_deck_set_master_tempo(p, 0, true), "master tempo");
    rx_deck_play_from(p, 0, 0.5);
    rx_player_null_advance(p, 8 * PERIOD);
    RXDeckState s = st(p);
    CHECK(rx_deck_audio(p, 0) == n && s.playing && s.advanced == 1 && released(a) &&
          !released(n), "on n %d, playing %d, advanced %u, a released %d", rx_deck_audio(p, 0) == n,
          s.playing, s.advanced, released(a));
    rx_player_destroy(p);
    for (int i = before; i < nb_bufs; i++)
        CHECK(bufs[i].released == 1, "a buffer released %d times", bufs[i].released);
    printf("master tempo switched on as the deck moves on, manual mode\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int mt = 0; mt < 2; mt++) {
        for (int what = CLEAR; what <= LOOP; what++) {
            run(what, mt, true);
            run(what, mt, false);
        }
    }
    test_manual_switch();

    for (int i = 0; i < nb_bufs; i++) {
        munmap(bufs[i].audio->samples, bufs[i].size);
        free(bufs[i].audio);
    }
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("player next track races: all tests passed\n");
    return 0;
}
