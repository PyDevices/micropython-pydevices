# Direct browser runtime for PyDevices. The variant is external to the pinned
# upstream checkout and is selected with VARIANT_DIR by build_mp.py, which
# lets this variant's manifest.py be the build's FROZEN_MANIFEST.
JSFLAGS += -s ASYNCIFY
JSFLAGS += -sASYNCIFY_IMPORTS=pydevices_http_get,pydevices_sleep_ms
JSFLAGS += -sASYNCIFY_STACK_SIZE=131072
JSFLAGS += -s ALLOW_MEMORY_GROWTH

FROZEN_MANIFEST ?= $(VARIANT_DIR)/manifest.py
