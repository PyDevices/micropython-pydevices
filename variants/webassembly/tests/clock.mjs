// time.ticks_ms() and time.ticks_us() keep counting forward when the host's
// wall clock is stepped back, and ticks_us has better than millisecond steps.
//
//   node variants/webassembly/tests/clock.mjs builds/webassembly/omnipython
import path from "node:path";

const realNow = Date.now.bind(Date);
let step = 0;
Date.now = () => realNow() + step;
globalThis.stepWallClock = (ms) => { step += ms; };

const dir = path.resolve(process.argv[2]);
const { loadMicroPython } = await import(path.join(dir, "micropython.mjs"));
const mp = await loadMicroPython({ stdout: (l) => console.log(l), stderr: (l) => console.error(l) });

await mp.runPythonAsync(`
import js, time, _wasm_bridge

t0, u0 = time.ticks_ms(), time.ticks_us()
js.stepWallClock(-5000)   # the host's clock steps back 5 s
_wasm_bridge.sleep_ms(50)
dms = time.ticks_diff(time.ticks_ms(), t0)
dus = time.ticks_diff(time.ticks_us(), u0)
print("after a 5 s step back and a 50 ms sleep: ticks_ms", dms, "ticks_us", dus)
assert 40 <= dms <= 1000, "ticks_ms followed the wall clock"
assert 40000 <= dus <= 1000000, "ticks_us followed the wall clock"

fine = [time.ticks_us() % 1000 for _ in range(2000)]
print("ticks_us readings off a whole millisecond:", sum(1 for x in fine if x), "of", len(fine))
assert any(fine), "ticks_us only moves in whole milliseconds"
print("clock: ok")
`);
