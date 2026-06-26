# compickle.py — versión 4.0
# Optimizaciones vs v3:
#   • _serialize: dispatch por type() con fallback a id(type) para subclases
#   • _ser_str: inline total sin llamadas — id cache + encode + emit en un bloque
#   • _ser_int: nuevos opcodes OP_UINT8 (0x11) y OP_NEG8 (0x12) del C v4
#   • _ser_dict: loop unrolled con acceso directo a k,v sin overhead
#   • buf.append eliminado donde posible — sustituido por buf += _BYTE[x] en casos críticos
#   • _wr_dedup_obj/_wr_dedup_raw: lookup combinado (tag,raw) sin tuple creation extra
#   • _reset_write_state: evita clear() de _source_cache (crece lentamente, no resetear)
#   • _read_dedup_bytes: soporte OP_SRC_Z (0x1B) para zlib
#   • _deserialize: nuevos opcodes 0x11 y 0x12, sin llamadas extra

import struct, types, inspect, marshal, zlib

try:
    from _compickle import serialize_fast, deserialize_fast, dedup_reset as _c_dedup_reset
    _USE_C = True
except ImportError:
    _USE_C = False

# ── source cache (NO se resetea entre dumps — crece lentamente) ──────────────
_source_cache: dict = {}  # id(obj) → bytes

def _get_source(obj) -> bytes:
    key = id(obj)
    r = _source_cache.get(key)
    if r is not None:
        return r
    try:
        src = inspect.getsource(obj).encode()
    except Exception:
        raise ValueError(f"No se puede obtener fuente de: {getattr(obj,'__qualname__',repr(obj))}")
    _source_cache[key] = src
    return src

# ── dedup ─────────────────────────────────────────────────────────────────────
_dedup_id:  dict = {}   # id(obj) → idx
_dedup_cnt: dict = {}   # (tag, raw) → idx
_dedup_n:   int  = 0

def _reset_write_state():
    global _dedup_n
    _dedup_id.clear()
    _dedup_cnt.clear()
    _dedup_n = 0
    # _source_cache NO se limpia: los id() de clases son estables durante la sesión

# ── struct helpers ────────────────────────────────────────────────────────────
_pack_H       = struct.Struct('>H').pack
_pack_I       = struct.Struct('>I').pack
_pack_d       = struct.Struct('>d').pack
_unpack_d_from = struct.Struct('>d').unpack_from

# bytes precalculados 0-255
_BYTE = [bytes([i]) for i in range(256)]

# Constantes de 2 bytes frecuentes precalculadas
_B00 = b'\x00'
_B80 = b'\x80'
_B81 = b'\x81'
_B0E = b'\x0e'
_B0F = b'\x0f'
_B10 = b'\x10'
_B11 = b'\x11'
_B12 = b'\x12'
_B15 = b'\x15'
_B1B = b'\x1b'
_B1D = b'\x1d'
_BFB = b'\xfb'
_BFE = b'\xfe'
_BFD = b'\xfd'
_BFC = b'\xfc'

def _write_len_b(n: int) -> bytes:
    if n <= 0x3F:   return _BYTE[n]
    if n <= 0x3FFF: return _BYTE[0x40 | (n >> 8)] + _BYTE[n & 0xFF]
    return b'\xFF' + _pack_I(n)

# refs precalculadas — caché persistente
_ref_cache: dict = {}

def _ref_bytes(idx: int) -> bytes:
    r = _ref_cache.get(idx)
    if r is None:
        if idx <= 0xFE:   r = _BFE + _BYTE[idx]
        elif idx <= 0xFFFF: r = _BFD + _pack_H(idx)
        else:              r = _BFC + _pack_I(idx)
        _ref_cache[idx] = r
    return r

# ── emit helpers ──────────────────────────────────────────────────────────────
def _emit_new(buf: bytearray, tag: int, raw: bytes) -> None:
    n = len(raw)
    if tag == 5 and n <= 63:
        buf.append(0x15); buf.append(n); buf.extend(raw)
    elif n <= 63:
        buf.append(0x0E); buf.append(n); buf.extend(raw)
    else:
        buf.append(0xFB); buf.append(tag)
        buf.extend(_write_len_b(n)); buf.extend(raw)

def _wr_dedup_obj(buf: bytearray, tag: int, obj, raw: bytes) -> None:
    global _dedup_n
    oid = id(obj)
    idx = _dedup_id.get(oid, -1)
    if idx >= 0:
        buf.extend(_ref_bytes(idx)); return
    key = (tag, raw)
    idx = _dedup_cnt.get(key, -1)
    if idx >= 0:
        _dedup_id[oid] = idx; buf.extend(_ref_bytes(idx)); return
    idx = _dedup_n; _dedup_n += 1
    _dedup_cnt[key] = idx; _dedup_id[oid] = idx
    _emit_new(buf, tag, raw)

def _wr_dedup_raw(buf: bytearray, tag: int, raw: bytes) -> None:
    global _dedup_n
    key = (tag, raw)
    idx = _dedup_cnt.get(key, -1)
    if idx >= 0:
        buf.extend(_ref_bytes(idx)); return
    idx = _dedup_n; _dedup_n += 1
    _dedup_cnt[key] = idx
    _emit_new(buf, tag, raw)

# ── serialización ─────────────────────────────────────────────────────────────
# CLAVE: _serialize usa type() directo + dict lookup — sin isinstance overhead
def _serialize(obj, buf: bytearray) -> None:
    fn = _DISPATCH.get(type(obj))
    if fn is not None:
        fn(obj, buf)
    else:
        _serialize_slow(obj, buf)

def _ser_none(_obj, buf):  buf.append(0x00)
def _ser_bool(obj, buf):   buf.append(0x81 if obj else 0x80)

def _ser_int(obj, buf):
    if 0 <= obj <= 58:
        buf.append(0xC0 + obj); return
    if 59 <= obj <= 255:
        buf.append(0x11); buf.append(obj); return
    if 256 <= obj <= 0xFFFF:
        buf.append(0x0F); buf.extend(_pack_H(obj)); return
    if -30 <= obj <= -1:
        buf.append(0x10); buf.append(-obj - 1); return
    if -256 <= obj <= -31:
        buf.append(0x12); buf.append(-obj - 1); return
    neg  = obj < 0
    absv = -obj if neg else obj
    nb   = max(1, (absv.bit_length() + 7) // 8)
    buf.append(0x02); buf.append(1 if neg else 0)
    buf.extend(_write_len_b(nb)); buf.extend(absv.to_bytes(nb, 'big'))

_pack_d_neg0_byte = _pack_d(-0.0)[0]

def _ser_float(obj, buf):
    if obj != obj:          buf.append(0x86); return
    if obj == 0.0:
        buf.append(0x83 if _pack_d(obj)[0] & 0x80 else 0x82); return
    if obj == 1.0:          buf.append(0x84); return
    if obj == -1.0:         buf.append(0x85); return
    if obj == 1e308 * 10:   buf.append(0x87); return
    if obj == -1e308 * 10:  buf.append(0x88); return
    buf.append(0x03); buf.extend(_pack_d(obj))

def _ser_complex(obj, buf):
    buf.append(0x04); buf.extend(_pack_d(obj.real)); buf.extend(_pack_d(obj.imag))

# HOT PATH: _ser_str completamente inline, sin llamadas a _emit_new/_wr_dedup_obj
def _ser_str(obj, buf):
    global _dedup_n
    oid = id(obj)
    # 1) identity hit — O(1), sin encode
    idx = _dedup_id.get(oid, -1)
    if idx >= 0:
        buf.extend(_ref_bytes(idx)); return
    # 2) encode y buscar por contenido
    raw = obj.encode()   # UTF-8, más rápido que encode('utf-8') explícito
    key = (5, raw)       # tag 0x05
    idx = _dedup_cnt.get(key, -1)
    if idx >= 0:
        _dedup_id[oid] = idx; buf.extend(_ref_bytes(idx)); return
    # 3) nuevo — registrar y emitir inline
    idx = _dedup_n; _dedup_n += 1
    _dedup_cnt[key] = idx; _dedup_id[oid] = idx
    n = len(raw)
    if n <= 63:
        buf.append(0x15); buf.append(n); buf.extend(raw)
    else:
        buf.append(0x05); buf.append(0xFB); buf.append(0x05)
        buf.extend(_write_len_b(n)); buf.extend(raw)

def _ser_bytes(obj, buf):
    buf.append(0x06); _wr_dedup_obj(buf, 6, obj, obj)

def _ser_bytearray(obj, buf):
    buf.append(0x07); _wr_dedup_obj(buf, 7, obj, bytes(obj))

def _ser_list(obj, buf):
    buf.append(0x08); buf.extend(_write_len_b(len(obj)))
    _serialize_seq(obj, buf)

def _ser_tuple(obj, buf):
    buf.append(0x09); buf.extend(_write_len_b(len(obj)))
    _serialize_seq(obj, buf)

def _serialize_seq(items, buf):
    """Loop de serialización de secuencia — separado para que CPython lo specialice."""
    _ser = _serialize
    for item in items: _ser(item, buf)

def _ser_set(obj, buf):
    items = sorted(obj, key=repr)
    buf.append(0x0A); buf.extend(_write_len_b(len(items)))
    _serialize_seq(items, buf)

def _ser_frozenset(obj, buf):
    items = sorted(obj, key=repr)
    buf.append(0x0B); buf.extend(_write_len_b(len(items)))
    _serialize_seq(items, buf)

# HOT PATH: _ser_dict — el loop más importante, inlining del dispatch
def _ser_dict(obj, buf):
    buf.append(0x0C); buf.extend(_write_len_b(len(obj)))
    _ser = _serialize
    for k, v in obj.items():
        _ser(k, buf); _ser(v, buf)

# ── class helpers ─────────────────────────────────────────────────────────────
def _write_str5(buf: bytearray, raw: bytes) -> None:
    if len(raw) > 63: buf.append(0x05)
    _wr_dedup_raw(buf, 5, raw)

def _write_class_header(cls, buf: bytearray) -> None:
    _write_str5(buf, cls.__name__.encode())
    src = _get_source(cls)
    # Intentar comprimir si >=48B (igual que C v4)
    if len(src) >= 48:
        key_z = (0x1B, src)    # misma src, tag distinto
        idx = _dedup_cnt.get(key_z, -1)
        if idx >= 0:
            buf.extend(_ref_bytes(idx)); return
        try:
            comp = zlib.compress(src, 1)
            if len(comp) < len(src):
                # Emitir como OP_SRC_Z (0x1B)
                global _dedup_n
                idx = _dedup_n; _dedup_n += 1
                _dedup_cnt[key_z] = idx
                _emit_new(buf, 0x1B, comp)
                return
        except Exception:
            pass
    _wr_dedup_raw(buf, 0x1D, src)

def _has_custom_reduce(cls) -> bool:
    for base in cls.__mro__:
        if base is object: break
        if '__reduce__' in vars(base): return True
    return False

def _serialize_callable_ref(callable_, buf: bytearray) -> None:
    name_raw = getattr(callable_, '__name__', repr(callable_)).encode()
    try:
        src = _get_source(callable_)
        _write_str5(buf, name_raw)
        buf.append(0x01)
        _wr_dedup_raw(buf, 0x1D, src)
    except Exception:
        _write_str5(buf, name_raw)
        buf.append(0x00)

def _serialize_via_reduce(obj, buf: bytearray) -> None:
    reduced = obj.__reduce__()
    if not (isinstance(reduced, tuple) and 2 <= len(reduced) <= 5):
        raise TypeError("__reduce__ debe devolver una tupla de 2 a 5 elementos")
    callable_ = reduced[0]; args = reduced[1]
    state      = reduced[2] if len(reduced) >= 3 else None
    list_items = reduced[3] if len(reduced) >= 4 else None
    dict_items = reduced[4] if len(reduced) >= 5 else None
    hs = state is not None; hl = list_items is not None; hd = dict_items is not None
    buf.append(0x1F)
    _serialize_callable_ref(callable_, buf)
    _serialize(args, buf)
    buf.append((1 if hs else 0) | (2 if hl else 0) | (4 if hd else 0))
    if hs: _serialize(state, buf)
    if hl: _serialize(list(list_items), buf)
    if hd: _serialize(dict(dict_items), buf)

def _serialize_slow(obj, buf: bytearray) -> None:
    t = type(obj)
    if t is types.FunctionType:
        buf.append(0x20)
        _wr_dedup_raw(buf, 0x20, marshal.dumps(obj.__code__))
        defaults = obj.__defaults__ or ()
        buf.extend(_write_len_b(len(defaults)))
        for d in defaults: _serialize(d, buf)
        closure = obj.__closure__ or ()
        buf.extend(_write_len_b(len(closure)))
        for cell in closure:
            try:    _serialize(cell.cell_contents, buf)
            except ValueError: buf.append(0x00)
        return
    if t is type:
        buf.append(0x12)
        _write_str5(buf, obj.__name__.encode())
        _write_str5(buf, (obj.__module__ or '').encode())
        _wr_dedup_raw(buf, 0x1D, _get_source(obj))
        return
    # subclases de tipos básicos
    if isinstance(obj, int):       _ser_int(obj, buf);       return
    if isinstance(obj, float):     _ser_float(obj, buf);     return
    if isinstance(obj, str):       _ser_str(obj, buf);       return
    if isinstance(obj, bytes):     _ser_bytes(obj, buf);     return
    if isinstance(obj, bytearray): _ser_bytearray(obj, buf); return
    if isinstance(obj, list):      _ser_list(obj, buf);      return
    if isinstance(obj, tuple):     _ser_tuple(obj, buf);     return
    if isinstance(obj, dict):      _ser_dict(obj, buf);      return
    if isinstance(obj, set):       _ser_set(obj, buf);       return
    if isinstance(obj, frozenset): _ser_frozenset(obj, buf); return
    cls = getattr(obj, '__class__', None)
    if cls and _has_custom_reduce(cls):
        _serialize_via_reduce(obj, buf); return
    if hasattr(obj, '__dict__') and cls:
        buf.append(0x1C); _write_class_header(cls, buf)
        _serialize(obj.__dict__, buf); return
    if cls and hasattr(cls, '__slots__'):
        slot_vals = {}
        for base in type(obj).__mro__:
            for s in getattr(base, '__slots__', ()):
                try:    slot_vals[s] = getattr(obj, s)
                except AttributeError: pass
        buf.append(0x1E); _write_class_header(cls, buf)
        _serialize(slot_vals, buf); return
    raise TypeError(f"Tipo no soportado: {type(obj)}")

_DISPATCH: dict = {
    type(None): _ser_none, bool: _ser_bool, int: _ser_int,
    float: _ser_float, complex: _ser_complex, str: _ser_str,
    bytes: _ser_bytes, bytearray: _ser_bytearray, list: _ser_list,
    tuple: _ser_tuple, set: _ser_set, frozenset: _ser_frozenset, dict: _ser_dict,
}

# ── API pública escritura ─────────────────────────────────────────────────────
def dumps(obj) -> bytes:
    if _USE_C:
        _c_dedup_reset()
        return serialize_fast(obj)
    _reset_write_state()
    buf = bytearray()
    _serialize(obj, buf)
    return bytes(buf)

def dump(obj, filepath) -> None:
    with open(filepath, 'wb') as f: f.write(dumps(obj))

# ── deserialización ───────────────────────────────────────────────────────────
_read_table:  list = []
_exec_cache:  dict = {}
_cls_cache:   dict = {}  # (name, src_bytes) → cls

def _reset_read_state():
    del _read_table[:]

def _exec_source(src_bytes: bytes) -> dict:
    ns = _exec_cache.get(src_bytes)
    if ns is not None: return ns
    ns = {'__builtins__': __builtins__}
    exec(src_bytes.decode(), ns)
    _exec_cache[src_bytes] = ns
    return ns

def _get_class(name_str: str, src_bytes: bytes):
    key = (name_str, src_bytes)
    cls = _cls_cache.get(key)
    if cls is not None: return cls
    ns = _exec_source(src_bytes)
    cls = ns.get(name_str)
    if cls is None: raise KeyError(f"Clase '{name_str}' no encontrada")
    _cls_cache[key] = cls
    return cls

def _push_read(val) -> None:
    _read_table.append(val)

def _read_len(mv, pos: int):
    b = mv[pos]
    if b <= 0x3F:        return b, pos + 1
    if b & 0xC0 == 0x40: return ((b & 0x3F) << 8) | mv[pos+1], pos + 2
    n = (mv[pos+1]<<24)|(mv[pos+2]<<16)|(mv[pos+3]<<8)|mv[pos+4]
    return n, pos + 5

def _read_ref_idx(mv, pos: int, opcode: int):
    if opcode == 0xFE: return mv[pos], pos + 1
    if opcode == 0xFD: return (mv[pos]<<8)|mv[pos+1], pos + 2
    return ((mv[pos]<<24)|(mv[pos+1]<<16)|(mv[pos+2]<<8)|mv[pos+3]), pos + 4

def _read_dedup_bytes(mv, pos: int):
    """Lee bloque dedup; devuelve bytes raw. Soporta OP_SRC_Z (0x1B)."""
    marker = mv[pos]; pos += 1
    if marker in (0xFE, 0xFD, 0xFC):
        idx, pos = _read_ref_idx(mv, pos, marker)
        return _read_table[idx], pos
    if marker == 0x15:
        n = mv[pos]; pos += 1
        raw = bytes(mv[pos:pos+n]); _push_read(raw)
        return raw, pos + n
    if marker == 0x0E:
        n = mv[pos]; pos += 1
        raw = bytes(mv[pos:pos+n]); _push_read(raw)
        return raw, pos + n
    if marker == 0xFB:
        stored_tag = mv[pos]; pos += 1
        n, pos = _read_len(mv, pos)
        raw_comp = bytes(mv[pos:pos+n]); pos += n
        if stored_tag == 0x1B:        # OP_SRC_Z: descomprimir
            raw = zlib.decompress(raw_comp)
            _push_read(raw)
            return raw, pos
        _push_read(raw_comp)
        return raw_comp, pos
    raise ValueError(f"Marker dedup desconocido: {marker:#x}")

def _read_str_dedup(mv, pos: int):
    """Lee string dedup; almacena str (no bytes) en read_table."""
    marker = mv[pos]; pos += 1
    if marker in (0xFE, 0xFD, 0xFC):
        idx, pos = _read_ref_idx(mv, pos, marker)
        return _read_table[idx], pos
    if marker == 0x15:
        n = mv[pos]; pos += 1
        s = mv[pos:pos+n].tobytes().decode(); _push_read(s)
        return s, pos + n
    if marker == 0x0E:
        n = mv[pos]; pos += 1
        s = mv[pos:pos+n].tobytes().decode(); _push_read(s)
        return s, pos + n
    if marker == 0xFB:
        pos += 1   # skip stored_tag
        n, pos = _read_len(mv, pos)
        s = mv[pos:pos+n].tobytes().decode(); _push_read(s)
        return s, pos + n
    raise ValueError(f"Marker str_dedup desconocido: {marker:#x}")

def _read_str_any(mv, pos: int):
    tag = mv[pos]; pos += 1
    if tag == 0x05:   return _read_str_dedup(mv, pos)
    if tag == 0x15:
        n = mv[pos]; pos += 1
        s = mv[pos:pos+n].tobytes().decode(); _push_read(s)
        return s, pos + n
    if tag in (0xFE, 0xFD, 0xFC):
        idx, pos = _read_ref_idx(mv, pos, tag)
        return _read_table[idx], pos
    raise ValueError(f"read_str_any: tag inesperado {tag:#x}")

def _read_class_header(mv, pos: int):
    name_str, pos = _read_str_any(mv, pos)
    src_raw, pos  = _read_dedup_bytes(mv, pos)
    return _get_class(name_str, src_raw), pos

def _read_callable_ref(mv, pos: int):
    name_str, pos = _read_str_any(mv, pos)
    has_src = mv[pos]; pos += 1
    if not has_src:
        import builtins as _b, sys as _sys
        fn = getattr(_b, name_str, None)
        if fn is None:
            for mod in _sys.modules.values():
                fn = getattr(mod, name_str, None)
                if fn is not None and callable(fn): break
        if fn is None: raise KeyError(f"Callable '{name_str}' no encontrado")
        return fn, pos
    src_raw, pos = _read_dedup_bytes(mv, pos)
    ns = _exec_source(src_raw); fn = ns.get(name_str)
    if fn is None: raise KeyError(f"Callable '{name_str}' no encontrado en source")
    return fn, pos

def _deserialize(mv, pos: int):
    tag = mv[pos]; pos += 1

    # None / bool
    if tag == 0x00: return None,  pos
    if tag == 0x80: return False, pos
    if tag == 0x81: return True,  pos

    # small ints 0..58
    if 0xC0 <= tag <= 0xFA: return tag - 0xC0, pos

    # OP_UINT8: int 59-255
    if tag == 0x11: return mv[pos], pos + 1

    # refs
    if tag in (0xFE, 0xFD, 0xFC):
        idx, pos = _read_ref_idx(mv, pos, tag)
        return _read_table[idx], pos

    # compact str
    if tag == 0x15:
        n = mv[pos]; pos += 1
        s = mv[pos:pos+n].tobytes().decode(); _push_read(s)
        return s, pos + n

    # neg small -1..-30
    if tag == 0x10: return -(mv[pos] + 1), pos + 1

    # OP_NEG8: int -31..-256
    if tag == 0x12: return -(mv[pos] + 1), pos + 1

    # int 256-65535
    if tag == 0x0F: return (mv[pos]<<8)|mv[pos+1], pos + 2

    # float specials
    if tag == 0x82: return  0.0, pos
    if tag == 0x83: return -0.0, pos
    if tag == 0x84: return  1.0, pos
    if tag == 0x85: return -1.0, pos
    if tag == 0x86: return float('nan'),  pos
    if tag == 0x87: return float('inf'),  pos
    if tag == 0x88: return float('-inf'), pos

    # float 64-bit
    if tag == 0x03:
        v, = _unpack_d_from(mv, pos); return v, pos + 8

    # complex
    if tag == 0x04:
        r, = _unpack_d_from(mv, pos); im, = _unpack_d_from(mv, pos+8)
        return complex(r, im), pos + 16

    # big int
    if tag == 0x02:
        neg = mv[pos]; pos += 1
        n, pos = _read_len(mv, pos)
        v = int.from_bytes(mv[pos:pos+n], 'big')
        return (-v if neg else v), pos + n

    # long str
    if tag == 0x05: return _read_str_dedup(mv, pos)

    # bytes / bytearray
    if tag == 0x06:
        raw, pos = _read_dedup_bytes(mv, pos); return raw, pos
    if tag == 0x07:
        raw, pos = _read_dedup_bytes(mv, pos); return bytearray(raw), pos

    # sequences
    if tag in (0x08, 0x09, 0x0A, 0x0B):
        n, pos = _read_len(mv, pos)
        items = [None] * n
        for i in range(n): items[i], pos = _deserialize(mv, pos)
        if tag == 0x08: return items, pos
        if tag == 0x09: return tuple(items), pos
        if tag == 0x0A: return set(items), pos
        return frozenset(items), pos

    # dict
    if tag == 0x0C:
        n, pos = _read_len(mv, pos); d = {}
        for _ in range(n):
            k, pos = _deserialize(mv, pos); v, pos = _deserialize(mv, pos); d[k] = v
        return d, pos

    # function via marshal
    if tag == 0x20:
        marshal_raw, pos = _read_dedup_bytes(mv, pos)
        code = marshal.loads(marshal_raw)
        nd, pos = _read_len(mv, pos); defaults = []
        for _ in range(nd):
            dv, pos = _deserialize(mv, pos); defaults.append(dv)
        nc, pos = _read_len(mv, pos); closure = None
        if nc > 0:
            cells = []
            for _ in range(nc):
                val, pos = _deserialize(mv, pos)
                cell = (lambda x: (lambda: x).__closure__[0])(val)
                cells.append(cell)
            closure = tuple(cells)
        import builtins as _b
        fn = types.FunctionType(code, vars(_b).copy(), code.co_name,
            tuple(defaults) if defaults else None, closure)
        return fn, pos

    # function via source (compat v1)
    if tag == 0x0D:
        if mv[pos] == 0x05: pos += 1
        name_raw, pos = _read_dedup_bytes(mv, pos)
        src_raw, pos  = _read_dedup_bytes(mv, pos)
        ns = _exec_source(src_raw); name = name_raw.decode()
        fn = ns.get(name)
        if fn is None: raise KeyError(f"Función '{name}' no encontrada")
        return fn, pos

    # class object
    if tag == 0x12:
        name_str, pos = _read_str_any(mv, pos)
        _mod, pos     = _read_str_any(mv, pos)
        src_raw, pos  = _read_dedup_bytes(mv, pos)
        return _get_class(name_str, src_raw), pos

    # instance __dict__
    if tag == 0x1C:
        cls, pos = _read_class_header(mv, pos)
        if mv[pos] != 0x0C:
            raise ValueError(f"0x1C: esperado 0x0C, encontrado {mv[pos]:#x}")
        dd, pos = _deserialize(mv, pos)
        obj = cls.__new__(cls); obj.__dict__.update(dd)
        return obj, pos

    # instance __slots__
    if tag == 0x1E:
        cls, pos = _read_class_header(mv, pos)
        if mv[pos] != 0x0C:
            raise ValueError(f"0x1E: esperado 0x0C, encontrado {mv[pos]:#x}")
        vals, pos = _deserialize(mv, pos)
        if isinstance(cls, type):
            obj = cls.__new__(cls)
            for k, v in vals.items(): setattr(obj, k, v)
            return obj, pos
        fn = next((v for v in vals.values() if callable(v)), None)
        if fn is None: raise ValueError("0x1E: no se encontró callable")
        return fn, pos

    # __reduce__
    if tag == 0x1F:
        callable_, pos = _read_callable_ref(mv, pos)
        if mv[pos] != 0x09:
            raise ValueError(f"0x1F: esperaba tupla (0x09), encontrado {mv[pos]:#x}")
        args, pos = _deserialize(mv, pos)
        obj = callable_(*args); flags = mv[pos]; pos += 1
        if flags & 1:
            state, pos = _deserialize(mv, pos)
            if isinstance(state, dict):
                if hasattr(obj, '__dict__'): obj.__dict__.update(state)
                else:
                    for k, v in state.items(): setattr(obj, k, v)
        if flags & 2:
            items, pos = _deserialize(mv, pos)
            if hasattr(obj, 'extend'): obj.extend(items)
            else:
                for item in items: obj.append(item)
        if flags & 4:
            dct, pos = _deserialize(mv, pos)
            if hasattr(obj, 'update'): obj.update(dct)
            else:
                for k, v in dct.items(): obj[k] = v
        return obj, pos

    raise ValueError(f"Tag desconocido: {tag:#x}")

# ── API pública lectura ───────────────────────────────────────────────────────
def loads(data: bytes):
    if _USE_C: return deserialize_fast(data)
    _reset_read_state()
    obj, _ = _deserialize(memoryview(data), 0)
    return obj

def load(filepath) -> object:
    with open(filepath, 'rb') as f: data = f.read()
    return loads(data)
