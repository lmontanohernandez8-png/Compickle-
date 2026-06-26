<p align="center">
  <img src="https://raw.githubusercontent.com/lmontanohernandez8-png/Micronnx/main/logo2.png" width="600"/>
</p>

# 🥒 compickle

**Serialización binaria para Python con motor en C — rápido, compacto y sin dependencias.**

[![Python](https://img.shields.io/badge/Python-3.9%2B-blue?style=flat-square&logo=python&logoColor=white)](https://www.python.org/)
[![Motor C](https://img.shields.io/badge/Motor-C%20nativo-orange?style=flat-square&logo=c&logoColor=white)]()
[![Versión](https://img.shields.io/badge/versión-1.0.9-blue?style=flat-square)]()
[![Licencia](https://img.shields.io/badge/Licencia-MIT-green?style=flat-square)]()
[![Compilación](https://img.shields.io/badge/Compilado--O3%20--march%3Dnative-red?style=flat-square)]()

---

## ¿Qué es compickle?

`compickle` es un serializador binario escrito principalmente en **C** y expuesto como extensión nativa de Python (`_compickle`). Está diseñado para ser **simple de usar** y **más rápido que `pickle`** en la mayoría de cargas de trabajo, gracias a:

- Un **buffer de salida dinámico** (`Buf`) que crece en potencias de 2 desde 256 bytes inicial.
- Un **`DedupState` local por llamada**: en v2 el estado de deduplicación vive en el stack de cada `serialize_fast()` — elimina por completo la necesidad de llamar `dedup_reset()` antes de cada `dumps()`.
- Una **tabla de deduplicación FNV-1a dinámica** que arranca con 256 entradas y 512 buckets, y crece (rehash automático con factor de carga máximo 0.65) sin límite fijo de capacidad.
- Un **id_cache por identidad de puntero Python** (`uintptr_t`): shortcut O(1) antes del hash FNV-1a para objetos que aparecen repetidos en el mismo árbol de serialización.
- Un **arena allocator interno** para los datos de dedup: bloque contiguo de 1 MB inicial, reset = mover un puntero, sin `malloc`/`free` por cada entrada.
- Un **read_table dinámico** en el deserializador: arranca con 256 entradas y crece con `realloc`, eliminando el límite fijo de 8 192 entradas de v1.
- **Funciones via `marshal`** (opcode `0x20`): lambdas, generator functions y funciones normales se serializan por bytecode, sin depender de `inspect.getsource`. Funciona en REPL y `eval()`.
- **Floats especiales** (`0.0`, `-0.0`, `1.0`, `-1.0`, `NaN`, `±inf`) en 1 byte cada uno (opcodes `0x82`–`0x88`).
- **Enteros negativos pequeños** `-1`..`-30` en 2 bytes (opcode `0x10`).
- **Compact string opcode** `0x15` para strings nuevas cortas (≤ 63 bytes UTF-8): ahorra 1 byte frente al camino largo.
- **Compilación nativa** con `-O3 -march=native -mtune=native` aplicada automáticamente en `setup.py`.
- Un **fallback puro en Python** (`compickle.py`) que implementa exactamente el mismo protocolo binario y se activa si la extensión C no está disponible.
- **Lazy loading** en `__init__.py`: la extensión C se importa solo en la primera llamada real a `dumps`, `loads`, `dump`, `load`, `dedup_reset` o `backend()`.

> **Versión actual:** `1.0.9` (definida en `__init__.py`)

---

## ⚙️ Instalación

```bash
pip install .
```

Para desarrollo (compilación en el directorio actual):

```bash
python setup.py build_ext --inplace
```

`setup.py` detecta la arquitectura vía `platform.machine()` y añade `-march=native -mtune=native` automáticamente en GCC/Clang. La extensión nativa se construye como `_compickle` (con guión bajo) y el módulo público `compickle` la importa internamente.

---

## 🚀 Uso rápido

```python
import compickle

datos = {
    "nombre": "Rex",
    "edad": 5,
    "activo": True,
    "coordenadas": (4.0, 2.0),
    "etiquetas": {"perro", "mascota"},
}

# Serializar a archivo
compickle.dump(datos, "datos.cpkl")

# Deserializar desde archivo
copia = compickle.load("datos.cpkl")

# Serializar/deserializar en memoria (bytes)
raw = compickle.dumps(datos)
copia2 = compickle.loads(raw)

# Ver qué motor está activo
print(compickle.backend())  # → 'c' o 'python'
```

---

## 📖 API completa

### `compickle.dump(obj, path)`

Serializa `obj` y escribe el resultado binario en `path`. Internamente llama a `dumps(obj)` y abre el archivo en modo `'wb'`.

```python
compickle.dump(mi_objeto, "salida.cpkl")
```

---

### `compickle.dumps(obj) → bytes`

Serializa `obj` y devuelve los bytes resultantes directamente.

En v2 con el motor C, cada llamada a `dumps()` crea su propio `DedupState` en el stack — **no es necesario llamar `dedup_reset()` antes de cada `dumps()`**. En el fallback Python, `dumps()` llama `_reset_write_state()` internamente.

```python
raw: bytes = compickle.dumps(mi_objeto)
```

---

### `compickle.load(path) → object`

Lee el archivo binario en `path` y reconstruye el objeto original.

```python
obj = compickle.load("salida.cpkl")
```

---

### `compickle.loads(data: bytes) → object`

Deserializa directamente desde un objeto `bytes`.

```python
obj = compickle.loads(raw_bytes)
```

---

### `compickle.dedup_reset()`

Limpia los caches globales de source y ejecución tanto del motor C (`source_cache`, `exec_cache`) como del fallback Python (`_reset_write_state()`).

En v2 **esto ya no es necesario antes de cada `dumps()`** — el `DedupState` del motor C es local por llamada. Úsalo únicamente para liberar memoria en procesos de larga duración que hayan serializado muchas clases o funciones distintas.

```python
compickle.dedup_reset()
```

---

### `compickle.backend() → str`

Devuelve el motor activo. Dispara el lazy loading si aún no se había importado la extensión.

```python
compickle.backend()  # → 'c'      (extensión _compickle compilada)
                     # → 'python' (fallback puro Python)
```

---

## 🧩 Tipos soportados

| Tipo Python              | Tag         | Deduplicado | Notas                                                         |
|--------------------------|-------------|:-----------:|---------------------------------------------------------------|
| `None`                   | `0x00`      | —           | 1 byte                                                        |
| `False`                  | `0x80`      | —           | 1 byte                                                        |
| `True`                   | `0x81`      | —           | 1 byte                                                        |
| `int` (0–58)             | `0xC0–0xFA` | —           | 1 byte: opcode directo `0xC0 + valor`                         |
| `int` (-1..-30)          | `0x10`      | —           | 2 bytes: `[0x10][magnitud - 1]`                               |
| `int` (59–65535)         | `0x0F`      | —           | 3 bytes: tag + `uint16` big-endian                            |
| `int` (arbitrario)       | `0x02`      | —           | signo(1) + write_len + bytes big-endian                       |
| `float` (+0.0)           | `0x82`      | —           | 1 byte                                                        |
| `float` (-0.0)           | `0x83`      | —           | 1 byte (distinguido del +0.0 por sign bit)                    |
| `float` (1.0)            | `0x84`      | —           | 1 byte                                                        |
| `float` (-1.0)           | `0x85`      | —           | 1 byte                                                        |
| `float` (NaN)            | `0x86`      | —           | 1 byte                                                        |
| `float` (+inf)           | `0x87`      | —           | 1 byte                                                        |
| `float` (-inf)           | `0x88`      | —           | 1 byte                                                        |
| `float` (general)        | `0x03`      | —           | 9 bytes: tag + IEEE 754 doble precisión big-endian            |
| `complex`                | `0x04`      | —           | 17 bytes: tag + dos `float64` big-endian                      |
| `str`                    | `0x05`/`0x15` | ✅         | UTF-8 + dedup FNV-1a; `0x15` para strings nuevas ≤ 63 bytes  |
| `bytes`                  | `0x06`      | ✅           | Deduplicado por contenido                                     |
| `bytearray`              | `0x07`      | ✅           | Deduplicado por contenido                                     |
| `list`                   | `0x08`      | —           | Elementos serializados recursivamente                         |
| `tuple`                  | `0x09`      | —           | Elementos serializados recursivamente                         |
| `set`                    | `0x0A`      | —           | Ordenado por `repr()` para serialización determinista         |
| `frozenset`              | `0x0B`      | —           | Ordenado por `repr()` para serialización determinista         |
| `dict`                   | `0x0C`      | —           | Claves y valores serializados recursivamente                  |
| `function` / `lambda`    | `0x20`      | ✅ (bytecode)| `marshal` del code object + defaults + freevars               |
| Generador (objeto)       | `0x08`      | —           | Se materializa en lista consumiendo el generador              |
| `type` (clase)           | `0x12`      | ✅           | Nombre + módulo + fuente (`inspect.getsource`)                |
| Instancia con `__dict__` | `0x1C`      | ✅ (fuente)  | Encabezado de clase + `__dict__` serializado                  |
| Instancia con `__slots__`| `0x1E`      | ✅ (fuente)  | Recorre MRO completo para capturar todos los slots            |
| Objeto con `__reduce__`  | `0x1F`      | ✅           | Soporta tuplas de 2 a 5 elementos; `None` en pos. 2–4 se ignora |

> **Prioridad de serialización de instancias:**
> 1. `__reduce__` personalizado en el MRO → tag `0x1F`
> 2. `__dict__` disponible → tag `0x1C`
> 3. `__slots__` sin `__dict__` → tag `0x1E`

---

## 🔬 Cómo funciona internamente

### Motor C (`compickle.c`, ~1 476 líneas)

#### Buffer de salida (`Buf`)

```c
typedef struct { uint8_t *buf; Py_ssize_t len, cap; } Buf;
```

El buffer comienza con capacidad 256 bytes y se duplica vía `realloc` cuando el espacio es insuficiente. Las funciones primitivas son `buf_u8`, `buf_u16be`, `buf_u64be` y `buf_raw`.

#### Arena allocator

```c
typedef struct { uint8_t *base; size_t pos; size_t cap; } Arena;
```

Bloque contiguo de 1 MB inicial para los datos de cada entrada dedup. Reset = mover `arena.pos` a 0. Sin `malloc`/`free` por entrada — elimina la fragmentación de memoria en serializaciones intensivas.

#### DedupState local por llamada (novedad v2)

```c
typedef struct {
    Arena    arena;
    DEntry  *table;   /* tabla de entradas (dinámica, inicia en 256) */
    int      count, cap;
    int     *buckets; /* tabla hash (inicia en 512 buckets, potencia de 2) */
    int      nbuckets;
    IdEntry *id_tab;  /* tabla hash de punteros Python → índice */
    int      id_nb, id_count;
} DedupState;
```

En v1, `dedup_table` era un array estático global de 8 192 entradas. En v2 el `DedupState` es local al stack de `py_serialize_fast()` y se destruye al terminar cada llamada. Esto elimina el bug de estado compartido entre llamadas consecutivas sin `dedup_reset()` explícito.

El rehash ocurre automáticamente cuando el factor de carga supera 0.65. El `id_tab` (cache por puntero) rehashea cuando supera 0.60.

#### id_cache (shortcut O(1) por identidad)

Antes de calcular `FNV-1a`, se busca `id(obj)` en `id_tab` por probing lineal sobre `uintptr_t`. Si hay hit, se emite la referencia directamente sin hash ni `memcmp`.

#### Tabla hash FNV-1a

```c
static uint32_t fnv1a(uint8_t tag, const uint8_t *d, Py_ssize_t n);
```

Hash de 32 bits aplicado sobre el `tag` de tipo seguido de los bytes del dato. Las colisiones se resuelven con listas enlazadas dentro de `DEntry.next`.

#### Codificación de longitudes variables

```
n ≤ 0x3F    → 1 byte
n ≤ 0x3FFF  → 2 bytes (0x40 | n>>8 , n & 0xFF)
n > 0x3FFF  → 5 bytes (0xFF + uint32 big-endian)
```

#### Compact string opcode (`0x15`)

Para strings nuevas con codificación UTF-8 de longitud ≤ 63 bytes:

```
[0x15] [len_1byte] [datos...]   → 2 + len bytes
```

Frente al camino general para strings nuevas largas:

```
[0x05] [0xFB] [0x05] [len...] [datos...]
```

#### Referencias dedup

```
idx ≤ 0xFE   → [0xFE] [idx_1byte]      (2 bytes total)
idx ≤ 0xFFFF → [0xFD] [idx_2bytes_be]  (3 bytes total)
idx > 0xFFFF → [0xFC] [idx_4bytes_be]  (5 bytes total)
```

Si hay hit en dedup, **no se emite el type tag** — solo la referencia. El deserializador reconstruye el tipo desde `read_table_types[idx]`.

#### Funciones vía `marshal` (opcode `0x20`)

El code object se serializa con `marshal.dumps()` y se deduplica en la tabla. A continuación se emiten los defaults posicionales y los valores de las variables libres (freevars). Cells vacías se emiten como `None` (`0x00`).

Al deserializar se reconstruye la función con `types.FunctionType` (C: `PyFunction_New`) usando `builtins` como globals.

#### Objetos generador

`PyGen_CheckExact` / `PyCoro_CheckExact` → se materializan en lista con `PySequence_List` (consume el generador) y se emiten con opcode `0x08`.

#### read_table dinámico (novedad v2)

En v1 era un array fijo de 8 192 `PyObject*`. En v2 arranca con 256 entradas y crece con `realloc` sin límite. El `reset` solo decrementa `read_table_count` y hace `Py_XDECREF` de las referencias — los buffers se reutilizan entre llamadas.

#### Caches globales (`source_cache`, `exec_cache`)

Son los únicos estados globales que persisten entre llamadas:

- `source_cache`: `id(obj)` → `bytes` del source vía `inspect.getsource`. Evita I/O repetido.
- `exec_cache`: `src_bytes` → `namespace dict` de `PyRun_String`. Evita re-ejecutar el mismo source al deserializar.

`dedup_reset()` limpia ambos; el `DedupState` de serialización se destruye automáticamente al terminar cada llamada.

#### Enteros arbitrarios — compatibilidad 3.9–3.13+

Para Python < 3.13 se usa `_PyLong_AsByteArray` con firma de 5 argumentos. Para Python ≥ 3.13 (donde cambió la firma) se usa la variante de 6 argumentos con `#if PY_VERSION_HEX >= 0x030d0000`. El número de bytes se calcula con `bit_length()` vía `PyObject_CallMethod`.

---

### Fallback Python (`compickle.py`, ~794 líneas)

Implementa exactamente el mismo protocolo binario en Python puro. Diferencias internas:

- La deduplicación usa `_dedup_by_content: dict[tuple[int, bytes], int]` más `_dedup_by_id: dict[int, int]` para el shortcut por identidad.
- Un `_ref_bytes_cache: dict[int, bytes]` precalcula los bytes de cada referencia.
- La serialización acumula en un `bytearray` (append nativo, sin concatenación de `bytes`).
- La deserialización usa `memoryview` para acceso sin copia.
- Los closures se reconstruyen con `types.FunctionType` usando la misma técnica de `lambda v: (lambda: v).__closure__[0]` que el motor C.
- Dispatch por tipo exacto en `_SERIALIZE_DISPATCH: dict[type, Callable]` — evita la cascada de `isinstance` en el ~99 % de los casos.
- `dumps()` llama `_reset_write_state()` internamente — mismo comportamiento que el motor C en v2.

---

### Lazy loading (`__init__.py`, ~103 líneas)

```python
def _ensure_loaded():
    global _USE_C, _serialize_fast, _deserialize_fast, _c_dedup_reset
    if _USE_C is not None:
        return
    try:
        from ._compickle import serialize_fast, deserialize_fast, dedup_reset
        _USE_C = True
    except ImportError:
        _USE_C = False
```

`_ensure_loaded()` se invoca en la primera llamada a cualquier función pública. Un `import compickle` nunca falla aunque la extensión C no esté compilada.

`__getattr__` resuelve `_USE_C`, `serialize_fast` y `deserialize_fast` bajo demanda para compatibilidad con código que acceda a estos nombres directamente en el módulo.

---

## 🏗️ Estructura del proyecto

```
compickle/
├── compickle.c     # Motor principal en C (~1 476 líneas) — extensión CPython _compickle
├── compickle.py    # Implementación de referencia en Python puro (~794 líneas)
├── __init__.py     # API pública con lazy loading (~103 líneas), versión 1.0.9
└── setup.py        # Build con -O3 -march=native -mtune=native
```

---

## 🧪 Ejemplos avanzados

### Clase con `__dict__`

```python
import compickle

class Punto:
    def __init__(self, x, y):
        self.x = x
        self.y = y

p = Punto(3.0, 7.5)
compickle.dump(p, "punto.cpkl")
p2 = compickle.load("punto.cpkl")
print(p2.x, p2.y)  # → 3.0  7.5
```

La clase se serializa con su código fuente completo (vía `inspect.getsource`). Al deserializar, el source se ejecuta en un namespace aislado y la clase se recupera por nombre.

---

### Clase con `__slots__`

```python
class Vector:
    __slots__ = ("x", "y", "z")
    def __init__(self, x, y, z):
        self.x, self.y, self.z = x, y, z

v = Vector(1, 2, 3)
compickle.dump(v, "vector.cpkl")
v2 = compickle.load("vector.cpkl")
print(v2.x, v2.y, v2.z)  # → 1  2  3
```

El motor recorre el MRO completo para capturar todos los slots, incluyendo los heredados. Solo se incluyen los slots con valor asignado.

---

### Objeto con `__reduce__`

```python
class Color:
    def __init__(self, r, g, b):
        self.r, self.g, self.b = r, g, b

    def __reduce__(self):
        return (Color, (self.r, self.g, self.b))

c = Color(255, 128, 0)
compickle.dump(c, "color.cpkl")
c2 = compickle.load("color.cpkl")
print(c2.r, c2.g, c2.b)  # → 255  128  0
```

`__reduce__` puede devolver tuplas de 2 a 5 elementos: `(callable, args[, state[, list_items[, dict_items]]])`. Los elementos `None` en posiciones 2–4 se tratan como ausentes. El callable debe tener `__name__`; si no tiene source accesible, se busca en `builtins` y módulos ya importados al deserializar.

---

### Lambda y funciones vía marshal

```python
import compickle

fn = lambda x: x * 2
raw = compickle.dumps(fn)
fn2 = compickle.loads(raw)
print(fn2(21))  # → 42
```

En v2, las funciones se serializan por bytecode (`marshal`). Esto funciona en el REPL y con lambdas, sin necesidad de archivo fuente en disco.

---

### Serialización en memoria (`dumps` / `loads`)

```python
import compickle

datos = [1, "hola", {"a": True}, (3.14,)]
raw = compickle.dumps(datos)
print(type(raw))   # → <class 'bytes'>

restaurado = compickle.loads(raw)
assert restaurado == datos
```

---

## 📦 Formato binario — tabla completa de opcodes

```
Opcode        Tipo / Significado
──────────────────────────────────────────────────────────────────────────
0x00          None
0x02          int arbitrario: signo(1) + write_len + bytes big-endian
0x03          float general: 8 bytes IEEE 754 big-endian
0x04          complex: 2 × float64 big-endian (16 bytes)
0x05          str: tag + write_dedup(UTF-8) [strings largas o refs]
0x06          bytes: tag + write_dedup(contenido)
0x07          bytearray: tag + write_dedup(contenido)
0x08          list / generador materializado: write_len(n) + n × serialize(item)
0x09          tuple: write_len(n) + n × serialize(item)
0x0A          set: write_len(n) + n × serialize(item ordenado por repr())
0x0B          frozenset: write_len(n) + n × serialize(item ordenado por repr())
0x0C          dict: write_len(n) + n × (serialize(k) + serialize(v))
0x0E          dedup short (nuevo): longitud(1 byte, ≤ 63) + datos
0x0F          int 16 bits: 2 bytes uint16 big-endian (rango 59–65535)
0x10          int negativo pequeño: [0x10][magnitud - 1] (rango -1..-30)
0x12          class: str_nombre + str_módulo + write_dedup(source)
0x15          str corta nueva (≤ 63 bytes UTF-8): [0x15][len][datos] — ahorra 1 byte
0x1C          instancia __dict__: class_header + serialize(__dict__)
0x1D          (interno) tag de source en tabla de dedup
0x1E          instancia __slots__: class_header + serialize(dict de slots)
0x1F          instancia __reduce__: callable_ref + args(0x09) + flags(1) + [state] + [list_items] + [dict_items]
0x20          función/lambda vía marshal: [marshal_dedup][n_def][defaults...][n_free][freevars...]
0x80          False
0x81          True
0x82          float +0.0
0x83          float -0.0
0x84          float 1.0
0x85          float -1.0
0x86          float NaN
0x87          float +inf
0x88          float -inf
0xC0–0xFA     int pequeño positivo (valor = opcode − 0xC0, rango 0–58)
0xFB          dedup long (nuevo): type_tag(1) + write_len + datos (> 63 bytes)
0xFC          referencia dedup: 4 bytes big-endian de índice
0xFD          referencia dedup: 2 bytes big-endian de índice
0xFE          referencia dedup: 1 byte de índice (índice ≤ 254)
```

---

## ⚠️ Limitaciones conocidas

- **Clases y callables de `__reduce__` requieren source:** `inspect.getsource()` debe poder acceder al código fuente al serializar. No funciona con clases definidas en el REPL. Las funciones normales y lambdas sí funcionan en el REPL vía `marshal` (opcode `0x20`).
- **Funciones no portables entre versiones de Python:** el bytecode serializado con `marshal` tiene un magic number ligado a la versión de CPython. Un stream serializado con Python 3.11 no puede deserializarse con Python 3.12.
- **Generadores objeto se consumen:** serializar un objeto generador (`g = gen_fn()`) lo materializa en lista y consume el generador. Para serializar la función generadora, pasar `gen_fn` directamente.
- **No compatible con `pickle`:** el formato binario es propio y no intercambiable con `pickle`, `marshal` u otros serializadores estándar de Python.
- **`__reduce__` requiere callable con `__name__`:** el callable devuelto por `__reduce__` debe tener atributo `__name__`. Si no tiene source accesible se busca en `builtins` y módulos ya importados al deserializar; si tampoco se encuentra ahí, la deserialización falla con `KeyError`.

---

## 📄 Licencia

MIT — úsalo como quieras.
