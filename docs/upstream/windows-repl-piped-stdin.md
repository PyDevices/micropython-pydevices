# Draft issue: windows `micropython -i` with piped stdin prints prompts forever

A draft for Brad to post on micropython/micropython, if he wants to. Nothing
here has been posted. The fix we carry meanwhile is
[patches/0013](../../patches/0013-micropython-windows-read-piped-stdin-in-the-repl.patch).

---

**Title:** windows: `micropython -i` with stdin from a pipe prints empty prompts in a tight loop

**Port, build**

windows port, MicroPython v1.29.0, built with mingw-w64 (the `dev` variant).
The code involved is the same for the MSVC build.

**Reproduction**

```
printf 'print(6*7)\r\n' | micropython.exe -i
```

It never prints `42`. It prints `>>> ` followed by a newline, over and over,
until it is killed (about 6 MB of prompts in five seconds). Without `-i` the
same pipe prints `42`, and CPython's `python.exe -i` reads the pipe fine.

**What happens**

`ports/unix/main.c:702`, which the windows port also builds, enters the REPL
when stdin is not a tty if `-i` or `MICROPYINSPECT` is given. The windows
`mp_hal_stdin_rx_chr` (`ports/windows/windows_mphal.c:189`) reads keys with
`ReadConsoleInput` (line 203), which only works on a console handle. On a
pipe it fails, and the function returns `CHAR_CTRL_C` (line 206, commented
"EOF, ctrl-D"). The REPL treats Ctrl-C as a cancelled line, prints a new
prompt and reads again, and the read fails again.

The unix port reads stdin with `read()` and returns Ctrl-D at end of input
(`ports/unix/unix_mphal.c:180`), so the same pipe works there.

**Suggested fix**

When `GetConsoleMode` fails on the stdin handle, read bytes with `ReadFile`
the way the unix port uses `read()`: CR LF or a lone LF is Enter, and end of
input returns Ctrl-D, which ends the REPL. A failed console read should
return Ctrl-D too, not Ctrl-C. We carry this as a patch and can open a PR
with it if that's welcome.
