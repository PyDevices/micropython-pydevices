# Draft issue: esp32 BLE reports the MTU exchange before the connection

A draft for Brad to post on micropython/micropython. Nothing here has been
posted. We carry no patch for this: PyDevices' `bledev` works around it in
Python (`bledev.mpble` keeps an early MTU and applies it on the connect), in
[PyDevices/pydevices#81](https://github.com/PyDevices/pydevices/pull/81).

---

**Title:** esp32: `_IRQ_MTU_EXCHANGED` can arrive before `_IRQ_CENTRAL_CONNECT`, and aioble then loses the MTU

**Port, board, firmware**

esp32 port, MicroPython v1.29.0 with ESP-IDF v5.5.4 and the IDF's NimBLE, on
ESP32-S3 boards: a Waveshare ESP32-S3-Touch-LCD-7 as the peripheral and a
LilyGo T-Embed as a second central. (Our builds carry a few local patches;
none of them touches Bluetooth.) The PC central is Windows 11 through
[bleak](https://github.com/hbldh/bleak) 3.0.2, and Windows exchanges the MTU
as soon as it connects.

**Reproduction**

On the board, a peripheral that logs the order of its IRQs:

```python
import bluetooth, time
_CONNECT, _DISCONNECT, _MTU = 1, 2, 21
ble = bluetooth.BLE()
ble.active(True)
ble.config(mtu=247)
ble.gatts_register_services(((bluetooth.UUID(0x181A), ((bluetooth.UUID(0x2A6E), 0x02),)),))
ADV = b"\x02\x01\x06" + bytes((9, 0x09)) + b"mtu-test"
seen = []
def irq(event, data):
    if event == _CONNECT:
        seen.append("connect")
        print(time.ticks_ms(), "CENTRAL_CONNECT", data[0])
    elif event == _MTU:
        seen.append("mtu")
        print(time.ticks_ms(), "MTU_EXCHANGED", data[0], data[1])
    elif event == _DISCONNECT:
        print(time.ticks_ms(), "CENTRAL_DISCONNECT", data[0], "order:", seen)
        seen.clear()
        ble.gap_advertise(100_000, ADV)
ble.irq(irq)
ble.gap_advertise(100_000, ADV)
```

On the PC, connect five times:

```python
import asyncio
from bleak import BleakClient, BleakScanner

async def main():
    device = await BleakScanner.find_device_by_name("mtu-test", timeout=10)
    for i in range(5):
        async with BleakClient(device) as client:
            await asyncio.sleep(1)
            print("bleak: connect", i, "mtu", client.mtu_size)
        await asyncio.sleep(5)  # Windows keeps an idle link a few seconds

asyncio.run(main())
```

The board prints this, five times out of five (2026-09-24):

```
169856 MTU_EXCHANGED 1 247
169915 CENTRAL_CONNECT 1
175795 CENTRAL_DISCONNECT 1 order: ['mtu', 'connect']
```

The MTU exchange is reported 59-60 ms before the connection it belongs to,
every time. The five-second pause matters: Windows keeps an idle link open
for a few seconds after the last client closes it, and a reconnect inside
that window reuses the link instead of making a new one, so the board sees
nothing.

With a second board as the central instead (MicroPython v1.29.0 on another
ESP32-S3, calling `gattc_exchange_mtu()` from inside its
`_IRQ_PERIPHERAL_CONNECT` handler), the order is normal, five times out of
five.

**What happens**

In the IDF's NimBLE (`components/bt/host/nimble/nimble/nimble/host/src/ble_gap.c`
in v5.5.4), `ble_gap_rx_conn_complete()` doesn't raise `BLE_GAP_EVENT_CONNECT`.
It inserts the connection into the host (`ble_hs_conn_insert()`, line 3394)
and, for a peripheral, asks the controller for the peer's version
(`ble_gap_rd_rem_ver_tx()`, line 3423). When that completes,
`ble_gap_rx_rd_rem_ver_info_complete()` reads the peer's features, and only
when those arrive does `ble_gap_rx_rd_rem_sup_feat_complete()` call
`ble_gap_event_connect_call()` (line 3512). That's two link-layer procedures,
several connection events. Meanwhile ATT is already live, so an MTU request
from the central is answered and `BLE_GAP_EVENT_MTU` goes out first.
`extmod/nimble/modbluetooth_nimble.c` passes both straight to Python in the
order it gets them (`commmon_gap_event_cb()`, line 414, and
`central_gap_event_cb()`, line 449).

The NimBLE in `lib/mynewt-nimble` (1.4, used by the other NimBLE ports)
raises the connect event in `ble_gap_rx_conn_complete()` itself, so this is
specific to the esp32 port.

A MicroPython central on the same port can't exchange the MTU early enough
to show it: its own connect IRQ is deferred the same way, until it has read
the peer's features and version, and by then the peripheral has reported the
connection. That's why two boards never show it and Windows always does.

**Why it matters**

aioble's `_device_irq` (`micropython-lib`,
`micropython/bluetooth/aioble/aioble/device.py`) looks the connection up in
`DeviceConnection._connected` and drops an MTU for a handle it doesn't know
yet. The link then runs at the negotiated MTU while the board believes 23, so
every notification the board sends carries 20 bytes. Against Windows that cut
our notification throughput from 49 to 19 KB/s.

**Possible fixes**

- In `modbluetooth_nimble.c`, hold an `BLE_GAP_EVENT_MTU` that arrives for a
  handle whose connect hasn't been reported yet, and deliver it right after
  the connect. That keeps the order the IRQ documentation implies on every
  port.
- Or, in aioble, keep an MTU for an unknown handle and apply it when the
  connection appears. That's what we do in our own code meanwhile.

Happy to open a PR for either, whichever you'd prefer.
