/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* LINK CUE (cyanbox/link.h CBLinkCue, src/link/cue.c): its decisions against
 * a driven clock, then a service on the loopback answering a mixer of the
 * test's own on real sockets.
 *
 * The decisions: nothing while the decks do not play; a deck started
 * takes over, and from then on every timing that names us with LINK CUE
 * on is answered with "go on", its counter echoed and the next 336 frames
 * taken; a mixer that holds enough asking nothing for a while is no
 * pause; a player taking the cue over keeps it until the decks start
 * again; LINK CUE off, nothing; after a timing left unanswered, or a long
 * pause, the first packet takes over again; nobody elected yet, taken
 * over; one mixer at a time, the next
 * once the first is silent for a second; a service stopping lets its
 * mixer go.
 *
 * The loopback: cyanbox's service on 127.0.0.1 and a "DJM-A9", device 33,
 * on 127.0.0.2, with ports of their own. A timing from an address that is
 * not on the link (127.0.0.3), or from the mixer before it is, goes
 * unanswered, and so does one while the decks do not play. On the link,
 * with a deck started, a timing every 7 ms for a second is answered packet
 * for packet: the first takes over, the mixer hands over, the rest go on,
 * each with the counter it answers and the frames in the order taken;
 * with LINK CUE off, nothing. */

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "cyanbox/library.h"
#include "cyanbox/link.h"

#include "src/link/cue.h"
#include "src/link/net.h"
#include "tests/formats/test.h"

#define MS INT64_C(1000)
#define T0 (INT64_C(1000) * 1000000)

/* The owner's audio: frame f of all that was taken is (f, -f) in 16 bits,
 * so what arrives says what was taken, and in what order. */
typedef struct Source {
    int     takes;
    int64_t frames;
} Source;

static int take(void *opaque, int16_t *out, int frames)
{
    Source *s = opaque;
    for (int i = 0; i < frames; i++) {
        out[i * 2] = (int16_t)(s->frames + i);
        out[i * 2 + 1] = (int16_t)-(s->frames + i);
    }
    s->frames += frames;
    s->takes++;
    return frames;
}

/* The frames of an audio packet are frames `first` on, in order. */
static bool frames_from(const CBPktCueAudio *a, int64_t first)
{
    for (int i = 0; i < CB_CUE_FRAMES; i++) {
        if (a->pcm[i * 2] != (int16_t)(first + i) || a->pcm[i * 2 + 1] != (int16_t)-(first + i))
            return false;
    }
    return true;
}

/* -------------------------------------------------------------- decisions */

static uint8_t out[CB_CUE_AUDIO_SIZE];
static CBPktCueAudio got;

/* cb_link_cue_answer to a timing of the mixer at `from` heard by the
 * service `svc`: the mode answered with, -1 for nothing. */
static int answer(CBLinkCue *c, const void *svc, uint32_t from, int64_t now, uint32_t counter,
                  bool on, int source)
{
    CBPktCueTiming t = { .name = "DJM-A9", .mixer = 33, .counter = counter, .on = on,
                         .source = source };
    size_t n = cb_link_cue_answer(c, svc, from, 17, "rekordbox", &t, now, out, sizeof(out));
    if (!n)
        return -1;
    if (n != CB_CUE_AUDIO_SIZE || cb_pkt_cue_audio_parse(&got, out, n) < 0 ||
        got.counter != counter || got.number != 17 || !got.audio || strcmp(got.name, "rekordbox")) {
        fprintf(stderr, "test_cue: a bad answer to timing %u\n", counter);
        failures++;
        return -2;
    }
    return got.mode;
}

static void decisions(void)
{
    Source src = { 0 };
    CBLinkCue *c = NULL;
    CHECK(cb_link_cue_new(&c, NULL, NULL) < 0 && !c);
    CHECK_EQ(cb_link_cue_new(&c, take, &src), 0);
    if (!c)
        return;
    int a, b;                   /* two services */
    const uint32_t djm = 0x0a000021, other = 0x0a000022;
    int64_t now = T0;
    uint32_t n = 100;
    CBLinkCueState st;

    /* The decks idle: nothing sent, the mixer seen all the same. */
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 0), -1);
    cb_link_cue_state_at(c, &st, now);
    CHECK(st.mixer && !strcmp(st.name, "DJM-A9") && st.number == 33 && st.on && !st.source &&
          !st.ours && !st.sending);

    /* A deck starts: taken over with the first 336 frames. */
    cb_link_cue_decks(c, true, true);
    now += 7 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 0), CB_CUE_TAKE);
    CHECK(src.takes == 1 && frames_from(&got, 0));
    /* Elected: every timing goes on, with the frames after. */
    bool all = true;
    for (int i = 1; i <= 20; i++) {
        now += 7 * MS;
        all &= answer(c, &a, djm, now, n++, true, 17) == CB_CUE_GO_ON && frames_from(&got, i * 336);
    }
    CHECK(all && src.takes == 21);
    cb_link_cue_state_at(c, &st, now);
    CHECK(st.mixer && st.on && st.source == 17 && st.ours && st.sending);

    /* The mixer holds enough and asks nothing for a while (the DJM-A9: 58
     * ms as its LINK CUE starts): no pause, the frames go on. Longer than
     * a mixer holds: taken over again. */
    now += 58 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 17), CB_CUE_GO_ON);
    CHECK(frames_from(&got, 21 * 336));
    now += 120 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 17), CB_CUE_GO_ON);
    now += 200 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 17), CB_CUE_TAKE);
    now += 7 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 17), CB_CUE_GO_ON);
    /* A timing that never came: taken over again. */
    now += 14 * MS;
    n++;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 17), CB_CUE_TAKE);

    /* A player takes the cue over: it keeps it while the decks play on. */
    for (int i = 0; i < 20; i++) {
        now += 7 * MS;
        all &= answer(c, &a, djm, now, n++, true, 2) == -1;
        cb_link_cue_decks(c, true, false);
    }
    CHECK(all && src.takes == 26);
    cb_link_cue_state_at(c, &st, now);
    CHECK(st.source == 2 && !st.ours && !st.sending);

    /* The decks start again: taken back, then on. */
    cb_link_cue_decks(c, true, true);
    now += 7 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 2), CB_CUE_TAKE);
    now += 7 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 17), CB_CUE_GO_ON);

    /* LINK CUE off at the mixer: nothing. On again after that pause: the
     * first takes over again (the mixer ran dry), then on. */
    for (int i = 0; i < 10; i++) {
        now += 7 * MS;
        all &= answer(c, &a, djm, now, n++, false, 17) == -1;
    }
    CHECK(all);
    now += 7 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 17), CB_CUE_TAKE);
    now += 7 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 17), CB_CUE_GO_ON);

    /* The decks stop: nothing. A start while they do not play is no claim,
     * and playing on without a start does not take a player's cue. */
    cb_link_cue_decks(c, false, false);
    now += 7 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 17), -1);
    cb_link_cue_decks(c, false, true);
    cb_link_cue_decks(c, true, false);
    now += 7 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 2), -1);
    /* Nobody elected yet: taken over without a start. */
    now += 7 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 0), CB_CUE_TAKE);

    /* Another mixer while this one is there, or the same address heard by
     * another service: not answered. Once this one is silent for a second
     * the other is, and taken over: the decks play. */
    now += 7 * MS;
    CHECK_EQ(answer(c, &a, other, now, 5, true, 0), -1);
    CHECK_EQ(answer(c, &b, djm, now, 5, true, 0), -1);
    now += 1100 * MS;
    cb_link_cue_state_at(c, &st, now);
    CHECK(!st.mixer);
    CHECK_EQ(answer(c, &b, djm, now, 6, true, 3), CB_CUE_TAKE);
    cb_link_cue_state_at(c, &st, now);
    CHECK(st.mixer && st.source == 3 && st.sending);
    /* A service stopping lets its mixer go: the other is answered at once. */
    cb_link_cue_forget(c, &a);
    now += 7 * MS;
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 17), -1);
    cb_link_cue_forget(c, &b);
    cb_link_cue_state_at(c, &st, now);
    CHECK(!st.mixer);
    CHECK_EQ(answer(c, &a, djm, now, n++, true, 17), CB_CUE_TAKE);

    cb_link_cue_free(&c);
    CHECK(!c);
    cb_link_cue_free(&c);
}

/* --------------------------------------------------------------- loopback */

#define LOCAL 0x7F000001u

static int wait_up(CBLink *link, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 20; i++) {
        int number = 0;
        if (cb_link_state(link, &number) == CB_LINK_UP)
            return number;
        usleep(20000);
    }
    return -1;
}

static bool lists(CBLink *link, int number, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 20; i++) {
        CBLinkPlayer p[8];
        int nb = CB_MIN(cb_link_players(link, p, 8), 8);
        for (int k = 0; k < nb; k++) {
            if (p[k].number == number)
                return true;
        }
        usleep(20000);
    }
    return false;
}

static void send_timing(int fd, uint16_t port, uint32_t counter, bool on, int source)
{
    CBPktCueTiming t = { .name = "DJM-A9", .mixer = 33, .counter = counter, .on = on,
                         .source = source };
    uint8_t buf[64];
    CBWr w = cb_wr(buf, sizeof(buf));
    cb_pkt_cue_timing(&w, &t);
    cb_link_send(fd, LOCAL, port, w.p, w.len);
}

/* The next answer on `fd` within `ms`: 1 with it in `a`, 0 for none. */
static int next_answer(const CBLinkIf *ifc, int fd, int ms, CBPktCueAudio *a)
{
    static uint8_t buf[2048];
    int64_t end = cb_time_us() + ms * MS;
    while (1) {
        uint32_t from;
        uint16_t port;
        ssize_t n = cb_link_recv(ifc, fd, buf, sizeof(buf), &from, &port);
        if (n > 0) {
            if (from != LOCAL || cb_pkt_cue_audio_parse(a, buf, (size_t)n) < 0 ||
                n != CB_CUE_AUDIO_SIZE) {
                fprintf(stderr, "test_cue: %zd bytes on the cue port that are no audio packet\n", n);
                failures++;
                continue;
            }
            return 1;
        }
        int64_t left = end - cb_time_us();
        if (left <= 0)
            return 0;
        struct pollfd p = { .fd = fd, .events = POLLIN };
        poll(&p, 1, (int)((left + 999) / 1000));
    }
}

/* Timings every 7 ms from `counter` on for `ms`, unanswered all. */
static bool unanswered(const CBLinkIf *ifc, int fd, uint16_t port, uint32_t *counter, int ms,
                       bool on, int source)
{
    static CBPktCueAudio a;
    bool none = true;
    for (int i = 0; i < ms / 7; i++) {
        send_timing(fd, port, (*counter)++, on, source);
        none &= !next_answer(ifc, fd, 7, &a);
    }
    return none;
}

static void loopback(const char *root)
{
    char *cmd = cb_asprintf("rm -rf '%s' && mkdir -p '%s/lib'", root, root);
    char *libdir = cb_asprintf("%s/lib", root);
    CHECK(cmd && libdir && !system(cmd));
    CBLibrary lib = { 0 };
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
    cb_library_close(&lib);

    /* The mixer's ports on 127.0.0.2, and a stranger's on 127.0.0.3. */
    CBLinkIf mif, sif;
    CHECK_EQ(cb_link_if_find(&mif, "lo", "127.0.0.2", NULL), 0);
    CHECK_EQ(cb_link_if_find(&sif, "lo", "127.0.0.3", NULL), 0);
    uint16_t ann = 0, status = 0, cue = 0, scue = 0;
    int fa = cb_link_udp(&mif, mif.addr, 0, &ann);
    int fs = cb_link_udp(&mif, mif.addr, 0, &status);
    int fc = cb_link_udp(&mif, mif.addr, 0, &cue);
    int f3 = cb_link_udp(&sif, sif.addr, 0, &scue);
    CHECK(fa >= 0 && fs >= 0 && fc >= 0 && f3 >= 0);

    Source src = { 0 };
    CBLinkCue *c = NULL;
    CBLinkCueState st;
    CHECK_EQ(cb_link_cue_new(&c, take, &src), 0);
    CBLinkOptions so;
    cb_link_options_default(&so);
    so.library = libdir;
    so.interface = "lo";
    so.broadcast = "127.0.0.2";
    so.ports = (CBLinkPorts) { 0 };
    so.peer_ports = (CBLinkPorts) { .announce = ann, .status = status, .cue = cue };
    so.cue = c;
    CBLink *link = NULL;
    CHECK_EQ(cb_link_start(&link, &so), 0);
    if (!link || !c || fa < 0 || fc < 0 || f3 < 0)
        goto done;
    CBLinkPorts sp;
    cb_link_bound_ports(link, &sp);
    CHECK(sp.cue != 0);
    CHECK_EQ(wait_up(link, 15000), 17);

    /* Not on the link: the stranger, and the mixer before its keep-alive. */
    cb_link_cue_decks(c, true, true);
    uint32_t counter = 1000;
    CHECK(unanswered(&sif, f3, sp.cue, &counter, 100, true, 0));
    CHECK(unanswered(&mif, fc, sp.cue, &counter, 100, true, 0));
    cb_link_cue_state(c, &st);
    CHECK(!st.mixer);

    /* The mixer joins (a DJM-V5's keep-alive, as rbxport captured it). */
    CBPktKeepAlive k = { .name = "DJM-A9", .proto = 2, .number = 33, .first = 2,
                         .mac = { 0xc8, 0x3d, 0xfc, 0x1d, 0xfe, 0xa2 }, .addr = mif.addr,
                         .peers = 1, .type = 3, .tail = 0x31 };
    uint8_t kb[64];
    CBWr kw = cb_wr(kb, sizeof(kb));
    cb_pkt_keep_alive(&kw, &k);
    cb_link_send(fa, LOCAL, sp.announce, kw.p, kw.len);
    CHECK(lists(link, 33, 3000));

    /* On the link, the decks idle: nothing. */
    cb_link_cue_decks(c, false, false);
    CHECK(unanswered(&mif, fc, sp.cue, &counter, 100, true, 0));
    cb_link_cue_state(c, &st);
    CHECK(st.mixer && !strcmp(st.name, "DJM-A9") && st.number == 33 && st.on && !st.sending);

    /* A deck starts: a second of timings, each answered. */
    cb_link_cue_decks(c, true, true);
    int sent = 0, answered = 0, taken_over = 0, out_of_order = 0, wrong_frames = 0, elected = 0;
    uint32_t last = 0;
    static CBPktCueAudio a;
    int64_t next = cb_time_us(), end = next + 1000 * MS;
    while (cb_time_us() < end) {
        send_timing(fc, sp.cue, counter++, true, elected);
        sent++;
        next += 7 * MS;
        while (next_answer(&mif, fc, (int)CB_MAX((next - cb_time_us()) / MS, 0), &a)) {
            out_of_order += a.counter >= counter || (last && a.counter <= last) || a.number != 17 ||
                            !a.audio;
            wrong_frames += !frames_from(&a, (int64_t)answered * CB_CUE_FRAMES);
            last = a.counter;
            answered++;
            if (a.mode == CB_CUE_TAKE) {
                taken_over++;
                /* Elected: the handover, and the next timings name us. */
                uint8_t hb[64];
                CBWr hw = cb_wr(hb, sizeof(hb));
                cb_pkt_cue_handover(&hw, "DJM-A9", 33);
                cb_link_send(fc, LOCAL, sp.cue, hw.p, hw.len);
                elected = 17;
            } else if (a.mode != CB_CUE_GO_ON) {
                out_of_order++;
            }
        }
    }
    /* What is still on its way. */
    while (next_answer(&mif, fc, 20, &a)) {
        wrong_frames += !frames_from(&a, (int64_t)answered * CB_CUE_FRAMES);
        answered++;
    }
    if (getenv("CB_TEST_VERBOSE"))
        fprintf(stderr, "  %d timings, %d answered, %d taking over\n", sent, answered, taken_over);
    CHECK(sent > 100 && answered >= sent - 2 && answered <= sent);
    CHECK_EQ(taken_over, 1);
    CHECK_EQ(out_of_order, 0);
    CHECK_EQ(wrong_frames, 0);
    CHECK_EQ(src.takes, answered);
    cb_link_cue_state(c, &st);
    CHECK(st.mixer && st.on && st.source == 17 && st.ours && st.sending);

    /* LINK CUE off at the mixer: nothing. */
    CHECK(unanswered(&mif, fc, sp.cue, &counter, 100, false, 17));

done:
    cb_link_stop(&link);
    /* The service gone, its mixer with it. */
    if (c) {
        cb_link_cue_state(c, &st);
        CHECK(!st.mixer);
    }
    cb_link_cue_free(&c);
    int fds[] = { fa, fs, fc, f3 };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(fds); i++) {
        if (fds[i] >= 0)
            close(fds[i]);
    }
    free(cmd);
    free(libdir);
}

int main(void)
{
    cb_log_set_level(CB_LOG_WARN);
    decisions();
    const char *dir = getenv("CB_TEST_DIR");
    loopback(dir ? dir : "build/test-link-cue");
    return test_result("test_cue");
}
