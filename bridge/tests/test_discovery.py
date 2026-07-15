"""Unit tests for mDNS advertisement (SPEC §4).

We test the registrar-command builder and the graceful "no registrar" path — not
a real register/browse round-trip (multicast mDNS is too environment-dependent
for a unit suite; the on-device connect is the real end-to-end check).
"""
from clawlexa_bridge import discovery
from clawlexa_bridge.discovery import SERVICE_INSTANCE, SERVICE_TYPE, advertise


def test_registrar_command_macos(monkeypatch):
    monkeypatch.setattr(discovery.sys, "platform", "darwin")
    monkeypatch.setattr(discovery.shutil, "which",
                        lambda name: "/usr/bin/dns-sd" if name == "dns-sd" else None)
    cmd = discovery.registrar_command(8765)
    assert cmd[:2] == ["dns-sd", "-R"]
    assert SERVICE_INSTANCE in cmd and SERVICE_TYPE in cmd and "8765" in cmd


def test_registrar_command_linux(monkeypatch):
    monkeypatch.setattr(discovery.sys, "platform", "linux")
    monkeypatch.setattr(discovery.shutil, "which",
                        lambda name: "/usr/bin/" + name if name == "avahi-publish-service" else None)
    cmd = discovery.registrar_command(8765)
    assert cmd[0].endswith("avahi-publish-service")
    assert SERVICE_INSTANCE in cmd and SERVICE_TYPE in cmd and "8765" in cmd


def test_registrar_command_none_when_no_tool(monkeypatch):
    monkeypatch.setattr(discovery.shutil, "which", lambda name: None)
    assert discovery.registrar_command(8765) is None


def test_advertise_without_registrar_is_graceful(monkeypatch):
    """No dns-sd/avahi -> advertise() yields None and never raises."""
    monkeypatch.setattr(discovery, "registrar_command", lambda port: None)
    with advertise(8765) as handle:
        assert handle is None
