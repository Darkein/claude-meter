"""Platform-backend interface. Each OS subclass returns a ready-to-use
BleakClient and OS-specific token/discovery/lock primitives. Core contains
zero `if platform` BLE branching — it just calls these four methods.
"""
from __future__ import annotations
from abc import ABC, abstractmethod
from typing import Any


class Backend(ABC):
    name: str = "base"

    @abstractmethod
    def read_token(self) -> str | None:
        """Return the Claude OAuth access token, or None if unavailable."""

    @abstractmethod
    async def discover_target(self, skip_addr: str | None = None) -> Any | None:
        """Return a connectable target (address str or BLEDevice), or None.
        skip_addr lets a just-failed handle be skipped for one cycle."""

    @abstractmethod
    def make_client(self, target: Any):
        """Build a BleakClient for target with this OS's required args.
        Owns all OS-specific BleakClient kwargs (address_type, caching, etc.)."""

    @abstractmethod
    def single_instance(self) -> object:
        """Acquire the process-wide single-instance lock. Return a handle to
        keep alive for the process lifetime, or call sys.exit(0) / return a
        no-op sentinel. Core never inspects the return value beyond holding it."""

    def on_target_failed(self, addr: str | None) -> str | None:
        """Hook after a failed session. Backends that cache an address drop it
        here (linux); macOS returns addr so core skips that handle next cycle;
        default no-op returns None (no skip). Keeps the cache/skip policy in the
        backend, not in core."""
        return None
