// vision.__revision__: which vision this firmware was built from, read as
// image.__revision__, espdl.__revision__, h264.__revision__ or
// rtsp.__revision__. The modules are esp-vision's, compiled as they are, so
// their globals tables can't carry the attribute; each module's attribute
// delegation answers for it instead. The build passes VISION_REVISION
// (git describe) as a compile definition; a build with no git says "unknown"
// rather than guessing.
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#ifndef VISION_REVISION
#define VISION_REVISION "unknown"
#endif
static const MP_DEFINE_STR_OBJ(vision_revision_obj, VISION_REVISION);

void vision_module_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    (void)self_in;
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR___revision__) {
        dest[0] = MP_OBJ_FROM_PTR(&vision_revision_obj);
    }
}

MP_REGISTER_MODULE_DELEGATION(image_module, vision_module_attr);
MP_REGISTER_MODULE_DELEGATION(espdl_module, vision_module_attr);
