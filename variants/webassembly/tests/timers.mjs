// Every _wasm_bridge timer firing reaches its own callback, including the
// ones that fire while Python is inside an Asyncify sleep, whether or not
// anyone polls afterwards.
//
//   node variants/webassembly/tests/timers.mjs builds/webassembly/omnipython
import path from "node:path";

const dir = path.resolve(process.argv[2]);
const { loadMicroPython } = await import(path.join(dir, "micropython.mjs"));
const mp = await loadMicroPython({ stdout: (l) => console.log(l), stderr: (l) => console.error(l) });

// Periodic timers and a one-shot that all fire during sleeps. The poll after
// each sleep keeps only an id of its own, as a single-timer client would.
await mp.runPythonAsync(`
import _wasm_bridge, time
counts = {1: 0, 2: 0, 3: 0}
def counter(i):
    def cb():
        counts[i] += 1
    return cb
_wasm_bridge.timer_start(1, 10, True, counter(1))
_wasm_bridge.timer_start(2, 25, True, counter(2))
_wasm_bridge.timer_start(3, 30, False, counter(3))
t0 = time.ticks_ms()
for _ in range(30):
    _wasm_bridge.sleep_ms(10)
    while _wasm_bridge.timer_poll() is not None:
        pass
ms = time.ticks_diff(time.ticks_ms(), t0)
_wasm_bridge.timer_cancel(1)
_wasm_bridge.timer_cancel(2)
print("over", ms, "ms of sleeps:", counts)
assert counts[1] >= ms // 10 // 2, "the 10 ms timer lost its firings"
assert counts[2] >= ms // 25 // 2, "the 25 ms timer lost its firings"
assert counts[3] == 1, "the one-shot was not delivered once"
`);

// One-shots that fire during a sleep that nobody polls after, in a script
// that then ends: each is delivered once, and a later timer still runs.
await mp.runPythonAsync(`
late = {4: 0, 5: 0, 6: 0}
def later(i):
    def cb():
        late[i] += 1
    return cb
_wasm_bridge.timer_start(4, 5, False, later(4))
_wasm_bridge.timer_start(5, 10, False, later(5))
_wasm_bridge.sleep_ms(60)
_wasm_bridge.timer_start(6, 20, False, later(6))
`);
await new Promise((resolve) => setTimeout(resolve, 200));
await mp.runPythonAsync(`
print("one-shots after the script ended:", late)
assert late == {4: 1, 5: 1, 6: 1}, "a firing during the sleep was not delivered"
print("timers: ok")
`);
