# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""The USB host port, its transfers and its isochronous streams.

`_usbhost` is the C half that `usb.core` and the `usb.host.*` packages are
built on. A program uses those packages and doesn't import this module; this
stub is for the people who write them.

Devices are named by the `dev_id` that `host_devices()` and `host_drain()`
report. Transfers block until they finish or time out, and raise `OSError`
with the errno: `ETIMEDOUT`, `EPIPE` for a stall, `ENODEV` once the device is
gone, `EACCES` for an endpoint whose interface isn't claimed. On a firmware
built without a USB host, calls that need one raise `OSError(EOPNOTSUPP)`.
"""

from typing import Any, Iterable

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git."""

# One attached device, as host_devices() and host_drain() report it:
# (dev_id, vid, pid, product, serial, classes, speed). `product` and `serial`
# are None here, filled in by the class drivers. `classes` is a set of
# "hid", "msc", "cdc", "midi", "uac", "uvc" and "other"; `speed` is "low",
# "full" or "high", or None.
_Device = tuple[int, int, int, str | None, str | None, set[str], str | None]

def host_start(classes: Iterable[str], *, full_speed: bool = False) -> set[str]:
    """Start the USB host, or leave a running one running.

    Args:
        classes: The device classes to start: any of "hid", "msc", "cdc",
            "midi", "uac", "uvc" and "other". Names this firmware can't drive
            are left out, and so are unknown ones. Takes effect when the host
            starts, not on a running host.
        full_speed: Hold the root port to full speed, so full- and low-speed
            devices work behind a hub on the ESP32-P4's high-speed host.

    Returns:
        The set of classes actually started.

    Raises:
        OSError: The host wouldn't start, or is wedged by a device that
            couldn't be released and needs a board reset.
    """

def host_stop() -> None:
    """Stop the host, close the classes and streams left open, and free the port."""

def host_devices() -> tuple[_Device, ...]:
    """Return the devices attached now, up to eight."""

def host_drain(limit: int) -> tuple[list[tuple[int, _Device]], bool]:
    """Take up to `limit` attach and detach events off the queue.

    Args:
        limit: The most events to take; the rest stay queued.

    Returns:
        `(events, lost)`. Each event is `(kind, device)` with kind 0 for an
        attach and 1 for a detach. `lost` is True when events were dropped
        since the last call.

    Raises:
        ValueError: `limit` is negative.
    """

def host_stats() -> tuple[bool, int, int, int, int, int] | None:
    """Return `(running, attaches, detaches, errors, lib_devices, lib_clients)`,
    or None on a firmware without a host."""

def host_port_cycle() -> int | None:
    """Power-cycle the root port to force a fresh connect, a software replug.

    Returns:
        The driver's result, or None on a firmware without a host.
    """

def host_intr_dump() -> None:
    """Print the host controller's interrupt counters, for debugging."""

def host_desc(dev_id: int) -> bytes:
    """Return a device's whole active configuration descriptor.

    It holds the interfaces, alternate settings, endpoints and class-specific
    descriptors, for the caller to parse.

    Raises:
        OSError: No such device, or it has no active configuration.
    """

def host_claim(dev_id: int, interface: int, alternate: int = 0) -> bool:
    """Claim an interface, at an alternate setting.

    Returns:
        True for a fresh claim, whose pipes start at DATA0; False when it was
        already claimed.
    """

def host_release(dev_id: int, interface: int) -> None:
    """Release a claimed interface."""

def host_ctrl(dev_id: int, setup: bytes | bytearray | memoryview, buf: Any, timeout_ms: int) -> int:
    """Run a control transfer.

    Args:
        dev_id: The device.
        setup: The 8-byte SETUP packet.
        buf: For an IN request, a writable buffer for the data; for an OUT
            request, the data to send; None for a request with no data stage.
            It must hold at least the packet's `wLength` bytes.
        timeout_ms: Milliseconds to wait.

    Returns:
        The number of bytes moved.

    Raises:
        ValueError: The setup isn't 8 bytes or the buffer is too short.
    """

def host_xfer(dev_id: int, endpoint: int, buf: Any, timeout_ms: int, raise_on_timeout: bool = True) -> int:
    """Run a bulk or interrupt transfer.

    An IN endpoint (address bit 7 set) reads into `buf`; an OUT endpoint
    sends it.

    Args:
        dev_id: The device.
        endpoint: The endpoint address.
        buf: A writable buffer for IN, or the data for OUT.
        timeout_ms: Milliseconds to wait; 0 waits for good.
        raise_on_timeout: When False, an IN read that times out returns 0 and
            leaves its transfer armed for the next read to collect.

    Returns:
        The number of bytes moved.
    """

def host_poll(dev_id: int, endpoint: int, buf: bytearray | memoryview) -> int:
    """Collect an IN read without waiting.

    The same kept transfer as `host_xfer` with `raise_on_timeout` False, so
    the two can share an endpoint.

    Returns:
        The bytes the transfer brought if it has finished, otherwise 0 at once.
    """

def host_stream(
    dev: int,
    ep: int,
    ring: Any,
    packet: int = 0,
    *,
    rate: int = 0,
    frame: int = 0,
    video: bool = False,
    max_frame: int = 0,
    downmix: bool = False,
) -> None:
    """Move an isochronous endpoint's packets to or from a ring, in C.

    The endpoint's interface must be claimed at its streaming alternate
    setting. Pass `None` as `ring` to stop the stream.

    Args:
        dev: The device.
        ep: The endpoint address; an IN endpoint fills the ring, an OUT
            endpoint drains it.
        ring: A `machine.USBDevice.Ring`, or for a capture stream an
            `audiopump.Ring`. None stops the stream.
        packet: The packet size in bytes.
        rate: The sample rate, to pace a PCM OUT stream.
        frame: The bytes in one frame of PCM, to pace a PCM OUT stream.
        video: Assemble UVC frames, each led in the ring by its 4-byte length.
        max_frame: The largest video frame in bytes.
        downmix: Average a stereo 16-bit IN stream to mono as its packets land.

    Raises:
        TypeError: `ring` is neither kind.
        ValueError: An `audiopump.Ring` was given for an OUT or video stream.
        OSError: The interface isn't claimed at its streaming setting, no
            stream is free, or this firmware's host has no isochronous streams.
    """

def host_stream_stats(dev_id: int, ep: int) -> tuple[int, int, int, int, int, int, int, int, int] | None:
    """Return `(packets, bytes, dropped, starved, errors, empty, frames, torn, big)`
    for a stream, or None when the endpoint has none."""

def host_stream_write(dev_id: int, ep: int, buf: bytes | bytearray | memoryview) -> int:
    """Queue PCM on an OUT stream.

    Returns:
        The bytes taken, in whole frames; 0 when the ring is full.
    """

def host_stream_space(dev_id: int, ep: int) -> int:
    """Return the bytes a `host_stream_write` can take now, in whole frames."""

def host_stream_c_sink(dev_id: int, ep: int, channels: int, bits: int) -> bytes:
    """Return the OUT stream's C sink structure by value, for another native
    module to copy and write to from its own task.

    It goes dead once the stream stops; a new stream issues a new one.
    """

def host_net_start(
    dev_id: int,
    interface: int,
    ep_in: int,
    ep_out: int,
    max_packet: int,
    mac: bytes | bytearray | memoryview,
    rndis: bool,
) -> None:
    """Bring up a USB network adapter as a network interface.

    Args:
        dev_id: The device.
        interface: The data interface.
        ep_in: The bulk IN endpoint address.
        ep_out: The bulk OUT endpoint address.
        max_packet: The endpoints' maximum packet size.
        mac: The adapter's 6-byte MAC address.
        rndis: True for an RNDIS adapter, which frames its packets.

    Raises:
        ValueError: `mac` isn't 6 bytes.
        OSError: This firmware has no network.
    """

def host_net_stop() -> None:
    """Take the USB network interface down."""

def host_net_info() -> tuple[bool, tuple[str, str, str, str], tuple[int, int, int, int, int]] | None:
    """Return `(up, (ip, netmask, gateway, dns), (rx, tx, rx_dropped, tx_dropped, rx_errors))`,
    or None when no interface is up."""

def host_net_default() -> None:
    """Make the USB network interface the default route."""
