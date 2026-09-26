import sys, time
from playwright.sync_api import sync_playwright

URL = "http://192.168.1.147/"
OUT = "/tmp/claude-1000/-home-brad-gh-pydevices/d03c77c5-9903-4055-80f3-88ec5e656c2f/scratchpad/cast/house_dashboard.png"

with sync_playwright() as p:
    b = p.chromium.launch()
    pg = b.new_page()
    pg.goto(URL, timeout=15000)
    # 1) websocket connects -> status "live"
    pg.wait_for_function("document.getElementById('status').textContent === 'live'", timeout=15000)
    print("PASS: dashboard connected (status=live)")
    # 2) room cards appear
    pg.wait_for_selector(".room[data-room='bath']", timeout=10000)
    rooms = pg.eval_on_selector_all(".room", "els => els.map(e => e.dataset.room)")
    print("PASS: rooms shown:", sorted(rooms))
    # 3) readings update live: sample the bath humidity a few times
    seen = []
    for _ in range(8):
        h = pg.get_attribute(".room[data-room='bath']", "data-humidity")
        seen.append(int(h))
        time.sleep(1.2)
    changed = len(set(seen)) > 1
    print(("PASS" if changed else "FAIL") + ": bath humidity updates live:", seen)
    # 4) alert appears when a threshold trips (bath goes damp)
    pg.wait_for_function("document.getElementById('alert').dataset.tripped === '1'", timeout=55000)
    txt = pg.text_content("#alert")
    print("PASS: alert tripped:", txt.strip()[:60])
    pg.screenshot(path=OUT)
    print("screenshot:", OUT)
    b.close()
print("DASH_TEST_DONE")
