// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic.PhoneBook (PBAP client) and btclassic.AudioGateway (HFP audio
// gateway): the phone's data, and the board as a phone. They wrap the
// phone book and gateway halves of btclassic.h and nothing else;
// mod_btclassic.c registers the types.
//
// The vCard and listing parsers here are plain C over the reply's bytes, so
// every backend gets the same records.

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "btclassic.h"

extern const mp_obj_type_t btc_bluetooth_type;

typedef struct {
    mp_obj_base_t base;
} phone_obj_t;

static mp_obj_t ph_done(int err) {
    switch (err) {
        case BTC_OK:
            return mp_const_none;
        case BTC_ERR_NOMEM:
            mp_raise_OSError(MP_ENOMEM);
        case BTC_ERR_UNSUPPORTED:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: not in this firmware"));
        case BTC_ERR_STATE:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: busy, or not connected"));
        case BTC_ERR_ARG:
            mp_raise_ValueError(NULL);
        default:
            if (err > 0) {
                // An OBEX response from the phone.
                if (err == 0xc1 || err == 0xc3) {
                    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the phone refused; allow access to contacts on the phone"));
                }
                if (err == 0xc4) {
                    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the phone has no such book or entry"));
                }
                mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the phone answered 0x%02x"), err);
            }
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the Bluetooth stack refused"));
    }
}

static void radio_arg(mp_obj_t bt) {
    if (!mp_obj_is_type(bt, &btc_bluetooth_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected btclassic.Bluetooth()"));
    }
    if (!btc_radio_active()) {
        ph_done(btc_radio_start());
    }
}

static const uint8_t *addr_or_none(mp_obj_t obj) {
    if (obj == mp_const_none) {
        return NULL;
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(obj, &buf, MP_BUFFER_READ);
    if (buf.len != 6) {
        mp_raise_ValueError(MP_ERROR_TEXT("an address is 6 bytes"));
    }
    return buf.buf;
}

static mp_obj_t maybe_int(int v) {
    return v < 0 ? mp_const_none : MP_OBJ_NEW_SMALL_INT(v);
}

static void put(mp_obj_t d, qstr key, mp_obj_t value) {
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(key), value);
}

static mp_obj_t exit_deinit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    mp_obj_t deinit[2];
    mp_load_method(args[0], MP_QSTR_deinit, deinit);
    return mp_call_method_n_kw(0, 0, deinit);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ph_exit_obj, 4, 4, exit_deinit);

// ---- text the phone sends ----

static bool is_utf8(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t c = s[i];
        size_t more = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 4;
        if (more == 4 || (more && i + more >= n)) {
            return false;
        }
        for (size_t k = 1; k <= more; k++) {
            if ((s[i + k] & 0xc0) != 0x80) {
                return false;
            }
        }
        i += more + 1;
    }
    return true;
}

// A string from bytes that should be UTF-8. Bytes that aren't are taken as
// Latin-1, the usual other charset in old vCards.
static mp_obj_t text_from(const char *s, size_t n) {
    if (is_utf8((const uint8_t *)s, n)) {
        return mp_obj_new_str(s, n);
    }
    vstr_t v;
    vstr_init(&v, n * 2);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) {
            vstr_add_byte(&v, c);
        } else {
            vstr_add_byte(&v, 0xc0 | (c >> 6));
            vstr_add_byte(&v, 0x80 | (c & 0x3f));
        }
    }
    return mp_obj_new_str_from_vstr(&v);
}

static bool word_is(const char *s, size_t n, const char *word) {
    size_t w = strlen(word);
    if (n != w) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') {
            c -= 32;
        }
        if (c != word[i]) {
            return false;
        }
    }
    return true;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// ---- vCard 2.1 and 3.0 ----
//
// A record per card: {"name": str, "numbers": [(number, kind)], "emails":
// [str]}, plus "org" when there is one, and "call" ("missed", "received",
// "dialed") and "time" ("20261108T101500") in call history.

typedef struct {
    // The property: group stripped, upper or lower case as sent.
    const char *name;
    size_t name_len;
    const char *params;     // ";TYPE=CELL;..." as sent, may be empty
    size_t params_len;
    vstr_t *value;          // decoded
} vprop_t;

// Does a parameter list hold `word`, bare (2.1: ";CELL") or as a value
// (";TYPE=CELL", ";TYPE=cell,voice")?
static bool has_param(const vprop_t *p, const char *word) {
    const char *s = p->params, *end = p->params + p->params_len;
    while (s < end) {
        // One token: from after ; = or , up to the next of them.
        while (s < end && (*s == ';' || *s == ',' || *s == '=')) {
            s++;
        }
        const char *t = s;
        while (s < end && *s != ';' && *s != ',' && *s != '=') {
            s++;
        }
        if (word_is(t, s - t, word)) {
            return true;
        }
    }
    return false;
}

// The first component of a structured value, up to an unescaped ';'.
static size_t first_component(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\') {
            i++;
        } else if (s[i] == ';') {
            return i;
        }
    }
    return n;
}

// Strip 3.0's backslash escapes in place.
static void unescape(vstr_t *v) {
    char *s = v->buf;
    size_t j = 0;
    for (size_t i = 0; i < v->len; i++) {
        if (s[i] == '\\' && i + 1 < v->len) {
            char c = s[++i];
            s[j++] = (c == 'n' || c == 'N') ? '\n' : c;
        } else {
            s[j++] = s[i];
        }
    }
    v->len = j;
}

static void card_prop(mp_obj_t card, const vprop_t *p, vstr_t *n_name) {
    const char *v = p->value->buf;
    size_t vn = p->value->len;
    if (word_is(p->name, p->name_len, "FN")) {
        unescape(p->value);
        put(card, MP_QSTR_name, text_from(p->value->buf, p->value->len));
    } else if (word_is(p->name, p->name_len, "N")) {
        // Family;Given;Middle;Prefix;Suffix: "Given Family", for a card with no FN.
        size_t fam = first_component(v, vn);
        const char *given = fam < vn ? v + fam + 1 : v + vn;
        size_t gn = first_component(given, v + vn - given);
        vstr_reset(n_name);
        vstr_add_strn(n_name, given, gn);
        if (gn && fam) {
            vstr_add_byte(n_name, ' ');
        }
        vstr_add_strn(n_name, v, fam);
    } else if (word_is(p->name, p->name_len, "TEL")) {
        static const char *const kinds[] = { "CELL", "MOBILE", "HOME", "WORK", "MAIN", "FAX", "PAGER", "OTHER" };
        const char *kind = "";
        for (size_t i = 0; i < MP_ARRAY_SIZE(kinds); i++) {
            if (has_param(p, kinds[i])) {
                kind = i == 1 ? "cell" : kinds[i];
                break;
            }
        }
        char lower[8];
        size_t kl = strlen(kind);
        for (size_t i = 0; i < kl; i++) {
            lower[i] = kind[i] >= 'A' && kind[i] <= 'Z' ? kind[i] + 32 : kind[i];
        }
        mp_obj_t pair[2] = { text_from(v, vn), mp_obj_new_str(lower, kl) };
        mp_obj_list_append(mp_obj_dict_get(card, MP_OBJ_NEW_QSTR(MP_QSTR_numbers)), mp_obj_new_tuple(2, pair));
    } else if (word_is(p->name, p->name_len, "EMAIL")) {
        unescape(p->value);
        mp_obj_list_append(mp_obj_dict_get(card, MP_OBJ_NEW_QSTR(MP_QSTR_emails)), text_from(p->value->buf, p->value->len));
    } else if (word_is(p->name, p->name_len, "ORG")) {
        p->value->len = first_component(v, vn);
        unescape(p->value);
        put(card, MP_QSTR_org, text_from(p->value->buf, p->value->len));
    } else if (word_is(p->name, p->name_len, "X-IRMC-CALL-DATETIME")) {
        const char *call = has_param(p, "MISSED") ? "missed"
            : has_param(p, "RECEIVED") ? "received"
            : has_param(p, "DIALED") ? "dialed" : NULL;
        put(card, MP_QSTR_call, call ? mp_obj_new_str(call, strlen(call)) : mp_const_none);
        put(card, MP_QSTR_time, text_from(v, vn));
    }
}

static mp_obj_t new_card(void) {
    mp_obj_t card = mp_obj_new_dict(4);
    put(card, MP_QSTR_name, MP_OBJ_NEW_QSTR(MP_QSTR_));
    put(card, MP_QSTR_numbers, mp_obj_new_list(0, NULL));
    put(card, MP_QSTR_emails, mp_obj_new_list(0, NULL));
    return card;
}

// Read one physical line from `*pos`: returns its start and sets `*len`
// (without the line break), or NULL at the end.
static const char *next_line(const char *data, size_t size, size_t *pos, size_t *len) {
    if (*pos >= size) {
        return NULL;
    }
    const char *s = data + *pos;
    const char *nl = memchr(s, '\n', size - *pos);
    size_t n = nl ? (size_t)(nl - s) : size - *pos;
    *pos += nl ? n + 1 : n;
    if (n && s[n - 1] == '\r') {
        n--;
    }
    *len = n;
    return s;
}

static mp_obj_t vcards_parse(const char *data, size_t size) {
    mp_obj_t cards = mp_obj_new_list(0, NULL);
    mp_obj_t card = MP_OBJ_NULL;
    vstr_t line, value, n_name;
    vstr_init(&line, 128);
    vstr_init(&value, 128);
    vstr_init(&n_name, 32);
    size_t pos = 0, len;
    const char *s = next_line(data, size, &pos, &len);
    while (s) {
        // A logical line: folded lines (3.0) start with a space or tab, and
        // a quoted-printable value (2.1) ends a line with '=' to go on.
        vstr_reset(&line);
        vstr_add_strn(&line, s, len);
        for (;;) {
            size_t save = pos, nlen;
            const char *t = next_line(data, size, &pos, &nlen);
            if (t && nlen && (t[0] == ' ' || t[0] == '\t')) {
                vstr_add_strn(&line, t + 1, nlen - 1);
                continue;
            }
            if (t && line.len && line.buf[line.len - 1] == '=' && memchr(line.buf, ':', line.len)) {
                // Only for quoted-printable: check its parameters.
                const char *colon = memchr(line.buf, ':', line.len);
                vprop_t probe = { .params = line.buf, .params_len = colon - line.buf };
                if (has_param(&probe, "QUOTED-PRINTABLE")) {
                    line.len--;
                    vstr_add_strn(&line, t, nlen);
                    continue;
                }
            }
            pos = save;
            break;
        }
        s = next_line(data, size, &pos, &len);

        const char *l = line.buf;
        const char *colon = memchr(l, ':', line.len);
        if (colon == NULL) {
            continue;
        }
        const char *semi = memchr(l, ';', colon - l);
        const char *name_end = semi ? semi : colon;
        // Strip a group: "item1.TEL".
        const char *name = l;
        for (const char *c = l; c < name_end; c++) {
            if (*c == '.') {
                name = c + 1;
            }
        }
        vprop_t p = {
            .name = name, .name_len = name_end - name,
            .params = name_end, .params_len = colon - name_end,
            .value = &value,
        };
        const char *v = colon + 1;
        size_t vn = line.buf + line.len - v;
        vstr_reset(&value);
        if (has_param(&p, "QUOTED-PRINTABLE")) {
            for (size_t i = 0; i < vn; i++) {
                int hi, lo;
                if (v[i] == '=' && i + 2 < vn
                    && (hi = hexval(v[i + 1])) >= 0 && (lo = hexval(v[i + 2])) >= 0) {
                    vstr_add_byte(&value, hi * 16 + lo);
                    i += 2;
                } else {
                    vstr_add_byte(&value, v[i]);
                }
            }
        } else {
            vstr_add_strn(&value, v, vn);
        }

        if (word_is(p.name, p.name_len, "BEGIN") && word_is(value.buf, value.len, "VCARD")) {
            card = new_card();
            vstr_reset(&n_name);
        } else if (word_is(p.name, p.name_len, "END") && word_is(value.buf, value.len, "VCARD")) {
            if (card != MP_OBJ_NULL) {
                mp_obj_t nm = mp_obj_dict_get(card, MP_OBJ_NEW_QSTR(MP_QSTR_name));
                if (!mp_obj_is_true(nm) && n_name.len) {
                    put(card, MP_QSTR_name, text_from(n_name.buf, n_name.len));
                }
                mp_obj_list_append(cards, card);
                card = MP_OBJ_NULL;
            }
        } else if (card != MP_OBJ_NULL) {
            card_prop(card, &p, &n_name);
        }
    }
    vstr_clear(&line);
    vstr_clear(&value);
    vstr_clear(&n_name);
    return cards;
}

// ---- the vCard listing: <card handle="1.vcf" name="Doe;John"/> ----

// The value of attribute `attr` inside one element, entities decoded.
static mp_obj_t xml_attr(const char *el, size_t n, const char *attr) {
    size_t an = strlen(attr);
    for (size_t i = 0; i + an + 2 <= n; i++) {
        if (memcmp(el + i, attr, an) == 0 && (i == 0 || el[i - 1] == ' ' || el[i - 1] == '\t' || el[i - 1] == '\n')) {
            size_t j = i + an;
            while (j < n && el[j] == ' ') {
                j++;
            }
            if (j >= n || el[j] != '=') {
                continue;
            }
            j++;
            while (j < n && el[j] == ' ') {
                j++;
            }
            if (j >= n || (el[j] != '"' && el[j] != '\'')) {
                continue;
            }
            char q = el[j++];
            vstr_t v;
            vstr_init(&v, 32);
            while (j < n && el[j] != q) {
                if (el[j] == '&') {
                    const char *semi = memchr(el + j, ';', n - j);
                    size_t en = semi ? (size_t)(semi - (el + j)) : 0;
                    const char *e = el + j + 1;
                    uint32_t cp = 0;
                    if (en == 4 && memcmp(e, "amp", 3) == 0) {
                        cp = '&';
                    } else if (en == 3 && memcmp(e, "lt", 2) == 0) {
                        cp = '<';
                    } else if (en == 3 && memcmp(e, "gt", 2) == 0) {
                        cp = '>';
                    } else if (en == 5 && memcmp(e, "quot", 4) == 0) {
                        cp = '"';
                    } else if (en == 5 && memcmp(e, "apos", 4) == 0) {
                        cp = '\'';
                    } else if (en > 2 && e[0] == '#') {
                        for (size_t k = 1; k < en - 1; k++) {
                            int d = e[1] == 'x' ? (k == 1 ? 0 : hexval(e[k])) : e[k] - '0';
                            if (k == 1 && e[1] == 'x') {
                                continue;
                            }
                            cp = cp * (e[1] == 'x' ? 16 : 10) + (d < 0 ? 0 : d);
                        }
                    }
                    if (cp) {
                        if (cp < 0x80) {
                            vstr_add_byte(&v, cp);
                        } else {
                            vstr_add_char(&v, cp);
                        }
                        j += en + 1;
                        continue;
                    }
                }
                vstr_add_byte(&v, el[j++]);
            }
            mp_obj_t s = text_from(v.buf, v.len);
            vstr_clear(&v);
            return s;
        }
    }
    return MP_OBJ_NEW_QSTR(MP_QSTR_);
}

static mp_obj_t listing_parse(const char *data, size_t size) {
    mp_obj_t out = mp_obj_new_list(0, NULL);
    const char *s = data, *end = data + size;
    while (s < end) {
        const char *lt = memchr(s, '<', end - s);
        if (lt == NULL || end - lt < 6) {
            break;
        }
        const char *gt = memchr(lt, '>', end - lt);
        if (gt == NULL) {
            break;
        }
        if (memcmp(lt + 1, "card", 4) == 0 && (lt[5] == ' ' || lt[5] == '\t' || lt[5] == '\n')) {
            mp_obj_t pair[2] = {
                xml_attr(lt + 5, gt - lt - 5, "handle"),
                xml_attr(lt + 5, gt - lt - 5, "name"),
            };
            mp_obj_list_append(out, mp_obj_new_tuple(2, pair));
        }
        s = gt + 1;
    }
    return out;
}

// =========================================================================
// PhoneBook: PBAP client
// =========================================================================

#define PB_PROPS (BTC_PB_PROP_VERSION | BTC_PB_PROP_FN | BTC_PB_PROP_N | BTC_PB_PROP_TEL \
    | BTC_PB_PROP_EMAIL | BTC_PB_PROP_ORG | BTC_PB_PROP_CALL_TIME)

static void pb_check(void) {
    if (!btc_pb_started()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the phone book is stopped"));
    }
}

// PhoneBook(bt, *, max_bytes=262144)
static mp_obj_t pb_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bt, ARG_max_bytes };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bt, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_max_bytes, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 262144} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    radio_arg(args[ARG_bt].u_obj);
    if (!btc_pb_started()) {
        mp_int_t max = args[ARG_max_bytes].u_int;
        if (max < 1024) {
            mp_raise_ValueError(MP_ERROR_TEXT("max_bytes must be 1024 or more"));
        }
        ph_done(btc_pb_start((size_t)max));
    }
    return MP_OBJ_FROM_PTR(mp_obj_malloc(phone_obj_t, type));
}

// Wait for the running request; raise on timeout or a refusal.
static void pb_wait(mp_int_t timeout_ms) {
    mp_uint_t start = mp_hal_ticks_ms();
    btc_pb_status_t st;
    for (;;) {
        btc_pb_status(&st);
        if (!st.busy) {
            break;
        }
        if ((mp_int_t)(mp_hal_ticks_ms() - start) >= timeout_ms) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
        mp_event_wait_ms(5);
    }
    ph_done(st.result);
}

// The reply as bytes, and the buffer let go.
static mp_obj_t pb_take(void) {
    btc_pb_status_t st;
    btc_pb_status(&st);
    vstr_t v;
    vstr_init_len(&v, st.length);
    v.len = btc_pb_data((uint8_t *)v.buf, st.length);
    btc_pb_release();
    return mp_obj_new_bytes_from_vstr(&v);
}

// "pb" -> "telecom/pb", or "SIM1/telecom/pb"
static void pb_folder(vstr_t *v, mp_obj_t book, bool sim) {
    vstr_init(v, 32);
    if (sim) {
        vstr_add_str(v, "SIM1/");
    }
    vstr_add_str(v, "telecom/");
    vstr_add_str(v, mp_obj_str_get_str(book));
}

static mp_obj_t pb_connect(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_addr, ARG_timeout };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_addr, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 30000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    pb_check();
    btc_pb_changes();
    ph_done(btc_pb_connect(addr_or_none(args[ARG_addr].u_obj)));
    mp_int_t timeout = args[ARG_timeout].u_int;
    if (timeout <= 0) {
        return mp_const_none;
    }
    // Up, or a connection change that left it down (a refusal).
    mp_uint_t start = mp_hal_ticks_ms();
    btc_pb_status_t st;
    for (;;) {
        btc_pb_status(&st);
        if (st.connected) {
            return mp_const_none;
        }
        if (btc_pb_changes() & BTC_CHANGED_CONNECTION) {
            ph_done(st.result ? st.result : BTC_ERR_FAIL);
        }
        if ((mp_int_t)(mp_hal_ticks_ms() - start) >= timeout) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
        mp_event_wait_ms(10);
    }
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pb_connect_obj, 1, pb_connect);

static mp_obj_t pb_disconnect(mp_obj_t self_in) {
    pb_check();
    return ph_done(btc_pb_disconnect());
}
static MP_DEFINE_CONST_FUN_OBJ_1(pb_disconnect_obj, pb_disconnect);

// The paging and filter arguments pull(), size() and entry() share.
enum { ARG_self, ARG_book, ARG_offset, ARG_count, ARG_raw, ARG_photos, ARG_sim, ARG_version, ARG_timeout };
static const mp_arg_t pb_pull_args[] = {
    { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_book, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_pb)} },
    { MP_QSTR_offset, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_count, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_raw, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    { MP_QSTR_photos, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    { MP_QSTR_sim, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    { MP_QSTR_version, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 21} },
    { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 30000} },
};

static uint8_t pb_format(mp_int_t version) {
    if (version != 21 && version != 30) {
        mp_raise_ValueError(MP_ERROR_TEXT("version must be 21 or 30"));
    }
    return version == 30 ? 1 : 0;
}

static uint16_t pb_count(mp_obj_t count) {
    if (count == mp_const_none) {
        return BTC_PB_ALL;
    }
    mp_int_t n = mp_obj_get_int(count);
    if (n < 0 || n > BTC_PB_ALL) {
        mp_raise_ValueError(MP_ERROR_TEXT("count must be 0 to 65535"));
    }
    return (uint16_t)n;
}

// pull(book="pb", *, offset=0, count=None, raw=False, photos=False,
//      sim=False, version=21, timeout=30000)
static mp_obj_t pb_pull(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mp_arg_val_t args[MP_ARRAY_SIZE(pb_pull_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(pb_pull_args), pb_pull_args, args);
    pb_check();
    vstr_t path;
    pb_folder(&path, args[ARG_book].u_obj, args[ARG_sim].u_bool);
    vstr_add_str(&path, ".vcf");
    uint64_t props = PB_PROPS | (args[ARG_photos].u_bool ? BTC_PB_PROP_PHOTO : 0);
    int err = btc_pb_pull(vstr_null_terminated_str(&path), (uint16_t)args[ARG_offset].u_int,
        pb_count(args[ARG_count].u_obj), pb_format(args[ARG_version].u_int), props);
    vstr_clear(&path);
    ph_done(err);
    pb_wait(args[ARG_timeout].u_int);
    mp_obj_t data = pb_take();
    if (args[ARG_raw].u_bool) {
        return data;
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data, &buf, MP_BUFFER_READ);
    return vcards_parse(buf.buf, buf.len);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pb_pull_obj, 1, pb_pull);

// size(book="pb", *, sim=False) -> entries in the book
static mp_obj_t pb_size(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mp_arg_val_t args[MP_ARRAY_SIZE(pb_pull_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(pb_pull_args), pb_pull_args, args);
    pb_check();
    vstr_t path;
    pb_folder(&path, args[ARG_book].u_obj, args[ARG_sim].u_bool);
    vstr_add_str(&path, ".vcf");
    int err = btc_pb_pull(vstr_null_terminated_str(&path), 0, 0, 0, 0);
    vstr_clear(&path);
    ph_done(err);
    pb_wait(args[ARG_timeout].u_int);
    btc_pb_release();
    btc_pb_status_t st;
    btc_pb_status(&st);
    return maybe_int(st.book_size);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pb_size_obj, 1, pb_size);

// list(book="pb", *, offset=0, count=None, search=None, by="name",
//      order="indexed", sim=False, timeout=30000) -> [(handle, name)]
static mp_obj_t pb_list(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_book, ARG_offset, ARG_count, ARG_search, ARG_by, ARG_order, ARG_sim, ARG_timeout };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_book, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_pb)} },
        { MP_QSTR_offset, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_count, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_search, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_by, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_name)} },
        { MP_QSTR_order, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_indexed)} },
        { MP_QSTR_sim, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 30000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    pb_check();
    qstr by = mp_obj_str_get_qstr(args[ARG_by].u_obj);
    qstr order = mp_obj_str_get_qstr(args[ARG_order].u_obj);
    if ((by != MP_QSTR_name && by != MP_QSTR_number) || (order != MP_QSTR_indexed && order != MP_QSTR_name)) {
        mp_raise_ValueError(MP_ERROR_TEXT("by is 'name' or 'number'; order is 'indexed' or 'name'"));
    }
    const char *search = args[ARG_search].u_obj == mp_const_none ? NULL : mp_obj_str_get_str(args[ARG_search].u_obj);
    vstr_t folder;
    pb_folder(&folder, args[ARG_book].u_obj, args[ARG_sim].u_bool);
    int err = btc_pb_list(vstr_null_terminated_str(&folder), (uint16_t)args[ARG_offset].u_int,
        pb_count(args[ARG_count].u_obj), order == MP_QSTR_name ? 1 : 0, search, by == MP_QSTR_number ? 1 : 0);
    vstr_clear(&folder);
    ph_done(err);
    pb_wait(args[ARG_timeout].u_int);
    mp_obj_t data = pb_take();
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data, &buf, MP_BUFFER_READ);
    return listing_parse(buf.buf, buf.len);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pb_list_obj, 1, pb_list);

// entry(book, handle, *, raw=False, photos=False, sim=False, version=21,
//       timeout=30000) -> record, or None
static mp_obj_t pb_entry(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_book, ARG_handle, ARG_raw, ARG_photos, ARG_sim, ARG_version, ARG_timeout };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_book, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_handle, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_raw, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_photos, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_sim, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_version, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 21} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 30000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    pb_check();
    vstr_t folder;
    pb_folder(&folder, args[ARG_book].u_obj, args[ARG_sim].u_bool);
    uint64_t props = PB_PROPS | (args[ARG_photos].u_bool ? BTC_PB_PROP_PHOTO : 0);
    int err = btc_pb_entry(vstr_null_terminated_str(&folder), mp_obj_str_get_str(args[ARG_handle].u_obj),
        pb_format(args[ARG_version].u_int), props);
    vstr_clear(&folder);
    ph_done(err);
    pb_wait(args[ARG_timeout].u_int);
    mp_obj_t data = pb_take();
    if (args[ARG_raw].u_bool) {
        return data;
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data, &buf, MP_BUFFER_READ);
    mp_obj_t cards = vcards_parse(buf.buf, buf.len);
    size_t n;
    mp_obj_t *items;
    mp_obj_list_get(cards, &n, &items);
    return n ? items[0] : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pb_entry_obj, 3, pb_entry);

// lookup(number, *, timeout=10000) -> the contact's name, or None: what a
// call screen shows for an incoming number. The phone does the matching.
static mp_obj_t pb_lookup(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_number, ARG_timeout };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_number, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 10000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    pb_check();
    const char *number = mp_obj_str_get_str(args[ARG_number].u_obj);
    if (!number[0]) {
        return mp_const_none;
    }
    ph_done(btc_pb_list("telecom/pb", 0, 1, 0, number, 1));
    pb_wait(args[ARG_timeout].u_int);
    mp_obj_t data = pb_take();
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data, &buf, MP_BUFFER_READ);
    mp_obj_t found = listing_parse(buf.buf, buf.len);
    size_t n;
    mp_obj_t *items;
    mp_obj_list_get(found, &n, &items);
    if (n == 0) {
        return mp_const_none;
    }
    mp_obj_t *pair;
    mp_obj_tuple_get(items[0], &n, &pair);
    // The listing's name is N, "Family;Given;...": ask for the card's FN.
    const char *handle = mp_obj_str_get_str(pair[0]);
    ph_done(btc_pb_entry("telecom/pb", handle, 0, BTC_PB_PROP_VERSION | BTC_PB_PROP_FN | BTC_PB_PROP_N));
    pb_wait(args[ARG_timeout].u_int);
    data = pb_take();
    mp_get_buffer_raise(data, &buf, MP_BUFFER_READ);
    mp_obj_t cards = vcards_parse(buf.buf, buf.len);
    mp_obj_list_get(cards, &n, &items);
    if (n) {
        mp_obj_t name = mp_obj_dict_get(items[0], MP_OBJ_NEW_QSTR(MP_QSTR_name));
        if (mp_obj_is_true(name)) {
            return name;
        }
    }
    return pair[1];
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pb_lookup_obj, 2, pb_lookup);

static mp_obj_t pb_changes(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btc_pb_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(pb_changes_obj, pb_changes);

static mp_obj_t pb_deinit(mp_obj_t self_in) {
    btc_pb_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(pb_deinit_obj, pb_deinit);

// PhoneBook.parse(data) -> records: vCards from anywhere, a file included.
static mp_obj_t pb_parse(mp_obj_t data) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data, &buf, MP_BUFFER_READ);
    return vcards_parse(buf.buf, buf.len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(pb_parse_fun_obj, pb_parse);
static MP_DEFINE_CONST_STATICMETHOD_OBJ(pb_parse_obj, MP_ROM_PTR(&pb_parse_fun_obj));

static void pb_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    btc_pb_status_t st;
    btc_pb_status(&st);
    switch (attr) {
        case MP_QSTR_connected:
            dest[0] = mp_obj_new_bool(st.connected);
            return;
        case MP_QSTR_peer:
            dest[0] = st.connected ? mp_obj_new_bytes(st.peer, 6) : mp_const_none;
            return;
        case MP_QSTR_repositories: {
            // What the phone keeps: "phone", "sim", "speed_dial", "favorites".
            static const qstr names[] = { MP_QSTR_phone, MP_QSTR_sim, MP_QSTR_speed_dial, MP_QSTR_favorites };
            mp_obj_t l = mp_obj_new_list(0, NULL);
            for (int i = 0; i < 4; i++) {
                if (st.repositories & (1 << i)) {
                    mp_obj_list_append(l, MP_OBJ_NEW_QSTR(names[i]));
                }
            }
            dest[0] = l;
            return;
        }
        case MP_QSTR_features:
            dest[0] = mp_obj_new_int_from_uint(st.features);
            return;
        case MP_QSTR_new_missed:
            dest[0] = maybe_int(st.new_missed);
            return;
        case MP_QSTR_truncated:
            dest[0] = mp_obj_new_bool(st.truncated);
            return;
        default:
            dest[1] = MP_OBJ_SENTINEL;
            return;
    }
}

static const mp_rom_map_elem_t pb_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&pb_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&pb_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_pull), MP_ROM_PTR(&pb_pull_obj) },
    { MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&pb_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_list), MP_ROM_PTR(&pb_list_obj) },
    { MP_ROM_QSTR(MP_QSTR_entry), MP_ROM_PTR(&pb_entry_obj) },
    { MP_ROM_QSTR(MP_QSTR_lookup), MP_ROM_PTR(&pb_lookup_obj) },
    { MP_ROM_QSTR(MP_QSTR_parse), MP_ROM_PTR(&pb_parse_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&pb_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&pb_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&ph_exit_obj) },
};
static MP_DEFINE_CONST_DICT(pb_locals, pb_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_phonebook_type,
    MP_QSTR_PhoneBook,
    MP_TYPE_FLAG_NONE,
    make_new, pb_make_new,
    attr, pb_attr,
    locals_dict, &pb_locals
    );

// =========================================================================
// AudioGateway: HFP audio gateway, the board as the phone
// =========================================================================

static uint8_t ag_channels = 1;

static void ag_check(void) {
    if (!btc_ag_started()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the audio gateway is stopped"));
    }
}

static mp_obj_t ag_text(btc_ag_text_t which) {
    char buf[128];
    size_t n = btc_ag_text(which, buf, sizeof(buf));
    return n ? text_from(buf, n) : mp_const_none;
}

static const char *ag_call_name(int v) {
    static const char *const names[] = { "idle", "incoming", "dialing", "alerting", "active", "held" };
    return v >= 0 && v < (int)MP_ARRAY_SIZE(names) ? names[v] : "idle";
}

// AudioGateway(bt, *, buffer_ms=400, channels=1)
static mp_obj_t ag_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bt, ARG_buffer_ms, ARG_channels };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bt, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_buffer_ms, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 400} },
        { MP_QSTR_channels, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    radio_arg(args[ARG_bt].u_obj);
    if (!btc_ag_started()) {
        mp_int_t ms = args[ARG_buffer_ms].u_int;
        mp_int_t channels = args[ARG_channels].u_int;
        if (ms < 50 || ms > 1000) {
            mp_raise_ValueError(MP_ERROR_TEXT("buffer_ms must be 50 to 1000"));
        }
        if (channels != 1 && channels != 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("channels must be 1 or 2"));
        }
        if (btc_hf_started()) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: HandsFree is running; one HFP role at a time"));
        }
        ph_done(btc_ag_start((uint32_t)ms));
        ag_channels = (uint8_t)channels;
    }
    return MP_OBJ_FROM_PTR(mp_obj_malloc(phone_obj_t, type));
}

// readinto(buf) -> frames that came from the headset's microphone
static mp_obj_t ag_readinto(mp_obj_t self_in, mp_obj_t buf_in) {
    ag_check();
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    size_t width = 2 * ag_channels;
    if (buf.len % width) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer must hold whole frames"));
    }
    return MP_OBJ_NEW_SMALL_INT(btc_ag_read((int16_t *)buf.buf, buf.len / width, ag_channels));
}
static MP_DEFINE_CONST_FUN_OBJ_2(ag_readinto_obj, ag_readinto);

// write(buf) -> bytes taken: 16-bit mono for the headset's speaker
static mp_obj_t ag_write(mp_obj_t self_in, mp_obj_t buf_in) {
    ag_check();
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_READ);
    return MP_OBJ_NEW_SMALL_INT(btc_ag_write((const int16_t *)buf.buf, buf.len / 2) * 2);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ag_write_obj, ag_write);

static mp_obj_t ag_changes(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btc_ag_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(ag_changes_obj, ag_changes);

static mp_obj_t ag_stats(mp_obj_t self_in) {
    btc_ring_stats_t st;
    btc_ag_stats(&st);
    btc_ag_status_t s;
    btc_ag_status(&s);
    mp_obj_t d = mp_obj_new_dict(10);
    put(d, MP_QSTR_buffered, mp_obj_new_int_from_uint(st.buffered));
    put(d, MP_QSTR_capacity, mp_obj_new_int_from_uint(st.capacity));
    put(d, MP_QSTR_received, mp_obj_new_int_from_uint(st.received));
    put(d, MP_QSTR_underruns, mp_obj_new_int_from_uint(st.underruns));
    put(d, MP_QSTR_overruns, mp_obj_new_int_from_uint(st.overruns));
    put(d, MP_QSTR_dropped, mp_obj_new_int_from_uint(st.dropped));
    put(d, MP_QSTR_repeated, mp_obj_new_int_from_uint(st.repeated));
    put(d, MP_QSTR_skipped, mp_obj_new_int_from_uint(st.skipped));
    put(d, MP_QSTR_sent, mp_obj_new_int_from_uint(s.sent));
    put(d, MP_QSTR_silent, mp_obj_new_int_from_uint(s.silent));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ag_stats_obj, ag_stats);

static mp_obj_t ag_connect(size_t n_args, const mp_obj_t *args) {
    ag_check();
    return ph_done(btc_ag_connect(addr_or_none(n_args > 1 ? args[1] : mp_const_none)));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ag_connect_obj, 1, 2, ag_connect);

#define AG_SIMPLE(fn, call) \
    static mp_obj_t fn(mp_obj_t self_in) { \
        ag_check(); \
        return ph_done(call()); \
    } \
    static MP_DEFINE_CONST_FUN_OBJ_1(fn##_obj, fn);

AG_SIMPLE(ag_disconnect, btc_ag_disconnect)
AG_SIMPLE(ag_connect_audio, btc_ag_connect_audio)
AG_SIMPLE(ag_disconnect_audio, btc_ag_disconnect_audio)
AG_SIMPLE(ag_alerting, btc_ag_alerting)
AG_SIMPLE(ag_answer, btc_ag_answer)
AG_SIMPLE(ag_hangup, btc_ag_hangup)

// ring(number=None): an incoming call, with the caller's number
static mp_obj_t ag_ring(size_t n_args, const mp_obj_t *args) {
    ag_check();
    const char *number = n_args > 1 && args[1] != mp_const_none ? mp_obj_str_get_str(args[1]) : "";
    return ph_done(btc_ag_ring(number));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ag_ring_obj, 1, 2, ag_ring);

// dial(number): the board calls out
static mp_obj_t ag_dial(mp_obj_t self_in, mp_obj_t number) {
    ag_check();
    return ph_done(btc_ag_dial(mp_obj_str_get_str(number)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(ag_dial_obj, ag_dial);

// dtmf() -> the digits the headset sent since the last call
static mp_obj_t ag_dtmf(mp_obj_t self_in) {
    char buf[32];
    size_t n = btc_ag_dtmf(buf, sizeof(buf));
    return mp_obj_new_str(buf, n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ag_dtmf_obj, ag_dtmf);

static mp_obj_t ag_deinit(mp_obj_t self_in) {
    btc_ag_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ag_deinit_obj, ag_deinit);

static int ag_level(mp_obj_t v, int hi) {
    mp_int_t n = mp_obj_get_int(v);
    if (n < 0 || n > hi) {
        mp_raise_ValueError(MP_ERROR_TEXT("value out of range"));
    }
    return (int)n;
}

static void ag_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL) {
        btc_ag_status_t st;
        btc_ag_status(&st);
        switch (attr) {
            case MP_QSTR_connected:
                dest[0] = mp_obj_new_bool(st.connected);
                return;
            case MP_QSTR_peer:
                dest[0] = st.connected ? mp_obj_new_bytes(st.peer, 6) : mp_const_none;
                return;
            case MP_QSTR_features:
                dest[0] = mp_obj_new_int_from_uint(st.peer_features);
                return;
            case MP_QSTR_service:
                dest[0] = mp_obj_new_bool(st.service > 0);
                return;
            case MP_QSTR_signal:
                dest[0] = maybe_int(st.signal);
                return;
            case MP_QSTR_roaming:
                dest[0] = mp_obj_new_bool(st.roaming > 0);
                return;
            case MP_QSTR_battery:
                dest[0] = maybe_int(st.battery);
                return;
            case MP_QSTR_operator:
                dest[0] = ag_text(BTC_AG_OPERATOR);
                return;
            case MP_QSTR_number:
                dest[0] = ag_text(BTC_AG_NUMBER);
                return;
            case MP_QSTR_call: {
                const char *c = ag_call_name(st.call);
                dest[0] = mp_obj_new_str(c, strlen(c));
                return;
            }
            case MP_QSTR_caller:
                dest[0] = ag_text(BTC_AG_CALLER);
                return;
            case MP_QSTR_dialed:
                dest[0] = ag_text(BTC_AG_DIALED);
                return;
            case MP_QSTR_in_band_ring:
                dest[0] = mp_obj_new_bool(st.in_band_ring);
                return;
            case MP_QSTR_voice_recognition:
                dest[0] = mp_obj_new_bool(st.voice_recognition);
                return;
            case MP_QSTR_nrec:
                dest[0] = mp_obj_new_bool(st.nrec);
                return;
            case MP_QSTR_speaker_gain:
                dest[0] = maybe_int(st.speaker_gain);
                return;
            case MP_QSTR_mic_gain:
                dest[0] = maybe_int(st.mic_gain);
                return;
            case MP_QSTR_audio:
                dest[0] = mp_obj_new_bool(st.audio);
                return;
            case MP_QSTR_sample_rate:
                dest[0] = mp_obj_new_int_from_uint(st.sample_rate);
                return;
            case MP_QSTR_wideband:
                dest[0] = mp_obj_new_bool(st.wideband);
                return;
            default:
                dest[1] = MP_OBJ_SENTINEL;
                return;
        }
    } else if (dest[1] != MP_OBJ_NULL) {
        ag_check();
        mp_obj_t v = dest[1];
        switch (attr) {
            case MP_QSTR_service:
                ph_done(btc_ag_set_indicators(mp_obj_is_true(v), BTC_UNKNOWN, BTC_UNKNOWN, BTC_UNKNOWN));
                break;
            case MP_QSTR_signal:
                ph_done(btc_ag_set_indicators(BTC_UNKNOWN, ag_level(v, 5), BTC_UNKNOWN, BTC_UNKNOWN));
                break;
            case MP_QSTR_roaming:
                ph_done(btc_ag_set_indicators(BTC_UNKNOWN, BTC_UNKNOWN, mp_obj_is_true(v), BTC_UNKNOWN));
                break;
            case MP_QSTR_battery:
                ph_done(btc_ag_set_indicators(BTC_UNKNOWN, BTC_UNKNOWN, BTC_UNKNOWN, ag_level(v, 5)));
                break;
            case MP_QSTR_operator:
                ph_done(btc_ag_set_text(BTC_AG_OPERATOR, v == mp_const_none ? "" : mp_obj_str_get_str(v)));
                break;
            case MP_QSTR_number:
                ph_done(btc_ag_set_text(BTC_AG_NUMBER, v == mp_const_none ? "" : mp_obj_str_get_str(v)));
                break;
            case MP_QSTR_in_band_ring:
                ph_done(btc_ag_set_in_band_ring(mp_obj_is_true(v)));
                break;
            case MP_QSTR_voice_recognition:
                ph_done(btc_ag_voice_recognition(mp_obj_is_true(v)));
                break;
            case MP_QSTR_speaker_gain:
                ph_done(btc_ag_set_speaker_gain(ag_level(v, 15)));
                break;
            case MP_QSTR_mic_gain:
                ph_done(btc_ag_set_mic_gain(ag_level(v, 15)));
                break;
            default:
                return;     // read only: AttributeError
        }
        dest[0] = MP_OBJ_NULL;
    }
}

static const mp_rom_map_elem_t ag_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&ag_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&ag_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&ag_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&ag_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&ag_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&ag_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect_audio), MP_ROM_PTR(&ag_connect_audio_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect_audio), MP_ROM_PTR(&ag_disconnect_audio_obj) },
    { MP_ROM_QSTR(MP_QSTR_ring), MP_ROM_PTR(&ag_ring_obj) },
    { MP_ROM_QSTR(MP_QSTR_dial), MP_ROM_PTR(&ag_dial_obj) },
    { MP_ROM_QSTR(MP_QSTR_alerting), MP_ROM_PTR(&ag_alerting_obj) },
    { MP_ROM_QSTR(MP_QSTR_answer), MP_ROM_PTR(&ag_answer_obj) },
    { MP_ROM_QSTR(MP_QSTR_hangup), MP_ROM_PTR(&ag_hangup_obj) },
    { MP_ROM_QSTR(MP_QSTR_dtmf), MP_ROM_PTR(&ag_dtmf_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&ag_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&ph_exit_obj) },
};
static MP_DEFINE_CONST_DICT(ag_locals, ag_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_gateway_type,
    MP_QSTR_AudioGateway,
    MP_TYPE_FLAG_NONE,
    make_new, ag_make_new,
    attr, ag_attr,
    locals_dict, &ag_locals
    );
