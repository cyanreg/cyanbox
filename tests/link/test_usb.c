/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* An all-in-one on USB (src/link/usb.h, CBLinkOptions.usb_card), in a
 * directory standing in for / with sound cards of the test's own: a card's
 * MIDI port gets rekordbox's activation, twelve bytes, every 200 ms, found
 * by its id (case aside) or its number. A card that is not there is looked
 * for again every second and written to once it comes; one whose port
 * goes (a FIFO whose reader closes, as a unit unplugged) is found again by
 * its id, under another number. Stopped, nothing more is written. A set
 * with a card sends while it runs, and one without sends nothing. */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cyanbox/link.h"

#include "src/link/usb.h"
#include "tests/formats/test.h"

static const unsigned char activate[12] = {
    0xf0, 0x00, 0x40, 0x05, 0x00, 0x00, 0x03, 0x0d, 0x00, 0x50, 0x01, 0xf7,
};

static char root[512];

static void path_of(char *out, size_t cap, const char *rel)
{
    snprintf(out, cap, "%s/%s", root, rel);
}

static void mkdirs(const char *rel)
{
    char p[1024];
    path_of(p, sizeof(p), rel);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    }
    mkdir(p, 0755);
}

static void put(const char *rel, const char *text)
{
    char p[1024];
    path_of(p, sizeof(p), rel);
    FILE *f = fopen(p, "w");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* A card: its id, and an empty file for its MIDI port. */
static void card(int n, const char *id)
{
    char rel[256];
    snprintf(rel, sizeof(rel), "sys/class/sound/card%d", n);
    mkdirs(rel);
    snprintf(rel, sizeof(rel), "sys/class/sound/card%d/id", n);
    char text[64];
    snprintf(text, sizeof(text), "%s\n", id);
    put(rel, text);
    snprintf(rel, sizeof(rel), "dev/snd/midiC%dD0", n);
    put(rel, "");
}

static long size_of(const char *rel)
{
    char p[1024];
    struct stat st;
    path_of(p, sizeof(p), rel);
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

/* Every twelve bytes the message. */
static bool all_activations(const char *rel)
{
    char p[1024];
    path_of(p, sizeof(p), rel);
    FILE *f = fopen(p, "rb");
    if (!f)
        return false;
    unsigned char b[12];
    bool ok = true;
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0)
        ok &= n == sizeof(b) && !memcmp(b, activate, sizeof(b));
    fclose(f);
    return ok;
}

static void wait_ms(int ms)
{
    usleep((useconds_t)ms * 1000);
}

/* Until `u` has sent `n` in all, `ms` at most: the time it took, -1 never. */
static int64_t until_sent(CBLinkUsb *u, int n, int ms)
{
    int64_t t0 = cb_time_us();
    while (cb_link_usb_sent(u) < n) {
        if (cb_time_us() - t0 > ms * INT64_C(1000))
            return -1;
        wait_ms(10);
    }
    return cb_time_us() - t0;
}

static void by_id_and_number(void)
{
    card(0, "sofhdadsp");
    card(3, "XDJRX3");
    CBLinkUsb *u = NULL;
    CHECK_EQ(cb_link_usb_start(&u, "", root), CB_ERROR(EINVAL));
    CHECK(!u);

    /* By its id, case aside: one at once, then every 200 ms. */
    CHECK_EQ(cb_link_usb_start(&u, "xdjrx3", root), 0);
    CHECK(until_sent(u, 1, 1000) >= 0);
    int64_t t = until_sent(u, 3, 2000);
    CHECK(t >= 350 * 1000);
    cb_link_usb_stop(&u);
    CHECK(!u);
    long n = size_of("dev/snd/midiC3D0");
    CHECK(n >= 36 && n % 12 == 0);
    CHECK(all_activations("dev/snd/midiC3D0"));
    CHECK_EQ(size_of("dev/snd/midiC0D0"), 0);
    /* Stopped, nothing more. */
    wait_ms(450);
    CHECK_EQ(size_of("dev/snd/midiC3D0"), n);

    /* By its number. */
    put("dev/snd/midiC3D0", "");
    CHECK_EQ(cb_link_usb_start(&u, "3", root), 0);
    CHECK(until_sent(u, 2, 1000) >= 0);
    cb_link_usb_stop(&u);
    CHECK(size_of("dev/snd/midiC3D0") >= 24);
    CHECK(all_activations("dev/snd/midiC3D0"));
    CHECK_EQ(size_of("dev/snd/midiC0D0"), 0);
}

/* Not there yet: looked for every second, written to once it comes. */
static void comes_later(void)
{
    CBLinkUsb *u = NULL;
    CHECK_EQ(cb_link_usb_start(&u, "XDJRX2", root), 0);
    wait_ms(400);
    CHECK_EQ(cb_link_usb_sent(u), 0);
    card(5, "XDJRX2");
    CHECK(until_sent(u, 2, 2000) >= 0);
    cb_link_usb_stop(&u);
    CHECK(size_of("dev/snd/midiC5D0") >= 24 && all_activations("dev/snd/midiC5D0"));
}

/* A FIFO's reader closing is a unit unplugged: the port is found again by
 * its id, plugged in as another card. */
static int fifo_card(int n, const char *id)
{
    char rel[256], p[1024];
    snprintf(rel, sizeof(rel), "sys/class/sound/card%d", n);
    mkdirs(rel);
    snprintf(rel, sizeof(rel), "sys/class/sound/card%d/id", n);
    put(rel, id);
    snprintf(rel, sizeof(rel), "dev/snd/midiC%dD0", n);
    path_of(p, sizeof(p), rel);
    unlink(p);
    CHECK_EQ(mkfifo(p, 0644), 0);
    int fd = open(p, O_RDONLY | O_NONBLOCK);
    CHECK(fd >= 0);
    return fd;
}

static void unplugged(void)
{
    int rd = fifo_card(7, "XDJRX3B");
    CBLinkUsb *u = NULL;
    CHECK_EQ(cb_link_usb_start(&u, "XDJRX3B", root), 0);
    CHECK(until_sent(u, 2, 1000) >= 0);
    unsigned char b[64];
    CHECK_EQ(read(rd, b, 12), 12);
    CHECK(!memcmp(b, activate, 12));
    /* Gone: its directory and its port too. */
    close(rd);
    char p[1024];
    path_of(p, sizeof(p), "sys/class/sound/card7/id");
    unlink(p);
    path_of(p, sizeof(p), "dev/snd/midiC7D0");
    unlink(p);
    wait_ms(500);
    int was = cb_link_usb_sent(u);
    wait_ms(500);
    CHECK_EQ(cb_link_usb_sent(u), was);
    /* Back as card 8. */
    rd = fifo_card(8, "XDJRX3B");
    CHECK(until_sent(u, was + 2, 2500) >= 0);
    ssize_t got = read(rd, b, sizeof(b));
    CHECK(got >= 12 && !memcmp(b, activate, 12));
    cb_link_usb_stop(&u);
    close(rd);
}

/* A set sends while it runs, on no interface at all; one without a card
 * sends nothing. */
static void with_a_set(void)
{
    card(9, "AIO9");
    CBLinkOptions o;
    cb_link_options_default(&o);
    o.interface = "cbnone0";
    o.test_usb_root = root;
    CBLinkSet *s = NULL;
    CHECK_EQ(cb_link_set_start(&s, &o), 0);
    wait_ms(500);
    cb_link_set_stop(&s);
    CHECK_EQ(size_of("dev/snd/midiC9D0"), 0);

    o.usb_card = "AIO9";
    CHECK_EQ(cb_link_set_start(&s, &o), 0);
    int64_t t0 = cb_time_us();
    while (size_of("dev/snd/midiC9D0") < 24 && cb_time_us() - t0 < 2000000)
        wait_ms(10);
    cb_link_set_stop(&s);
    long n = size_of("dev/snd/midiC9D0");
    CHECK(n >= 24 && all_activations("dev/snd/midiC9D0"));
    wait_ms(450);
    CHECK_EQ(size_of("dev/snd/midiC9D0"), n);
}

int main(void)
{
    cb_log_set_level(CB_LOG_ERROR);
    signal(SIGPIPE, SIG_IGN);
    const char *dir = getenv("CB_TEST_DIR");
    snprintf(root, sizeof(root), "%s", dir ? dir : "build/test-link-usb");
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0)
        return 1;
    mkdirs("dev/snd");
    by_id_and_number();
    comes_later();
    unplugged();
    with_a_set();
    return test_result("test_usb");
}
