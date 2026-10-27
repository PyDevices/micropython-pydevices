# Build on Windows

A Linux or WSL machine cross-builds the same `micropython.exe` with the same
command, `./build_mp.py --port windows --variant pydevices --modules all`,
once the MinGW packages in [Install](../get-started/install.md#install-what-each-port-needs)
are installed; the SDL2 it links against is fetched. This page is for building
on Windows itself.

You can build the windows port natively, from PowerShell, with MSYS2's MinGW
toolchain. You need Python from python.org and Git for Windows; the rest is a
one-time install of MSYS2 and five of its packages:

```powershell
winget install --id MSYS2.MSYS2 -e
C:\msys64\usr\bin\bash.exe -lc "pacman -Syu --noconfirm"
C:\msys64\usr\bin\bash.exe -lc "pacman -S --needed --noconfirm make mingw-w64-x86_64-gcc autoconf automake libtool"
```

If the first `pacman` line says it updated MSYS2's core and must close, run it
once more. Then, in any PowerShell:

```powershell
git clone https://github.com/PyDevices/micropython-pydevices.git
cd micropython-pydevices
python build_mp.py --port windows --variant pydevices --modules all
.\builds\windows\pydevices\micropython.exe
```

That's all you type. `build_mp.py` finds MSYS2 in `C:\msys64` (set
`MSYS2_ROOT` if it's elsewhere), puts its tools first on the build's `PATH`,
and uses its `bash`. Left to `PATH`, `bash` from PowerShell is WSL's, which
would run the build inside Linux. Without MSYS2 it stops and prints the install
lines above.

`micropython.exe` needs only Windows' own DLLs, so you can run it from any
shell, MSYS2 or not.

## What differs from Linux

- Type `python build_mp.py`, not `./build_mp.py`: Windows doesn't read the
  `#!` line, and python.org's Python is `python` or `py`, never `python3`. If
  `python` opens the Microsoft Store, turn off the `python.exe` and
  `python3.exe` App Execution Aliases (Settings, Apps, Advanced app settings,
  App execution aliases), or type `py` instead.
- Use `--variant pydevices` for the Windows display and audio backends
  (`displaydev.windisplay`, `audiodev.win_audio`). They need `uwin32`, which
  only that variant freezes, so on `dev` or the default they fail to import.
- Only the windows port builds natively. Build esp32, rp2, webassembly and
  unix from WSL, as on Linux.
- The installer, `curl -fsSL https://pydevices.github.io/install.sh | sh`, runs
  in Git Bash (Start menu, Git Bash), not PowerShell. A plain `git clone`, as
  above, does the same job.
