// The same __revision__ for the modules only the ESP32-P4 has: h264 and rtsp.
// See vision_revision.c.
#include "py/obj.h"
#include "py/runtime.h"

extern void vision_module_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest);

MP_REGISTER_MODULE_DELEGATION(mp_module_h264, vision_module_attr);
MP_REGISTER_MODULE_DELEGATION(mp_module_rtsp, vision_module_attr);
