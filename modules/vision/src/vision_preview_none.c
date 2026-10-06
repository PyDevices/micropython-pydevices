// esp-vision's preview streams an image to its own IDE over its USB
// multiplexer (EV-MUX), which our firmware doesn't carry. This stands in for
// platform/preview.c: image.flush() raises OSError("preview flush failed:
// ESP_ERR_NOT_SUPPORTED") rather than pretending to show anything.
#include "preview.h"

void esp_vision_preview_init0(void) {
}

void esp_vision_preview_deinit(void) {
}

esp_err_t esp_vision_preview_flush(const image_t *img) {
    (void)img;
    return ESP_ERR_NOT_SUPPORTED;
}
