"""Public API for compickle."""

__version__ = "1.1.0"

_USE_C = None
_serialize_fast = None
_deserialize_fast = None
_c_dedup_reset = None


def _ensure_loaded():
    global _USE_C, _serialize_fast, _deserialize_fast, _c_dedup_reset
    if _USE_C is not None:
        return
    try:
        from ._compickle import (serialize_fast as _sf,
                                  deserialize_fast as _df,
                                  dedup_reset as _dr)
        _serialize_fast   = _sf
        _deserialize_fast = _df
        _c_dedup_reset    = _dr
        _USE_C = True
    except ImportError:
        _USE_C = False


def dumps(obj) -> bytes:
    """Serializa obj y devuelve bytes."""
    _ensure_loaded()
    if _USE_C:
        # En v2 el DedupState es local por llamada en C,
        # así que serialize_fast no necesita dedup_reset() previo.
        return _serialize_fast(obj)
    from .compickle import dumps as _dumps
    return _dumps(obj)


def loads(data: bytes):
    """Deserializa desde bytes."""
    _ensure_loaded()
    if _USE_C:
        return _deserialize_fast(data)
    from .compickle import loads as _loads
    return _loads(data)


def dump(obj, path):
    """Serializa obj y guarda en path."""
    data = dumps(obj)
    with open(path, 'wb') as f:
        f.write(data)


def load(path):
    """Carga y deserializa desde path."""
    with open(path, 'rb') as f:
        data = f.read()
    return loads(data)


def dedup_reset():
    """
    Resetea caches internas de Python y C.

    En v2 esto NO es necesario llamarlo antes de cada dumps() —
    el módulo C gestiona su DedupState por llamada automáticamente.
    Úsalo para liberar memoria de los caches de source/exec.
    """
    _ensure_loaded()
    from .compickle import _reset_write_state
    _reset_write_state()
    if _USE_C and _c_dedup_reset is not None:
        _c_dedup_reset()


def backend() -> str:
    """Devuelve 'c' si el módulo compilado está activo, si no 'python'."""
    _ensure_loaded()
    return "c" if _USE_C else "python"


def __getattr__(name):
    if name == "_USE_C":
        _ensure_loaded()
        return _USE_C
    if name == "serialize_fast":
        _ensure_loaded()
        return _serialize_fast
    if name == "deserialize_fast":
        _ensure_loaded()
        return _deserialize_fast
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


__all__ = [
    "__version__",
    "dumps",
    "loads",
    "dump",
    "load",
    "dedup_reset",
    "backend",
]
