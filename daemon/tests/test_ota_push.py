import hashlib

import pytest

from daemon import ota_push


def test_build_begin_layout():
    sha = bytes(range(32))
    frame = ota_push.build_begin(0x01020304, sha)
    assert frame[0] == 0x01                      # BEGIN opcode
    assert frame[1:5] == (0x01020304).to_bytes(4, "little")
    assert frame[5:] == sha
    assert len(frame) == 1 + 4 + 32


def test_build_begin_rejects_bad_digest():
    with pytest.raises(ValueError):
        ota_push.build_begin(10, b"too-short")


def test_build_data_prefixes_opcode():
    assert ota_push.build_data(b"\x03\x04") == b"\x02\x03\x04"


def test_chunk_image_reassembles():
    image = bytes(range(256)) * 5          # 1280 bytes
    chunks = ota_push.chunk_image(image, 100)
    assert all(len(c) <= 100 for c in chunks)
    assert b"".join(chunks) == image
    assert len(chunks) == (len(image) + 99) // 100


def test_chunk_image_rejects_zero_payload():
    with pytest.raises(ValueError):
        ota_push.chunk_image(b"x", 0)


@pytest.mark.parametrize("mtu,expected", [
    (0, 20),       # unknown -> floor (23-3-1 = 19, clamped to 20)
    (23, 20),      # default ATT MTU -> floor
    (185, 181),    # macOS cap
    (517, 513),    # BlueZ negotiated
])
def test_payload_size(mtu, expected):
    assert ota_push._payload_size(mtu) == expected


def test_full_frame_roundtrip():
    # A host builds BEGIN + DATA*; reconstruct on the "device" side and confirm
    # the image and digest survive framing.
    image = bytes((i * 7) & 0xFF for i in range(1000))
    digest = hashlib.sha256(image).digest()

    begin = ota_push.build_begin(len(image), digest)
    assert begin[0] == 0x01
    size = int.from_bytes(begin[1:5], "little")
    assert size == len(image)
    assert begin[5:] == digest

    payload = ota_push._payload_size(185)
    rebuilt = bytearray()
    for frame in (ota_push.build_data(c) for c in ota_push.chunk_image(image, payload)):
        assert frame[0] == 0x02
        rebuilt += frame[1:]
    assert bytes(rebuilt) == image
    assert hashlib.sha256(bytes(rebuilt)).digest() == digest


def test_terminal_frame_constants():
    assert ota_push.END_FRAME == b"\x03"
    assert ota_push.ABORT_FRAME == b"\x04"
