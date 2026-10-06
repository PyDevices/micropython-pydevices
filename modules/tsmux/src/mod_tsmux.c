// tsmux for Python: MPEG-TS from H.264 access units, and HLS segments.
//
//   m = tsmux.Muxer()                  # lpcm=True adds castif's audio track
//   ts = m.tables() + m.video(au, pts, key=True)    # bytes, 188 per packet
//
//   hls = tsmux.Segmenter(segments=3, target=1000)  # ms per segment
//   hls.add(au, pts, key)              # each access unit from h264enc
//   hls.playlist()                     # str: an HLS media playlist
//   hls.segment(n)                     # bytes of seg<n>.ts, or None
//
// pts is in 90 kHz ticks. The segmenter cuts at the first keyframe at least
// `target` ms after the segment began, keeps the last `segments` finished
// ones, and starts each with the PAT and PMT, so any one plays alone. Every
// port: the muxing is tsmux_core.c, which castif also uses.

#include <string.h>

#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "tsmux_core.h"

static void tsmux_to_vstr(void *ctx, const uint8_t *pkt) {
    vstr_add_strn((vstr_t *)ctx, (const char *)pkt, 188);
}

// --- tsmux.Muxer ------------------------------------------------------------

typedef struct _tsmux_muxer_obj_t {
    mp_obj_base_t base;
    tsmux_t m;
    vstr_t *out;    // the call in progress collects into this
} tsmux_muxer_obj_t;

static void tsmux_muxer_collect(void *ctx, const uint8_t *pkt) {
    tsmux_muxer_obj_t *self = ctx;
    vstr_add_strn(self->out, (const char *)pkt, 188);
}

static mp_obj_t tsmux_muxer_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_lpcm };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_lpcm, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    tsmux_muxer_obj_t *self = mp_obj_malloc(tsmux_muxer_obj_t, type);
    tsmux_init(&self->m, args[ARG_lpcm].u_bool, tsmux_muxer_collect, self);
    self->out = NULL;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t tsmux_muxer_begin(tsmux_muxer_obj_t *self, vstr_t *vstr, size_t hint) {
    vstr_init(vstr, hint);
    self->out = vstr;
    return mp_const_none;
}

static mp_obj_t tsmux_muxer_end(tsmux_muxer_obj_t *self, vstr_t *vstr) {
    self->out = NULL;
    return mp_obj_new_bytes_from_vstr(vstr);
}

static mp_uint_t tsmux_get_pts(mp_obj_t o) {
    // a 33-bit PTS is carried modulo 2**32 here, as castif always has
    return (mp_uint_t)mp_obj_get_int_truncated(o);
}

static mp_obj_t tsmux_muxer_tables(mp_obj_t self_in) {
    tsmux_muxer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    vstr_t vstr;
    tsmux_muxer_begin(self, &vstr, 2 * 188);
    tsmux_tables(&self->m);
    return tsmux_muxer_end(self, &vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(tsmux_muxer_tables_obj, tsmux_muxer_tables);

// video(au, pts, key=False) -> bytes
static mp_obj_t tsmux_muxer_video(size_t n_args, const mp_obj_t *args) {
    tsmux_muxer_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_buffer_info_t au;
    mp_get_buffer_raise(args[1], &au, MP_BUFFER_READ);
    bool key = n_args > 3 && mp_obj_is_true(args[3]);
    vstr_t vstr;
    tsmux_muxer_begin(self, &vstr, (au.len / 176 + 2) * 188);
    tsmux_video(&self->m, au.buf, au.len, tsmux_get_pts(args[2]), key);
    return tsmux_muxer_end(self, &vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(tsmux_muxer_video_obj, 3, 4, tsmux_muxer_video);

// lpcm(pcm, pts) -> bytes: 48 kHz stereo s16, native order
static mp_obj_t tsmux_muxer_lpcm(mp_obj_t self_in, mp_obj_t pcm, mp_obj_t pts) {
    tsmux_muxer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->m.lpcm) {
        mp_raise_ValueError(MP_ERROR_TEXT("tsmux: made without lpcm=True"));
    }
    mp_buffer_info_t b;
    mp_get_buffer_raise(pcm, &b, MP_BUFFER_READ);
    if (b.len % 4 || b.len == 0 || b.len > 65000) {
        mp_raise_ValueError(MP_ERROR_TEXT("tsmux: LPCM is whole stereo frames, under 65000 bytes"));
    }
    vstr_t vstr;
    tsmux_muxer_begin(self, &vstr, (b.len / 184 + 2) * 188);
    tsmux_lpcm(&self->m, b.buf, b.len, tsmux_get_pts(pts));
    return tsmux_muxer_end(self, &vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_3(tsmux_muxer_lpcm_obj, tsmux_muxer_lpcm);

static mp_obj_t tsmux_muxer_pcr(mp_obj_t self_in, mp_obj_t pcr) {
    tsmux_muxer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    vstr_t vstr;
    tsmux_muxer_begin(self, &vstr, 188);
    tsmux_pcr(&self->m, tsmux_get_pts(pcr));
    return tsmux_muxer_end(self, &vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(tsmux_muxer_pcr_obj, tsmux_muxer_pcr);

static mp_obj_t tsmux_muxer_reset(mp_obj_t self_in) {
    tsmux_muxer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    tsmux_reset(&self->m);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(tsmux_muxer_reset_obj, tsmux_muxer_reset);

static const mp_rom_map_elem_t tsmux_muxer_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_tables), MP_ROM_PTR(&tsmux_muxer_tables_obj) },
    { MP_ROM_QSTR(MP_QSTR_video), MP_ROM_PTR(&tsmux_muxer_video_obj) },
    { MP_ROM_QSTR(MP_QSTR_lpcm), MP_ROM_PTR(&tsmux_muxer_lpcm_obj) },
    { MP_ROM_QSTR(MP_QSTR_pcr), MP_ROM_PTR(&tsmux_muxer_pcr_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&tsmux_muxer_reset_obj) },
};
static MP_DEFINE_CONST_DICT(tsmux_muxer_locals_dict, tsmux_muxer_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    tsmux_muxer_type, MP_QSTR_Muxer, MP_TYPE_FLAG_NONE,
    make_new, tsmux_muxer_make_new,
    locals_dict, &tsmux_muxer_locals_dict
    );

// --- tsmux.Segmenter ----------------------------------------------------------

#define TSMUX_SEG_MAX 16

typedef struct _tsmux_segmenter_obj_t {
    mp_obj_base_t base;
    tsmux_t m;
    vstr_t cur;                 // the segment being written
    bool open;                  // cur holds a segment begun at a keyframe
    uint32_t cur_start;         // its first PTS
    uint32_t next_seq;          // the number cur will have
    uint32_t target;            // segment length to aim for, 90 kHz ticks
    uint32_t target_s;          // #EXT-X-TARGETDURATION: fixed, only ever raised
    uint8_t keep;               // finished segments kept
    uint8_t count;              // finished segments held
    uint32_t first_seq;         // the oldest held segment's number
    uint32_t dur[TSMUX_SEG_MAX];          // their durations, 90 kHz ticks
    mp_obj_t seg[TSMUX_SEG_MAX];          // and their bytes, oldest first
} tsmux_segmenter_obj_t;

static mp_obj_t tsmux_segmenter_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_segments, ARG_target };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_segments, MP_ARG_INT, {.u_int = 3} },
        { MP_QSTR_target, MP_ARG_INT, {.u_int = 1000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    mp_int_t keep = args[ARG_segments].u_int, target = args[ARG_target].u_int;
    if (keep < 1 || keep > TSMUX_SEG_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("tsmux: segments is 1..16"));
    }
    if (target < 100 || target > 60000) {
        mp_raise_ValueError(MP_ERROR_TEXT("tsmux: target is 100..60000 ms"));
    }
    tsmux_segmenter_obj_t *self = mp_obj_malloc(tsmux_segmenter_obj_t, type);
    memset((char *)self + sizeof(mp_obj_base_t), 0, sizeof(*self) - sizeof(mp_obj_base_t));
    tsmux_init(&self->m, 0, tsmux_to_vstr, &self->cur);
    self->keep = (uint8_t)keep;
    self->target = (uint32_t)target * 90;
    // HLS forbids the target duration to change while a stream plays (players
    // drop a playlist whose value flips), and each segment's duration, rounded
    // to the nearest second, must not exceed it. Players reload the playlist
    // about once per target duration, so it is kept as low as that allows: the
    // target rounded, raised only if a segment ever rounds above it (one
    // second too high, and VLC played 3 s of every 6).
    self->target_s = ((uint32_t)target + 500) / 1000;
    if (self->target_s < 1) {
        self->target_s = 1;
    }
    return MP_OBJ_FROM_PTR(self);
}

static void tsmux_segmenter_finish(tsmux_segmenter_obj_t *self, uint32_t end_pts) {
    if (self->count == self->keep) {
        memmove(&self->seg[0], &self->seg[1], (self->keep - 1) * sizeof(mp_obj_t));
        memmove(&self->dur[0], &self->dur[1], (self->keep - 1) * sizeof(uint32_t));
        self->count--;
        self->first_seq++;
    }
    if (self->count == 0) {
        self->first_seq = self->next_seq;
    }
    self->dur[self->count] = end_pts - self->cur_start;
    uint32_t secs = (self->dur[self->count] + 45000) / 90000;
    if (secs > self->target_s) {
        self->target_s = secs;
    }
    self->seg[self->count] = mp_obj_new_bytes_from_vstr(&self->cur);
    self->count++;
    self->next_seq++;
    self->open = false;
}

// add(au, pts, key) -> True when a segment was finished by this frame
static mp_obj_t tsmux_segmenter_add(size_t n_args, const mp_obj_t *args) {
    tsmux_segmenter_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_buffer_info_t au;
    mp_get_buffer_raise(args[1], &au, MP_BUFFER_READ);
    uint32_t pts = (uint32_t)mp_obj_get_int_truncated(args[2]);
    bool key = mp_obj_is_true(args[3]);
    bool finished = false;
    if (key && self->open && (uint32_t)(pts - self->cur_start) >= self->target) {
        tsmux_segmenter_finish(self, pts);
        finished = true;
    }
    if (!self->open) {
        if (!key) {
            return mp_const_false;      // a segment starts at a keyframe
        }
        vstr_init(&self->cur, 64 * 1024);
        self->open = true;
        self->cur_start = pts;
    }
    if (key) {
        tsmux_tables(&self->m);
    }
    tsmux_video(&self->m, au.buf, au.len, pts, key);
    (void)n_args;
    return mp_obj_new_bool(finished);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(tsmux_segmenter_add_obj, 4, 4, tsmux_segmenter_add);

// playlist(prefix="seg") -> str: the finished segments as an HLS media playlist
static mp_obj_t tsmux_segmenter_playlist(size_t n_args, const mp_obj_t *args) {
    tsmux_segmenter_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    const char *prefix = n_args > 1 ? mp_obj_str_get_str(args[1]) : "seg";
    vstr_t vstr;
    mp_print_t print;
    vstr_init_print(&vstr, 256, &print);
    mp_printf(&print, "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:%u\n#EXT-X-MEDIA-SEQUENCE:%u\n",
        (unsigned)self->target_s, (unsigned)self->first_seq);
    for (int i = 0; i < self->count; i++) {
        uint32_t ms = self->dur[i] / 90;
        mp_printf(&print, "#EXTINF:%u.%03u,\n%s%u.ts\n", (unsigned)(ms / 1000), (unsigned)(ms % 1000),
            prefix, (unsigned)(self->first_seq + i));
    }
    return mp_obj_new_str_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(tsmux_segmenter_playlist_obj, 1, 2, tsmux_segmenter_playlist);

// segment(n) -> bytes of segment n, or None when it is not (or no longer) held
static mp_obj_t tsmux_segmenter_segment(mp_obj_t self_in, mp_obj_t n_in) {
    tsmux_segmenter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t n = mp_obj_get_int(n_in);
    mp_int_t i = n - (mp_int_t)self->first_seq;
    if (self->count == 0 || i < 0 || i >= self->count) {
        return mp_const_none;
    }
    return self->seg[i];
}
static MP_DEFINE_CONST_FUN_OBJ_2(tsmux_segmenter_segment_obj, tsmux_segmenter_segment);

static void tsmux_segmenter_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    tsmux_segmenter_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (attr == MP_QSTR_first) {
        // the oldest segment held, or None before the first is finished
        dest[0] = self->count ? mp_obj_new_int_from_uint(self->first_seq) : mp_const_none;
    } else if (attr == MP_QSTR_count) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(self->count);
    } else {
        dest[1] = MP_OBJ_SENTINEL;
    }
}

static const mp_rom_map_elem_t tsmux_segmenter_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_add), MP_ROM_PTR(&tsmux_segmenter_add_obj) },
    { MP_ROM_QSTR(MP_QSTR_playlist), MP_ROM_PTR(&tsmux_segmenter_playlist_obj) },
    { MP_ROM_QSTR(MP_QSTR_segment), MP_ROM_PTR(&tsmux_segmenter_segment_obj) },
};
static MP_DEFINE_CONST_DICT(tsmux_segmenter_locals_dict, tsmux_segmenter_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    tsmux_segmenter_type, MP_QSTR_Segmenter, MP_TYPE_FLAG_NONE,
    make_new, tsmux_segmenter_make_new,
    attr, tsmux_segmenter_attr,
    locals_dict, &tsmux_segmenter_locals_dict
    );

// --- module -------------------------------------------------------------------

static const mp_rom_map_elem_t tsmux_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_tsmux) },
    { MP_ROM_QSTR(MP_QSTR_Muxer), MP_ROM_PTR(&tsmux_muxer_type) },
    { MP_ROM_QSTR(MP_QSTR_Segmenter), MP_ROM_PTR(&tsmux_segmenter_type) },
};
static MP_DEFINE_CONST_DICT(tsmux_module_globals, tsmux_module_globals_table);

const mp_obj_module_t tsmux_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&tsmux_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_tsmux, tsmux_user_cmodule);
