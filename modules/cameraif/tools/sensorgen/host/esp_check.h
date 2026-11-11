#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_ERROR(x, tag, ...) do { esp_err_t err_rc_ = (x); if (err_rc_ != ESP_OK) return err_rc_; } while (0)
#define ESP_RETURN_ON_FALSE(a, err_code, tag, ...) do { if (!(a)) return (err_code); } while (0)
#define ESP_GOTO_ON_ERROR(x, goto_tag, log_tag, ...) do { ret = (x); if (ret != ESP_OK) goto goto_tag; } while (0)
#define ESP_GOTO_ON_FALSE(a, err_code, goto_tag, log_tag, ...) do { if (!(a)) { ret = (err_code); goto goto_tag; } } while (0)
