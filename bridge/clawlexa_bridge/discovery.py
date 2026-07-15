"""mDNS advertisement so the device finds the bridge by service type instead of
a hardcoded IP (SPEC §4).

The service (`_clawlexa._tcp`) is registered with the host's **OS mDNS responder**
— `dns-sd` on macOS, `avahi-publish-service` on Linux — because those answer LAN
multicast queries reliably. (An in-process Python responder like `zeroconf` does
*not* on macOS: the system `mDNSResponder` owns UDP 5353, so the service registers
locally but never answers the network — the device's query times out.)

Best-effort: if no system registrar is available we log and carry on, and the
device falls back to its Kconfig `BRIDGE_HOST`. Kept off the hot path so a
discovery hiccup can never take the link down.
"""
from __future__ import annotations

import contextlib
import logging
import shutil
import subprocess
import sys
from typing import Iterator, List, Optional

log = logging.getLogger("clawlexa.bridge")

SERVICE_TYPE = "_clawlexa._tcp"
SERVICE_INSTANCE = "clawlexa-bridge"


def registrar_command(port: int) -> Optional[List[str]]:
    """The command that advertises the service via the OS mDNS responder, or None
    if neither `dns-sd` (macOS) nor `avahi-publish-service` (Linux) is on PATH.
    The responder fills in this host's `.local` name + addresses automatically, so
    we never have to detect our own IP."""
    if sys.platform == "darwin" and shutil.which("dns-sd"):
        return ["dns-sd", "-R", SERVICE_INSTANCE, SERVICE_TYPE, ".", str(port)]
    avahi = shutil.which("avahi-publish-service")
    if avahi:
        return [avahi, SERVICE_INSTANCE, SERVICE_TYPE, str(port)]
    return None


@contextlib.contextmanager
def advertise(port: int) -> Iterator[Optional[subprocess.Popen]]:
    """Advertise the bridge over mDNS for the life of the context (best-effort).
    Yields the registrar subprocess, or None if advertising was skipped."""
    cmd = registrar_command(port)
    if cmd is None:
        log.info("mDNS: no system registrar (dns-sd/avahi) found — discovery off "
                 "(device uses its configured BRIDGE_HOST)")
        yield None
        return
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as e:  # never let advertising break serving
        log.warning("mDNS: could not start %s (%s) — device uses BRIDGE_HOST", cmd[0], e)
        yield None
        return
    log.info("mDNS: advertising %s %s on port %d (via %s)",
             SERVICE_INSTANCE, SERVICE_TYPE, port, cmd[0])
    try:
        yield proc
    finally:
        with contextlib.suppress(Exception):
            proc.terminate()
            proc.wait(timeout=2)
        log.info("mDNS: stopped advertising")
