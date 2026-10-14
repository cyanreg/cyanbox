/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The database server's message codec (dbmsg.h): parse, write, menu
 * items.
 *
 * The parser is incremental over a TCP stream: it reads a message from
 * the front of what has arrived and says whether it is whole, needs more
 * or cannot be one. "Needs more" is only ever the answer when every byte
 * so far could start a message: the fixed bytes of the header are checked
 * as soon as they are there, and a field's tag before its value, so a
 * stream of garbage is refused at once rather than waited on. Lengths are
 * checked against the caller's cap before more bytes are waited for, so
 * a peer cannot make the caller buffer more than the cap. */

#include <stdlib.h>
#include <string.h>

#include "rxbox/common.h"

#include "dbmsg.h"

const uint8_t rx_db_greeting[RX_DB_GREETING_LEN] = { 0x11, 0x00, 0x00, 0x00, 0x01 };

/* Before the argument tags: 11 magic, 11 txid, 10 kind, 0f count, then
 * the tag list as a blob, 14 and its length (rbxport's measurement of
 * rekordbox 7.2.11, rbl-dbserver src/lib.rs HEADER_LEN). */
#define HEADER_LEN 20

/* The header's fixed bytes: the field tags and the magic. */
static const struct { uint8_t at, v; } fixed[] = {
    { 0, RX_DB_FIELD_NUM4 }, { 1, 0x87 }, { 2, 0x23 }, { 3, 0x49 }, { 4, 0xae },
    { 5, RX_DB_FIELD_NUM4 }, { 10, RX_DB_FIELD_NUM2 }, { 13, RX_DB_FIELD_NUM1 },
    { 15, RX_DB_FIELD_BLOB },
};

/* Whether the field tag `field` carries an argument declared as `type`. */
static bool field_fits(uint8_t type, uint8_t field)
{
    switch (type) {
    case RX_DB_NUM:  return field == RX_DB_FIELD_NUM1 || field == RX_DB_FIELD_NUM2 ||
                            field == RX_DB_FIELD_NUM4;
    case RX_DB_BLOB: return field == RX_DB_FIELD_BLOB;
    case RX_DB_STR:  return field == RX_DB_FIELD_STRING;
    }
    return false;
}

/* What to say when `need` bytes from `pos` are not all there: more, or
 * too long for the cap. */
static int short_of(size_t pos, uint64_t need, size_t max)
{
    return pos + need > max ? RX_ERROR(E2BIG) : 0;
}

int rx_dbmsg_parse(const uint8_t *buf, size_t len, size_t max, RXDbMsg *m)
{
    for (size_t i = 0; i < RX_ARRAY_ELEMS(fixed); i++)
        if (fixed[i].at < len && buf[fixed[i].at] != fixed[i].v)
            return RX_ERROR(EPROTO);
    if (len < HEADER_LEN)
        return short_of(0, HEADER_LEN, max);

    RXRd r = rx_rd(buf, len);
    m->txid = rx_rd_at_u32(&r, 6);
    m->kind = rx_rd_at_u16(&r, 11);
    int count = rx_rd_at_u8(&r, 14);
    uint32_t nb_tags = rx_rd_at_u32(&r, 16);
    /* The list is `count` long, or a fixed twelve zero-padded (dbmsg.h). */
    if (count > RX_DB_MAX_ARGS || nb_tags < (uint32_t)count || nb_tags > RX_DB_MAX_ARGS)
        return RX_ERROR(EPROTO);
    r.pos = HEADER_LEN;
    if (rx_rd_left(&r) < nb_tags)
        return short_of(r.pos, nb_tags, max);
    const uint8_t *tags = rx_rd_bytes(&r, nb_tags);
    for (int i = 0; i < count; i++)
        if (tags[i] != RX_DB_NUM && tags[i] != RX_DB_BLOB && tags[i] != RX_DB_STR)
            return RX_ERROR(EPROTO);

    m->nb_args = count;
    for (int i = 0; i < count; i++) {
        RXDbArg *a = &m->args[i];
        *a = (RXDbArg) { .type = tags[i] };
        /* A blob after a number 0 is declared and not sent. */
        if (tags[i] == RX_DB_BLOB && i > 0 && m->args[i - 1].type == RX_DB_NUM &&
            m->args[i - 1].num == 0)
            continue;
        if (!rx_rd_left(&r))
            return short_of(r.pos, 1, max);
        uint8_t field = rx_rd_u8(&r);
        if (!field_fits(tags[i], field))
            return RX_ERROR(EPROTO);
        /* A number's value, or a blob's or a string's 4-byte length. */
        size_t width = field == RX_DB_FIELD_NUM1 ? 1 : field == RX_DB_FIELD_NUM2 ? 2 : 4;
        if (rx_rd_left(&r) < width)
            return short_of(r.pos, width, max);
        if (field == RX_DB_FIELD_NUM1)
            a->num = rx_rd_u8(&r);
        else if (field == RX_DB_FIELD_NUM2)
            a->num = rx_rd_u16(&r);
        else
            a->num = rx_rd_u32(&r);
        if (field != RX_DB_FIELD_BLOB && field != RX_DB_FIELD_STRING)
            continue;
        /* A string's length counts UTF-16 units, its NUL included. */
        uint64_t bytes = field == RX_DB_FIELD_STRING ? 2 * (uint64_t)a->num : a->num;
        if (rx_rd_left(&r) < bytes)
            return short_of(r.pos, bytes, max);
        a->len = (uint32_t)bytes;
        a->p = rx_rd_bytes(&r, (size_t)bytes);
        a->num = 0;
    }
    if (r.err || r.pos > max)
        return r.pos > max ? RX_ERROR(E2BIG) : RX_ERROR(EPROTO);
    return (int)r.pos;
}

void rx_dbmsg_write(RXWr *w, const RXDbMsg *m)
{
    int count = RX_CLAMP(m->nb_args, 0, RX_DB_MAX_ARGS);
    rx_wr_u8(w, RX_DB_FIELD_NUM4);
    rx_wr_u32(w, RX_DB_MAGIC);
    rx_wr_u8(w, RX_DB_FIELD_NUM4);
    rx_wr_u32(w, m->txid);
    rx_wr_u8(w, RX_DB_FIELD_NUM2);
    rx_wr_u16(w, m->kind);
    rx_wr_u8(w, RX_DB_FIELD_NUM1);
    rx_wr_u8(w, (uint8_t)count);
    rx_wr_u8(w, RX_DB_FIELD_BLOB);
    rx_wr_u32(w, (uint32_t)count);
    /* An argument of no known type goes as a number: what is written
     * always parses. */
    for (int i = 0; i < count; i++) {
        uint8_t t = m->args[i].type;
        rx_wr_u8(w, t == RX_DB_STR || t == RX_DB_BLOB ? t : RX_DB_NUM);
    }
    for (int i = 0; i < count; i++) {
        const RXDbArg *a = &m->args[i];
        switch (a->type) {
        default:
            rx_wr_u8(w, RX_DB_FIELD_NUM4);
            rx_wr_u32(w, a->num);
            break;
        case RX_DB_BLOB:
            if (i > 0 && m->args[i - 1].type != RX_DB_STR && m->args[i - 1].type != RX_DB_BLOB &&
                m->args[i - 1].num == 0)
                break;
            rx_wr_u8(w, RX_DB_FIELD_BLOB);
            rx_wr_u32(w, a->len);
            rx_wr_bytes(w, a->p, a->len);
            break;
        case RX_DB_STR:
            rx_wr_u8(w, RX_DB_FIELD_STRING);
            if (a->str) {
                rx_wr_u32(w, (uint32_t)rx_link_utf16_units(a->str) + 1);
                rx_wr_utf16(w, a->str, true, true);
            } else {
                /* As parsed: the units with their NUL, as they came (an
                 * odd last byte cannot be one), so a message read and
                 * written again is the same. */
                uint32_t units = a->len / 2;
                rx_wr_u32(w, units);
                if (units)
                    rx_wr_bytes(w, a->p, (size_t)units * 2);
            }
            break;
        }
    }
}

void rx_dbmsg_numbers(RXWr *w, uint32_t txid, uint16_t kind, const uint32_t *v, int nb)
{
    RXDbMsg m = { .txid = txid, .kind = kind, .nb_args = RX_CLAMP(nb, 0, RX_DB_MAX_ARGS) };
    for (int i = 0; i < m.nb_args; i++)
        m.args[i] = (RXDbArg) { .type = RX_DB_NUM, .num = v[i] };
    rx_dbmsg_write(w, &m);
}

uint32_t rx_dbmsg_num(const RXDbMsg *m, int i)
{
    return i >= 0 && i < m->nb_args && m->args[i].type == RX_DB_NUM ? m->args[i].num : 0;
}

char *rx_dbmsg_str(const RXDbMsg *m, int i)
{
    if (i < 0 || i >= m->nb_args || m->args[i].type != RX_DB_STR)
        return NULL;
    return rx_link_utf16_to_utf8(m->args[i].p, m->args[i].len, true);
}

const uint8_t *rx_dbmsg_blob(const RXDbMsg *m, int i, uint32_t *len)
{
    *len = 0;
    if (i < 0 || i >= m->nb_args || m->args[i].type != RX_DB_BLOB || !m->args[i].p)
        return NULL;
    *len = m->args[i].len;
    return m->args[i].p;
}

/* --------------------------------------------------------------- items */

/* A text's length as the item carries it: UTF-16 bytes with the NUL
 * (rbxport's capture: "ARTIST" in U+FFFA..U+FFFB is 0x12). */
static uint32_t text_bytes(const char *s)
{
    return 2 * ((uint32_t)rx_link_utf16_units(s ? s : "") + 1);
}

void rx_dbmsg_item(RXWr *w, uint32_t txid, const RXDbItem *it, bool extended)
{
    const char *t1 = it->text ? it->text : "", *t2 = it->text2 ? it->text2 : "";
    const char *t3 = it->text3 ? it->text3 : "";
    RXDbMsg m = { .txid = txid, .kind = RX_DB_MENU_ITEM, .nb_args = extended ? 16 : 12 };
    const RXDbArg args[16] = {
        { .type = RX_DB_NUM, .num = it->a },
        { .type = RX_DB_NUM, .num = it->id },
        { .type = RX_DB_NUM, .num = text_bytes(t1) },
        { .type = RX_DB_STR, .str = t1 },
        { .type = RX_DB_NUM, .num = text_bytes(t2) },
        { .type = RX_DB_STR, .str = t2 },
        { .type = RX_DB_NUM, .num = it->type },
        { .type = RX_DB_NUM, .num = it->flags },
        { .type = RX_DB_NUM, .num = it->c },
        { .type = RX_DB_NUM, .num = it->d },
        { .type = RX_DB_NUM, .num = it->e },
        { .type = RX_DB_NUM, .num = it->key },
        { .type = RX_DB_NUM, .num = it->art },
        { .type = RX_DB_NUM, .num = text_bytes(t3) },
        { .type = RX_DB_STR, .str = t3 },
        { .type = RX_DB_NUM, .num = it->f },
    };
    memcpy(m.args, args, sizeof(args));
    rx_dbmsg_write(w, &m);
}

int rx_dbrow_parse(const RXDbMsg *m, RXDbRow *row)
{
    *row = (RXDbRow) { 0 };
    if (m->kind != RX_DB_MENU_ITEM || (m->nb_args != 12 && m->nb_args != 16))
        return RX_ERROR(EPROTO);
    static const uint8_t want[16] = {
        RX_DB_NUM, RX_DB_NUM, RX_DB_NUM, RX_DB_STR, RX_DB_NUM, RX_DB_STR, RX_DB_NUM, RX_DB_NUM,
        RX_DB_NUM, RX_DB_NUM, RX_DB_NUM, RX_DB_NUM, RX_DB_NUM, RX_DB_NUM, RX_DB_STR, RX_DB_NUM,
    };
    for (int i = 0; i < m->nb_args; i++)
        if (m->args[i].type != want[i])
            return RX_ERROR(EPROTO);
    *row = (RXDbRow) {
        .a     = m->args[0].num,
        .id    = m->args[1].num,
        .type  = m->args[6].num,
        .flags = m->args[7].num,
        .c     = m->args[8].num,
        .d     = m->args[9].num,
        .e     = m->args[10].num,
        .key   = m->args[11].num,
        .art   = m->nb_args > 12 ? m->args[12].num : 0,
        .f     = m->nb_args > 12 ? m->args[15].num : 0,
    };
    row->text = rx_dbmsg_str(m, 3);
    row->text2 = rx_dbmsg_str(m, 5);
    row->text3 = m->nb_args > 12 ? rx_dbmsg_str(m, 14) : rx_strdup("");
    if (!row->text || !row->text2 || !row->text3) {
        rx_dbrow_free(row);
        return RX_ERROR(ENOMEM);
    }
    return 0;
}

void rx_dbrow_free(RXDbRow *row)
{
    free(row->text);
    free(row->text2);
    free(row->text3);
    *row = (RXDbRow) { 0 };
}

void rx_dbrows_free(RXDbRow **rows, int nb)
{
    if (!*rows)
        return;
    for (int i = 0; i < nb; i++)
        rx_dbrow_free(&(*rows)[i]);
    free(*rows);
    *rows = NULL;
}
