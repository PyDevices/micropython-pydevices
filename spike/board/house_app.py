# house_app.py -- the Batch 2 smart-home appliance the P4 boots into.
#
# One program: the house panel on the P4's own display, the Batch 0 hub's
# dashboard served at http://<board ip>/ for a phone, and a SHOW-TV touch button
# that casts the panel (video + audio) to the 65". All on one asyncio loop, fed
# by the SimHouse until the FunHouse is plugged in.
#
# Set it as the board's /main.py to have it come up on boot.
import sys, asyncio, time

sys.path.insert(0, "/cast")     # roku_cast, pumpcast, castlive, micecast
sys.path.insert(0, "/lib")      # sensor_hub

from sensor_hub.hub import Hub
import house_panel as hp
from board_config import fb, touch_read


async def feeder(hub, sim):
    while True:
        sim.step()
        for line in sim.lines():
            hub.ingest(line, "sim")
        await asyncio.sleep(1)


async def painter(hub, audio, state):
    prev = set()
    while True:
        snap = hub.snapshot()
        items = []
        for name in hp.SimHouse.ROOMS:
            node = snap["nodes"].get(name)
            cur = hp.latest(node["series"]) if node else {}
            items += hp.alerts_for(name, cur)
        keys = set(k for k, _ in items)
        if keys - prev:
            audio.chime_now()
        prev = keys
        hp.draw_panel(snap, [t for _, t in items], state["casting"], "house")
        # SHOW-TV touch button toggles the cast (the RokuScreen is made on first use)
        try:
            pts = touch_read()
        except Exception:
            pts = None
        if pts and time.ticks_diff(time.ticks_ms(), state["touch"]) > 700:
            p = pts[0]
            px = p[0] if isinstance(p, (tuple, list)) else getattr(p, "x", 0)
            py = p[1] if isinstance(p, (tuple, list)) else getattr(p, "y", 0)
            if hp.in_rect(px, py, hp.CAST_BTN):
                state["touch"] = time.ticks_ms()
                tv = state["tv"]
                if tv is None:
                    from roku_cast import RokuScreen
                    tv = state["tv"] = RokuScreen(hp.TV, name="PyDevices House", log=lambda *a: None)
                    tv.on()
                if state["casting"]:
                    tv.stop_cast()
                    state["casting"] = False
                else:
                    state["casting"] = bool(tv.start_cast(fb, audio=audio.block, seconds=3600))
        # a full redraw holds the GIL ~0.2 s; yield generously so the hub server stays responsive
        await asyncio.sleep(0.8)


async def main(port=80):
    hub = Hub(name="house")
    audio = hp.HouseAudio(log=lambda *a: None)
    state = {"casting": False, "touch": -10000, "tv": None}
    server = await asyncio.start_server(hub.http, "0.0.0.0", port)
    asyncio.create_task(feeder(hub, hp.SimHouse()))
    asyncio.create_task(painter(hub, audio, state))
    try:
        import network
        ip = network.WLAN(network.STA_IF).ifconfig()[0]
    except Exception:
        ip = "?"
    print("house app: panel up, dashboard on http://%s:%d/" % (ip, port))
    while True:
        await asyncio.sleep(3600)


def run(port=80):
    asyncio.run(main(port))


if __name__ == "__main__":
    run()
