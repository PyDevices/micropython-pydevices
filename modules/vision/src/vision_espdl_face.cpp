// SPDX-License-Identifier: MIT
// espdl.HumanFaceDetect: faces with five landmarks, from ESP-DL's two-stage
// human_face_detect models (MSR finds candidates, MNP refines each one and
// places the eyes, nose and mouth corners).
//
//   det = espdl.HumanFaceDetect("/models/human_face_detect_msr_s8_v1.espdl",
//                               "/models/human_face_detect_mnp_s8_v1.espdl")
//   for x, y, w, h, score, keypoints in det.detect(img):  # keypoints: 10 ints
//
// The stages follow ESP-DL's models/human_face_detect (MIT, 0.5.0, the release
// for the esp-dl this firmware pins): the same anchors, thresholds, BGR input
// and square crops for MNP. Both models are read from the filesystem, as the
// other espdl classes read theirs. The whole detection runs without the GIL.

#include <array>
#include <list>
#include <new>
#include <vector>

// esp-dl's headers use FreeRTOS without including it.
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "dl_detect_define.hpp"
#include "dl_detect_mnp_postprocessor.hpp"
#include "dl_detect_msr_postprocessor.hpp"
#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"
#include "esp_heap_caps.h"
#include "fbs_model.hpp"

extern "C" {
#include "extmod/vfs.h"
#include "py/mperrno.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py_image.h"
}

extern "C" const mp_obj_type_t espdl_human_face_detect_type;

namespace {

// One stage: a model loaded from a file, its preprocessor and postprocessor.
struct face_stage_t {
    uint8_t *data;
    dl::Model *model;
    dl::image::ImagePreprocessor *pre;
    dl::detect::DetectPostprocessor *post;
};

typedef struct {
    mp_obj_base_t base;
    face_stage_t msr, mnp;
} face_obj_t;

void face_stage_free(face_stage_t *st) {
    delete st->post;
    st->post = nullptr;
    delete st->pre;
    st->pre = nullptr;
    delete st->model;
    st->model = nullptr;
    if (st->data != nullptr) {
        heap_caps_free(st->data);
        st->data = nullptr;
    }
}

void face_free(face_obj_t *self) {
    face_stage_free(&self->mnp);
    face_stage_free(&self->msr);
}

// The model file in one 16-byte-aligned block (PSRAM first), as esp-dl wants
// a model it reads in place.
uint8_t *face_read_file(mp_obj_t path) {
    mp_obj_t args[2] = { path, MP_OBJ_NEW_QSTR(MP_QSTR_rb) };
    mp_obj_t file = mp_vfs_open(2, args, (mp_map_t *)&mp_const_empty_map);
    int err = 0;
    mp_off_t size = mp_stream_seek(file, 0, MP_SEEK_END, &err);
    if (size > 0) {
        mp_stream_seek(file, 0, MP_SEEK_SET, &err);
    }
    if (size <= 0 || err != 0) {
        mp_stream_close(file);
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("HumanFaceDetect: empty or unreadable model file"));
    }
    size_t aligned = ((size_t)size + 15u) & ~15u;
    uint8_t *data = (uint8_t *)heap_caps_aligned_alloc(16, aligned, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == nullptr) {
        data = (uint8_t *)heap_caps_aligned_alloc(16, aligned, MALLOC_CAP_DEFAULT);
    }
    if (data == nullptr) {
        mp_stream_close(file);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("HumanFaceDetect: no memory for the model"));
    }
    mp_uint_t got = mp_stream_read_exactly(file, data, (mp_uint_t)size, &err);
    mp_stream_close(file);
    if (err != 0 || got != (mp_uint_t)size) {
        heap_caps_free(data);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("HumanFaceDetect: short read of the model file"));
    }
    return data;
}

// Loads one stage's model; NULL on success, else why not. The postprocessor
// is made by the caller, since the two stages differ there.
const char *face_stage_load(face_stage_t *st) {
    st->model = new (std::nothrow) dl::Model((const char *)st->data, fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    if (st->model == nullptr) {
        return "no memory for the model";
    }
    if (st->model->get_fbs_model() == nullptr) {
        return "the model file did not load (is it an .espdl model for this chip?)";
    }
    st->model->minimize();
    // The models take BGR, as ESP-DL's own HumanFaceDetect feeds them.
    st->pre = new (std::nothrow) dl::image::ImagePreprocessor(st->model,
        std::array<float, 3> {0.0f, 0.0f, 0.0f}, std::array<float, 3> {1.0f, 1.0f, 1.0f}, true);
    return st->pre == nullptr ? "no memory for the preprocessor" : nullptr;
}

image_t *face_get_image(mp_obj_t img_in, dl::image::img_t *out) {
    if (!mp_obj_is_type(img_in, &py_image_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected image.Image"));
    }
    image_t *img = (image_t *)py_image_cobj(img_in);
    if (img == nullptr || img->pixels == nullptr || img->w <= 0 || img->h <= 0) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("invalid image"));
    }
    if (img->pixfmt != PIXFORMAT_RGB565) {
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("HumanFaceDetect takes an RGB565 image"));
    }
    out->data = img->pixels;
    out->width = (uint16_t)img->w;
    out->height = (uint16_t)img->h;
    out->pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
    return img;
}

face_obj_t *face_get(mp_obj_t self_in) {
    face_obj_t *self = (face_obj_t *)MP_OBJ_TO_PTR(self_in);
    if (self->msr.post == nullptr || self->mnp.post == nullptr) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("HumanFaceDetect object is deinitialized"));
    }
    return self;
}

// Both stages, as ESP-DL's MSRMNP::run does it: MSR over the whole picture,
// then MNP on a square around each candidate, then one NMS over MNP's faces.
std::list<dl::detect::result_t> &face_run(face_obj_t *self, const dl::image::img_t &img) {
    self->msr.pre->preprocess(img);
    self->msr.model->run();
    self->msr.post->clear_result();
    self->msr.post->postprocess();
    std::list<dl::detect::result_t> &candidates = self->msr.post->get_result(img.width, img.height);

    self->mnp.post->clear_result();
    for (dl::detect::result_t &c : candidates) {
        int cx = (c.box[0] + c.box[2]) >> 1;
        int cy = (c.box[1] + c.box[3]) >> 1;
        int side = DL_MAX(c.box[2] - c.box[0], c.box[3] - c.box[1]);
        c.box[0] = cx - (side >> 1);
        c.box[1] = cy - (side >> 1);
        c.box[2] = c.box[0] + side;
        c.box[3] = c.box[1] + side;
        c.limit_box(img.width, img.height);
        self->mnp.pre->preprocess(img, c.box);
        self->mnp.model->run();
        self->mnp.post->postprocess();
    }
    self->mnp.post->nms();
    return self->mnp.post->get_result(img.width, img.height);
}

float face_float_arg(mp_obj_t obj, float dflt) {
    return obj == MP_OBJ_NULL ? dflt : mp_obj_get_float_to_f(obj);
}

} // namespace

// HumanFaceDetect(msr_path, mnp_path, *, score=0.5, nms=0.5, mnp_score=0.5, mnp_nms=0.5)
static mp_obj_t face_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_msr, ARG_mnp, ARG_score, ARG_nms, ARG_mnp_score, ARG_mnp_nms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_msr_path, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mnp_path, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_nms, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mnp_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mnp_nms, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    float score = face_float_arg(args[ARG_score].u_obj, 0.5f);
    float nms = face_float_arg(args[ARG_nms].u_obj, 0.5f);
    float mnp_score = face_float_arg(args[ARG_mnp_score].u_obj, 0.5f);
    float mnp_nms = face_float_arg(args[ARG_mnp_nms].u_obj, 0.5f);

    face_obj_t *self = mp_obj_malloc_with_finaliser(face_obj_t, type);
    self->msr = face_stage_t{nullptr, nullptr, nullptr, nullptr};
    self->mnp = face_stage_t{nullptr, nullptr, nullptr, nullptr};

    self->msr.data = face_read_file(args[ARG_msr].u_obj);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        self->mnp.data = face_read_file(args[ARG_mnp].u_obj);
        nlr_pop();
    } else {
        face_free(self);
        nlr_jump(nlr.ret_val);
    }

    const char *why = face_stage_load(&self->msr);
    if (why == nullptr) {
        // ESP-DL's anchors for the MSR stage: two strides, two box sizes each.
        self->msr.post = new (std::nothrow) dl::detect::MSRPostprocessor(self->msr.model, self->msr.pre,
            score, nms, 10, {{8, 8, 9, 9, {{16, 16}, {32, 32}}}, {16, 16, 9, 9, {{64, 64}, {128, 128}}}});
        if (self->msr.post == nullptr) {
            why = "no memory for the postprocessor";
        }
    }
    if (why == nullptr) {
        why = face_stage_load(&self->mnp);
    }
    if (why == nullptr) {
        // MNP sees one 48x48 crop per candidate.
        self->mnp.post = new (std::nothrow) dl::detect::MNPPostprocessor(self->mnp.model, self->mnp.pre,
            mnp_score, mnp_nms, 10, {{1, 1, 0, 0, {{48, 48}}}});
        if (self->mnp.post == nullptr) {
            why = "no memory for the postprocessor";
        }
    }
    if (why != nullptr) {
        face_free(self);
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("HumanFaceDetect: %s"), why);
    }
    return MP_OBJ_FROM_PTR(self);
}

// detect(img) -> [(x, y, w, h, score, keypoints)], keypoints a tuple of ten
// ints, five x, y pairs in ESP-DL's order: left eye, left mouth corner, nose,
// right eye, right mouth corner.
static mp_obj_t face_detect(mp_obj_t self_in, mp_obj_t img_in) {
    face_obj_t *self = face_get(self_in);
    dl::image::img_t img;
    face_get_image(img_in, &img);
    // img_in stays referenced from this frame, so its pixels outlive the run.
    MP_THREAD_GIL_EXIT();
    std::list<dl::detect::result_t> &faces = face_run(self, img);
    MP_THREAD_GIL_ENTER();

    mp_obj_t out = mp_obj_new_list(0, nullptr);
    for (const dl::detect::result_t &r : faces) {
        if (r.box.size() < 4) {
            continue;
        }
        int w = r.box[2] - r.box[0] + 1;
        int h = r.box[3] - r.box[1] + 1;
        if (w <= 0 || h <= 0) {
            continue;
        }
        mp_obj_t kp[10];
        size_t nkp = r.keypoint.size() < 10 ? r.keypoint.size() : 10;
        for (size_t i = 0; i < nkp; i++) {
            kp[i] = mp_obj_new_int(r.keypoint[i]);
        }
        mp_obj_t item[6] = {
            mp_obj_new_int(r.box[0]),
            mp_obj_new_int(r.box[1]),
            mp_obj_new_int(w),
            mp_obj_new_int(h),
            mp_obj_new_float(r.score),
            mp_obj_new_tuple(nkp, kp),
        };
        mp_obj_list_append(out, mp_obj_new_tuple(6, item));
    }
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_2(face_detect_obj, face_detect);

// set_thresholds(*, score=, nms=, mnp_score=, mnp_nms=) -> self
static mp_obj_t face_set_thresholds(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_score, ARG_nms, ARG_mnp_score, ARG_mnp_nms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_nms, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mnp_score, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_mnp_nms, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    face_obj_t *self = face_get(args[ARG_self].u_obj);
    if (args[ARG_score].u_obj != MP_OBJ_NULL) {
        self->msr.post->set_score_thr(mp_obj_get_float_to_f(args[ARG_score].u_obj));
    }
    if (args[ARG_nms].u_obj != MP_OBJ_NULL) {
        self->msr.post->set_nms_thr(mp_obj_get_float_to_f(args[ARG_nms].u_obj));
    }
    if (args[ARG_mnp_score].u_obj != MP_OBJ_NULL) {
        self->mnp.post->set_score_thr(mp_obj_get_float_to_f(args[ARG_mnp_score].u_obj));
    }
    if (args[ARG_mnp_nms].u_obj != MP_OBJ_NULL) {
        self->mnp.post->set_nms_thr(mp_obj_get_float_to_f(args[ARG_mnp_nms].u_obj));
    }
    return args[ARG_self].u_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(face_set_thresholds_obj, 1, face_set_thresholds);

static mp_obj_t face_deinit(mp_obj_t self_in) {
    face_free((face_obj_t *)MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(face_deinit_obj, face_deinit);

static const mp_rom_map_elem_t face_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&face_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&face_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_detect), MP_ROM_PTR(&face_detect_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_thresholds), MP_ROM_PTR(&face_set_thresholds_obj) },
};
static MP_DEFINE_CONST_DICT(face_locals_dict, face_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espdl_human_face_detect_type,
    MP_QSTR_HumanFaceDetect,
    MP_TYPE_FLAG_NONE,
    make_new, reinterpret_cast<const void *>(face_make_new),
    locals_dict, &face_locals_dict
    );
