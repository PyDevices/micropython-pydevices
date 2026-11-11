#pragma once
#include <stdlib.h>
#define MALLOC_CAP_DEFAULT 0
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_INTERNAL 0
#define heap_caps_calloc(n, s, caps) calloc((n), (s))
#define heap_caps_malloc(s, caps) malloc(s)
