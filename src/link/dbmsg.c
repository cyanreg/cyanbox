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

#include "cyanbox/common.h"

#include "dbmsg.h"

const uint8_t cb_db_greeting[CB_DB_GREETING_LEN] = { 0x11, 0x00, 0x00, 0x00, 0x01 };

/* Before the argument tags: 11 magic, 11 txid, 10 kind, 0f count, then
 * the tag list as a blob, 14 and its length (rbxport's measurement of
 * rekordbox 7.2.11, rbl-dbserver src/lib.rs HEADER_LEN). */
#define HEADER_LEN 20

/* The header's fixed bytes: the field tags and the magic. */
static const struct { uint8_t at, v; } fixed[] = {
    { 0, CB_DB_FIELD_NUM4 }, { 1, 0x87 }, { 2, 0x23 }, { 3, 0x49 }, { 4, 0xae },
    { 5, CB_DB_FIELD_NUM4 }, { 10, CB_DB_FIELD_NUM2 }, { 13, CB_DB_FIELD_NUM1 },
    { 15, CB_DB_FIELD_BLOB },
};

/* Whether the field tag `field` carries an argument declared as `type`. */
static bool field_fits(uint8_t type, uint8_t field)
{
    switch (type) {
    case CB_DB_NUM:  return field == CB_DB_FIELD_NUM1 || field == CB_DB_FIELD_NUM2 ||
                            field == CB_DB_FIELD_NUM4;
    case CB_DB_BLOB: return field == CB_DB_FIELD_BLOB;
    case CB_DB_STR:  return field == CB_DB_FIELD_STRING;
    }
    return false;
}

/* What to say when `need` bytes from `pos` are not all there: more, or
 * too long for the cap. */
static int short_of(size_t pos, uint64_t need, size_t max)
{
    return pos + need > max ? CB_ERROR(E2BIG) : 0;
}

int cb_dbmsg_parse(const uint8_t *buf, size_t len, size_t max, CBDbMsg *m)
{
    for (size_t i = 0; i < CB_ARRAY_ELEMS(fixed); i++)
        if (fixed[i].at < len && buf[fixed[i].at] != fixed[i].v)
            return CB_ERROR(EPROTO);
    if (len < HEADER_LEN)
        return short_of(0, HEADER_LEN, max);

    CBRd r = cb_rd(buf, len);
    m->txid = cb_rd_at_u32(&r, 6);
    m->kind = cb_rd_at_u16(&r, 11);
    int count = cb_rd_at_u8(&r, 14);
    uint32_t nb_tags = cb_rd_at_u32(&r, 16);
    /* The list is `count` long, or a fixed twelve zero-padded (dbmsg.h). */
    if (count > CB_DB_MAX_ARGS || nb_tags < (uint32_t)count || nb_tags > CB_DB_MAX_ARGS)
        return CB_ERROR(EPROTO);
    r.pos = HEADER_LEN;
    if (cb_rd_left(&r) < nb_tags)
        return short_of(r.pos, nb_tags, max);
    const uint8_t *tags = cb_rd_bytes(&r, nb_tags);
    for (int i = 0; i < count; i++)
        if (tags[i] != CB_DB_NUM && tags[i] != CB_DB_BLOB && tags[i] != CB_DB_STR)
            return CB_ERROR(EPROTO);

    m->nb_args = count;
    for (int i = 0; i < count; i++) {
        CBDbArg *a = &m->args[i];
        *a = (CBDbArg) { .type = tags[i] };
        /* A blob after a number 0 is declared and not sent. */
        if (tags[i] == CB_DB_BLOB && i > 0 && m->args[i - 1].type == CB_DB_NUM &&
            m->args[i - 1].num == 0)
            continue;
        if (!cb_rd_left(&r))
            return short_of(r.pos, 1, max);
        uint8_t field = cb_rd_u8(&r);
        if (!field_fits(tags[i], field))
            return CB_ERROR(EPROTO);
        /* A number's value, or a blob's or a string's 4-byte length. */
        size_t width = field == CB_DB_FIELD_NUM1 ? 1 : field == CB_DB_FIELD_NUM2 ? 2 : 4;
        if (cb_rd_left(&r) < width)
            return short_of(r.pos, width, max);
        if (field == CB_DB_FIELD_NUM1)
            a->num = cb_rd_u8(&r);
        else if (field == CB_DB_FIELD_NUM2)
            a->num = cb_rd_u16(&r);
        else
            a->num = cb_rd_u32(&r);
        if (field != CB_DB_FIELD_BLOB && field != CB_DB_FIELD_STRING)
            continue;
        /* A string's length counts UTF-16 units, its NUL included. */
        uint64_t bytes = field == CB_DB_FIELD_STRING ? 2 * (uint64_t)a->num : a->num;
        if (cb_rd_left(&r) < bytes)
            return short_of(r.pos, bytes, max);
        a->len = (uint32_t)bytes;
        a->p = cb_rd_bytes(&r, (size_t)bytes);
        a->num = 0;
    }
    if (r.err || r.pos > max)
        return r.pos > max ? CB_ERROR(E2BIG) : CB_ERROR(EPROTO);
    return (int)r.pos;
}

void cb_dbmsg_write(CBWr *w, const CBDbMsg *m)
{
    int count = CB_CLAMP(m->nb_args, 0, CB_DB_MAX_ARGS);
    cb_wr_u8(w, CB_DB_FIELD_NUM4);
    cb_wr_u32(w, CB_DB_MAGIC);
    cb_wr_u8(w, CB_DB_FIELD_NUM4);
    cb_wr_u32(w, m->txid);
    cb_wr_u8(w, CB_DB_FIELD_NUM2);
    cb_wr_u16(w, m->kind);
    cb_wr_u8(w, CB_DB_FIELD_NUM1);
    cb_wr_u8(w, (uint8_t)count);
    cb_wr_u8(w, CB_DB_FIELD_BLOB);
    cb_wr_u32(w, (uint32_t)count);
    /* An argument of no known type goes as a number: what is written
     * always parses. */
    for (int i = 0; i < count; i++) {
        uint8_t t = m->args[i].type;
        cb_wr_u8(w, t == CB_DB_STR || t == CB_DB_BLOB ? t : CB_DB_NUM);
    }
    for (int i = 0; i < count; i++) {
        const CBDbArg *a = &m->args[i];
        switch (a->type) {
        default:
            cb_wr_u8(w, CB_DB_FIELD_NUM4);
            cb_wr_u32(w, a->num);
            break;
        case CB_DB_BLOB:
            if (i > 0 && m->args[i - 1].type != CB_DB_STR && m->args[i - 1].type != CB_DB_BLOB &&
                m->args[i - 1].num == 0)
                break;
            cb_wr_u8(w, CB_DB_FIELD_BLOB);
            cb_wr_u32(w, a->len);
            cb_wr_bytes(w, a->p, a->len);
            break;
        case CB_DB_STR:
            cb_wr_u8(w, CB_DB_FIELD_STRING);
            if (a->str) {
                cb_wr_u32(w, (uint32_t)cb_link_utf16_units(a->str) + 1);
                cb_wr_utf16(w, a->str, true, true);
            } else {
                /* As parsed: the units with their NUL, as they came (an
                 * odd last byte cannot be one), so a message read and
                 * written again is the same. */
                uint32_t units = a->len / 2;
                cb_wr_u32(w, units);
                if (units)
                    cb_wr_bytes(w, a->p, (size_t)units * 2);
            }
            break;
        }
    }
}

void cb_dbmsg_numbers(CBWr *w, uint32_t txid, uint16_t kind, const uint32_t *v, int nb)
{
    CBDbMsg m = { .txid = txid, .kind = kind, .nb_args = CB_CLAMP(nb, 0, CB_DB_MAX_ARGS) };
    for (int i = 0; i < m.nb_args; i++)
        m.args[i] = (CBDbArg) { .type = CB_DB_NUM, .num = v[i] };
    cb_dbmsg_write(w, &m);
}

uint32_t cb_dbmsg_num(const CBDbMsg *m, int i)
{
    return i >= 0 && i < m->nb_args && m->args[i].type == CB_DB_NUM ? m->args[i].num : 0;
}

char *cb_dbmsg_str(const CBDbMsg *m, int i)
{
    if (i < 0 || i >= m->nb_args || m->args[i].type != CB_DB_STR)
        return NULL;
    return cb_link_utf16_to_utf8(m->args[i].p, m->args[i].len, true);
}

const uint8_t *cb_dbmsg_blob(const CBDbMsg *m, int i, uint32_t *len)
{
    *len = 0;
    if (i < 0 || i >= m->nb_args || m->args[i].type != CB_DB_BLOB || !m->args[i].p)
        return NULL;
    *len = m->args[i].len;
    return m->args[i].p;
}

/* --------------------------------------------------------------- items */

/* A text's length as the item carries it: UTF-16 bytes with the NUL
 * (rbxport's capture: "ARTIST" in U+FFFA..U+FFFB is 0x12). */
static uint32_t text_bytes(const char *s)
{
    return 2 * ((uint32_t)cb_link_utf16_units(s ? s : "") + 1);
}

void cb_dbmsg_item(CBWr *w, uint32_t txid, const CBDbItem *it, bool extended)
{
    cb_dbmsg_item_latin1(w, txid, it, extended, false);
}

void cb_dbmsg_item_latin1(CBWr *w, uint32_t txid, const CBDbItem *it, bool extended, bool latin1)
{
    char *f1 = latin1 ? cb_utf8_latin1(it->text) : NULL;
    char *f2 = latin1 ? cb_utf8_latin1(it->text2) : NULL;
    char *f3 = latin1 ? cb_utf8_latin1(it->text3) : NULL;
    const char *t1 = f1 ? f1 : it->text ? it->text : "", *t2 = f2 ? f2 : it->text2 ? it->text2 : "";
    const char *t3 = f3 ? f3 : it->text3 ? it->text3 : "";
    CBDbMsg m = { .txid = txid, .kind = CB_DB_MENU_ITEM, .nb_args = extended ? 16 : 12 };
    const CBDbArg args[16] = {
        { .type = CB_DB_NUM, .num = it->a },
        { .type = CB_DB_NUM, .num = it->id },
        { .type = CB_DB_NUM, .num = text_bytes(t1) },
        { .type = CB_DB_STR, .str = t1 },
        { .type = CB_DB_NUM, .num = text_bytes(t2) },
        { .type = CB_DB_STR, .str = t2 },
        { .type = CB_DB_NUM, .num = it->type },
        { .type = CB_DB_NUM, .num = it->flags },
        { .type = CB_DB_NUM, .num = it->c },
        { .type = CB_DB_NUM, .num = it->d },
        { .type = CB_DB_NUM, .num = it->e },
        { .type = CB_DB_NUM, .num = it->key },
        { .type = CB_DB_NUM, .num = it->art },
        { .type = CB_DB_NUM, .num = text_bytes(t3) },
        { .type = CB_DB_STR, .str = t3 },
        { .type = CB_DB_NUM, .num = it->f },
    };
    memcpy(m.args, args, sizeof(args));
    cb_dbmsg_write(w, &m);
    free(f1);
    free(f2);
    free(f3);
}

int cb_dbrow_parse(const CBDbMsg *m, CBDbRow *row)
{
    *row = (CBDbRow) { 0 };
    if (m->kind != CB_DB_MENU_ITEM || (m->nb_args != 12 && m->nb_args != 16))
        return CB_ERROR(EPROTO);
    static const uint8_t want[16] = {
        CB_DB_NUM, CB_DB_NUM, CB_DB_NUM, CB_DB_STR, CB_DB_NUM, CB_DB_STR, CB_DB_NUM, CB_DB_NUM,
        CB_DB_NUM, CB_DB_NUM, CB_DB_NUM, CB_DB_NUM, CB_DB_NUM, CB_DB_NUM, CB_DB_STR, CB_DB_NUM,
    };
    for (int i = 0; i < m->nb_args; i++)
        if (m->args[i].type != want[i])
            return CB_ERROR(EPROTO);
    *row = (CBDbRow) {
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
    row->text = cb_dbmsg_str(m, 3);
    row->text2 = cb_dbmsg_str(m, 5);
    row->text3 = m->nb_args > 12 ? cb_dbmsg_str(m, 14) : cb_strdup("");
    if (!row->text || !row->text2 || !row->text3) {
        cb_dbrow_free(row);
        return CB_ERROR(ENOMEM);
    }
    return 0;
}

void cb_dbrow_free(CBDbRow *row)
{
    free(row->text);
    free(row->text2);
    free(row->text3);
    *row = (CBDbRow) { 0 };
}

void cb_dbrows_free(CBDbRow **rows, int nb)
{
    if (!*rows)
        return;
    for (int i = 0; i < nb; i++)
        cb_dbrow_free(&(*rows)[i]);
    free(*rows);
    *rows = NULL;
}
