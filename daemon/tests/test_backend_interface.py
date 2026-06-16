import inspect
import pytest
from daemon.backends.base import Backend

def test_backend_is_abc_with_four_methods():
    assert inspect.isabstract(Backend)
    for m in ("read_token", "discover_target", "make_client", "single_instance"):
        assert hasattr(Backend, m), m

def test_cannot_instantiate_incomplete():
    class Partial(Backend):
        name = "x"
        def read_token(self): return None
    with pytest.raises(TypeError):
        Partial()
