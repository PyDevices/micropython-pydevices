# Draft issue: VfsFat names are code page 437, not UTF-8, so non-ASCII filenames fail

A draft for micropython/micropython. Nothing here has been posted yet. The fix we carry meanwhile is
[patches/0038](../../patches/micropython/0038-micropython-lib-oofatfs-utf-8-filenames.patch).

---

**Title:** extmod/vfs_fat: filenames go through FatFs as code page 437, so a card written by a PC can't be listed or opened by its non-ASCII names

**Port, board, firmware**

Every port that uses `VfsFat` with long filenames. Seen on the esp32 port
(MicroPython v1.30.0-preview, an ESP32-P4 with an SD card formatted and
filled on Windows) and reproduced on the unix port with `VfsFat` on a RAM
block device. `lib/oofatfs/ffconf.h` hard-codes `FF_LFN_UNICODE 0`, and no
`MICROPY_FATFS_*` option reaches it.

**Reproduction**

Copy a file named `Ravel - Boléro.mid` onto a FAT card from a PC, then on
the board:

```python
>>> os.listdir(b"/sd")
[b'Ravel - Bol\x82ro.mid']                 # code page 437 bytes, not UTF-8
>>> os.listdir("/sd")
UnicodeError:
>>> open("/sd/Ravel - Boléro.mid", "rb")
OSError: [Errno 2] ENOENT
```

A name with a character code page 437 doesn't have, such as `Dvořák.mid`,
is listed only by its short name (`b'DVOR\xc1K~1.MID'`). The other
direction is wrong too: a file MicroPython creates as `Boléro.mid` appears
on the PC as `Bol├⌐ro.mid`, because its UTF-8 bytes were stored as two
code page 437 characters.

**Cause**

`FF_LFN_UNICODE 0` makes FatFs take and return names in the OEM code page
(`FF_CODE_PAGE`, 437 by default). MicroPython's `str` is UTF-8, and
`vfs_fat.c` passes `mp_obj_str_get_str()` to `f_open()` and builds names
from `FILINFO.fname` with `mp_obj_new_str()` unchanged, so every non-ASCII
name is mistranslated in both directions.

**The fix**

One line in `lib/oofatfs/ffconf.h`: `#define FF_LFN_UNICODE 2`, so the API
is UTF-8. It only takes effect where LFN is enabled, and `FF_CODE_PAGE`
still decides how short names are generated. Our patch makes it a
`MICROPY_FATFS_LFN_UNICODE` option defaulting to 2, so a port can keep the
old behaviour.

With it, all seven accented names on the card above list by their real
names, open, stat and read whole, and on unix a round trip through a RAM
disk works for Latin, CJK and an emoji (a surrogate pair on disk).

One limit: `FF_LFN_BUF` is 255 bytes, so a name whose UTF-8 form is longer
than that (FAT allows 255 UTF-16 units) is listed by its short name, as
names the code page couldn't hold were before.

**Cost**

No RAM: `FILINFO` and the LFN working buffer keep their sizes, and the
code page tables are already linked in with LFN. Code: +472 bytes on the
unix port (x86-64), and +400 bytes of firmware on an
ESP32-P4 (`ff.c` grows by 408 bytes of code; its 512-byte `LfnBuf` and its
read-only data are unchanged).
