import asyncio, json
from unittest.mock import AsyncMock, MagicMock, patch
from daemon import core

def test_connect_and_run_uses_backend_make_client():
    backend = MagicMock()
    client = MagicMock()
    client.connect = AsyncMock()
    client.disconnect = AsyncMock()
    client.is_connected = True
    client.read_gatt_char = AsyncMock(return_value=b"")
    client.start_notify = AsyncMock()
    client.write_gatt_char = AsyncMock()
    backend.make_client.return_value = client
    backend.read_token.return_value = "tok"
    stop = asyncio.Event(); stop.set()  # stop immediately after one pass
    with patch.object(core, "poll_api", new=AsyncMock(return_value={"ok": True, "s": 1})), \
         patch.object(core, "awatch", return_value=_empty_aiter()):
        asyncio.run(core.connect_and_run(backend, "AA:BB:CC:DD:EE:FF", stop))
    backend.make_client.assert_called_once_with("AA:BB:CC:DD:EE:FF")

async def _empty_aiter():
    if False:
        yield
