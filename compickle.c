#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>

/* Extra opcodes v4 */
#define OP_UINT8   0x11  /* int 59-254: 2 bytes total */
#define OP_NEG8    0x12  /* int -31..-256: 2 bytes total (stores abs-31) */
#define OP_SRC_Z   0x1B  /* class source comprimida con zlib */
#define OP_SRC_RAW 0x1D  /* class source sin comprimir (existente) */

#ifdef __GNUC__
#  define FORCE_INLINE __attribute__((always_inline)) static inline
#else
#  define FORCE_INLINE static inline
#endif

/* ── arena allocator ─────────────────────────────────────────────────────── */
#define ARENA_INIT  (1 << 20)

typedef struct {
    uint8_t *base;
    size_t   pos;
    size_t   cap;
} Arena;

FORCE_INLINE uint8_t *arena_alloc(Arena *a, size_t n) {
    if (!n) return (uint8_t *)"";
    /* Sin alineación: strings y bytes no necesitan alineación de 8B */
    if (__builtin_expect(a->pos + n > a->cap, 0)) {
        size_t new_cap = a->cap ? a->cap * 2 : ARENA_INIT;
        while (new_cap < a->pos + n) new_cap *= 2;
        uint8_t *p = realloc(a->base, new_cap);
        if (!p) return NULL;
        a->base = p;
        a->cap  = new_cap;
    }
    uint8_t *ptr = a->base + a->pos;
    a->pos += n;
    return ptr;
}

static void arena_free(Arena *a) {
    free(a->base); a->base = NULL; a->pos = 0; a->cap = 0;
}

/* ── Buf ─────────────────────────────────────────────────────────────────── */
typedef struct { uint8_t *buf; Py_ssize_t len, cap; } Buf;

static int buf_grow(Buf *b, Py_ssize_t need) {
    Py_ssize_t nc = b->cap ? b->cap * 2 : 512;
    while (nc < b->len + need) nc *= 2;
    uint8_t *p = realloc(b->buf, nc);
    if (!p) { PyErr_NoMemory(); return 0; }
    b->buf = p; b->cap = nc; return 1;
}

FORCE_INLINE int buf_u8(Buf *b, uint8_t v) {
    if (__builtin_expect(b->len >= b->cap, 0))
        if (!buf_grow(b, 1)) return 0;
    b->buf[b->len++] = v; return 1;
}

FORCE_INLINE int buf_u8_2(Buf *b, uint8_t a, uint8_t c) {
    if (__builtin_expect(b->len + 2 > b->cap, 0))
        if (!buf_grow(b, 2)) return 0;
    b->buf[b->len] = a; b->buf[b->len+1] = c; b->len += 2; return 1;
}

FORCE_INLINE int buf_raw(Buf *b, const uint8_t *d, Py_ssize_t n) {
    if (__builtin_expect(b->len + n > b->cap, 0))
        if (!buf_grow(b, n)) return 0;
    memcpy(b->buf + b->len, d, n); b->len += n; return 1;
}

FORCE_INLINE int buf_u16be(Buf *b, uint16_t v) {
    if (__builtin_expect(b->len + 2 > b->cap, 0))
        if (!buf_grow(b, 2)) return 0;
    b->buf[b->len]   = (uint8_t)(v >> 8);
    b->buf[b->len+1] = (uint8_t)(v & 0xFF);
    b->len += 2; return 1;
}

FORCE_INLINE int buf_u64be(Buf *b, uint64_t v) {
    if (__builtin_expect(b->len + 8 > b->cap, 0))
        if (!buf_grow(b, 8)) return 0;
    uint8_t *p = b->buf + b->len;
    p[0]=(uint8_t)(v>>56); p[1]=(uint8_t)(v>>48);
    p[2]=(uint8_t)(v>>40); p[3]=(uint8_t)(v>>32);
    p[4]=(uint8_t)(v>>24); p[5]=(uint8_t)(v>>16);
    p[6]=(uint8_t)(v>>8);  p[7]=(uint8_t)(v&0xFF);
    b->len += 8; return 1;
}

/* ── dedup ───────────────────────────────────────────────────────────────── */
#define DEDUP_CAP_INIT   256
#define HASH_INIT        512
#define HASH_LOAD_MAX    0.65
#define ID_CACHE_INIT    512

typedef struct { uint8_t tag; uint8_t *data; Py_ssize_t len; int next; uint32_t h; } DEntry;
typedef struct { uintptr_t key; int val; } IdEntry;

typedef struct {
    Arena    arena;
    DEntry  *table;
    int      count, cap;
    int     *buckets, nbuckets;
    IdEntry *id_tab;
    int      id_nb, id_count;
} DedupState;

FORCE_INLINE uint32_t fnv1a(uint8_t tag, const uint8_t *d, Py_ssize_t n) {
    uint32_t h = 2166136261u;
    h = (h ^ tag) * 16777619u;
    for (Py_ssize_t i = 0; i < n; i++) h = (h ^ d[i]) * 16777619u;
    return h;
}

/* py_hash_to_bucket: use Python object hash (already computed, O(1)) as bucket key */
FORCE_INLINE uint32_t py_hash_bucket(PyObject *obj, uint8_t tag) {
    Py_hash_t h = PyObject_Hash(obj);
    if (h == -1) { PyErr_Clear(); h = 0; }
    return (uint32_t)((uint64_t)(unsigned long long)h * 2654435761ULL ^ tag) ;
}

static int dedup_init(DedupState *ds) {
    ds->arena = (Arena){NULL,0,0};
    ds->table = NULL; ds->count = ds->cap = 0;
    ds->nbuckets = HASH_INIT;
    ds->buckets = malloc((size_t)HASH_INIT * sizeof(int));
    if (!ds->buckets) { PyErr_NoMemory(); return 0; }
    memset(ds->buckets, -1, (size_t)HASH_INIT * sizeof(int));
    ds->id_nb = ID_CACHE_INIT; ds->id_count = 0;
    ds->id_tab = calloc((size_t)ID_CACHE_INIT, sizeof(IdEntry));
    if (!ds->id_tab) { free(ds->buckets); PyErr_NoMemory(); return 0; }
    return 1;
}

static void dedup_destroy(DedupState *ds) {
    arena_free(&ds->arena);
    free(ds->table); free(ds->buckets); free(ds->id_tab);
    ds->table=NULL; ds->buckets=NULL; ds->id_tab=NULL;
    ds->count=ds->cap=ds->nbuckets=ds->id_nb=ds->id_count=0;
}

FORCE_INLINE int id_tab_find(DedupState *ds, PyObject *obj) {
    if (!ds->id_count) return -1;
    uintptr_t key = (uintptr_t)obj;
    int mask = ds->id_nb - 1;
    int slot = (int)((key >> 3) & (uintptr_t)mask);
    for (;;) {
        IdEntry *e = &ds->id_tab[slot];
        if (!e->key) return -1;
        if (e->key == key) return e->val;
        slot = (slot + 1) & mask;
    }
}

static void id_tab_insert(DedupState *ds, PyObject *obj, int idx) {
    if (ds->id_count >= (int)(ds->id_nb * 0.6)) {
        int new_nb = ds->id_nb * 2;
        IdEntry *new_t = calloc((size_t)new_nb, sizeof(IdEntry));
        if (!new_t) return;
        int mask = new_nb - 1;
        for (int i = 0; i < ds->id_nb; i++) {
            if (!ds->id_tab[i].key) continue;
            int s = (int)((ds->id_tab[i].key >> 3) & (uintptr_t)mask);
            while (new_t[s].key) s = (s+1) & mask;
            new_t[s] = ds->id_tab[i];
        }
        free(ds->id_tab); ds->id_tab = new_t; ds->id_nb = new_nb;
    }
    uintptr_t key = (uintptr_t)obj;
    int mask = ds->id_nb - 1;
    int slot = (int)((key >> 3) & (uintptr_t)mask);
    while (ds->id_tab[slot].key) slot = (slot+1) & mask;
    ds->id_tab[slot].key = key; ds->id_tab[slot].val = idx;
    ds->id_count++;
}

static int dedup_rehash(DedupState *ds) {
    int new_nb = ds->nbuckets * 2;
    int *new_b = malloc((size_t)new_nb * sizeof(int));
    if (!new_b) { PyErr_NoMemory(); return 0; }
    memset(new_b, -1, (size_t)new_nb * sizeof(int));
    int mask = new_nb - 1;
    for (int i = 0; i < ds->count; i++) {
        DEntry *e = &ds->table[i];
        int bk = (int)(e->h & (uint32_t)mask); /* use stored hash */
        e->next = new_b[bk]; new_b[bk] = i;
    }
    free(ds->buckets); ds->buckets = new_b; ds->nbuckets = new_nb;
    return 1;
}

FORCE_INLINE int dedup_find(DedupState *ds, uint8_t tag, const uint8_t *d, Py_ssize_t n) {
    if (!ds->table) return -1;
    uint32_t h = fnv1a(tag, d, n);
    int idx = ds->buckets[h & (uint32_t)(ds->nbuckets-1)];
    while (idx != -1) {
        DEntry *e = &ds->table[idx];
        if (e->h==h && e->tag==tag && e->len==n && memcmp(e->data,d,n)==0) return idx;
        idx = e->next;
    }
    return -1;
}

static int dedup_register(DedupState *ds, uint8_t tag, const uint8_t *d, Py_ssize_t n) {
    if (ds->count >= (int)(ds->nbuckets * HASH_LOAD_MAX))
        if (!dedup_rehash(ds)) return -1;
    if (ds->count >= ds->cap) {
        int new_cap = ds->cap ? ds->cap*2 : DEDUP_CAP_INIT;
        DEntry *t = realloc(ds->table, (size_t)new_cap * sizeof(DEntry));
        if (!t) { PyErr_NoMemory(); return -1; }
        ds->table = t; ds->cap = new_cap;
    }
    uint8_t *copy = arena_alloc(&ds->arena, n);
    if (!copy && n) { PyErr_NoMemory(); return -1; }
    if (n) memcpy(copy, d, n);
    int idx = ds->count;
    ds->table[idx].tag  = tag;
    ds->table[idx].data = copy;
    ds->table[idx].len  = n;
    uint32_t h = fnv1a(tag, d, n);
    ds->table[idx].h = h;
    int bucket = (int)(h & (uint32_t)(ds->nbuckets-1));
    ds->table[idx].next = ds->buckets[bucket];
    ds->buckets[bucket] = idx;
    ds->count++;
    return idx;
}

FORCE_INLINE int dedup_register_obj(DedupState *ds, PyObject *obj, uint8_t tag,
                                    const uint8_t *d, Py_ssize_t n) {
    int idx = dedup_register(ds, tag, d, n);
    if (idx < 0) return -1;
    id_tab_insert(ds, obj, idx);
    return idx;
}

/* ── Encoding helpers ─────────────────────────────────────────────────────── */
FORCE_INLINE int write_len(Buf *b, Py_ssize_t n) {
    if (n <= 0x3F)   return buf_u8(b, (uint8_t)n);
    if (n <= 0x3FFF) return buf_u8_2(b, (uint8_t)(0x40|(n>>8)), (uint8_t)(n&0xFF));
    uint8_t t[5] = {0xFF,(n>>24)&0xFF,(n>>16)&0xFF,(n>>8)&0xFF,n&0xFF};
    return buf_raw(b, t, 5);
}

FORCE_INLINE int write_ref(Buf *b, int idx) {
    if (idx <= 0xFE) return buf_u8_2(b, 0xFE, (uint8_t)idx);
    if (idx <= 0xFFFF) return buf_u8(b,0xFD) && buf_u16be(b,(uint16_t)idx);
    uint8_t t[4] = {(idx>>24)&0xFF,(idx>>16)&0xFF,(idx>>8)&0xFF,idx&0xFF};
    return buf_u8(b,0xFC) && buf_raw(b,t,4);
}

/* _write_bytes_payload_reg: emit new data and register with py-hash dedup */
static int dedup_register_with_pyhash(DedupState *ds, PyObject *obj, uint8_t tag,
                                       const uint8_t *d, Py_ssize_t n);  /* forward decl */

FORCE_INLINE int _write_bytes_payload_reg(Buf *b, DedupState *ds, uint8_t type_tag,
                                           PyObject *obj, const uint8_t *d, Py_ssize_t n) {
    if (type_tag == 0x05 && n <= 0x3F) {
        dedup_register_with_pyhash(ds, obj, type_tag, d, n);
        if (__builtin_expect(b->len + 2 + n <= b->cap, 1)) {
            b->buf[b->len]=0x15; b->buf[b->len+1]=(uint8_t)n;
            memcpy(b->buf+b->len+2, d, n); b->len += 2+n; return 1;
        }
        return buf_u8(b,0x15) && buf_u8(b,(uint8_t)n) && buf_raw(b,d,n);
    }
    if (!buf_u8(b, type_tag)) return 0;
    if (n <= 0x3F) {
        dedup_register_with_pyhash(ds, obj, type_tag, d, n);
        if (__builtin_expect(b->len + 2 + n <= b->cap, 1)) {
            b->buf[b->len]=0x0E; b->buf[b->len+1]=(uint8_t)n;
            memcpy(b->buf+b->len+2, d, n); b->len += 2+n; return 1;
        }
        return buf_u8(b,0x0E) && buf_u8(b,(uint8_t)n) && buf_raw(b,d,n);
    }
    dedup_register_with_pyhash(ds, obj, type_tag, d, n);
    return buf_u8(b,0xFB) && buf_u8(b,type_tag) && write_len(b,n) && buf_raw(b,d,n);
}

/* dedup_pyhash: calcula el hash de bucket a partir del hash Python del objeto.
   Retorna 0 si PyObject_Hash falla (el caller usa FNV como fallback). */
FORCE_INLINE uint32_t dedup_pyhash(PyObject *obj, uint8_t tag) {
    Py_hash_t ph = PyObject_Hash(obj);
    if (__builtin_expect(ph == -1, 0)) { PyErr_Clear(); return 0; }
    return (uint32_t)((uint64_t)(unsigned long long)ph * 2654435761ULL) ^ (uint32_t)tag * 16777619u;
}

/* dedup_find_h: buscar con hash ya calculado — evita recalcular en register */
FORCE_INLINE int dedup_find_h(DedupState *ds, uint8_t tag, const uint8_t *d,
                               Py_ssize_t n, uint32_t h) {
    if (!ds->table) return -1;
    int idx = ds->buckets[h & (uint32_t)(ds->nbuckets - 1)];
    while (idx != -1) {
        DEntry *e = &ds->table[idx];
        if (e->h == h && e->tag == tag && e->len == n && memcmp(e->data, d, n) == 0) return idx;
        idx = e->next;
    }
    return -1;
}

/* dedup_find_with_pyhash: interfaz de compatibilidad */
static int __attribute__((unused)) dedup_find_with_pyhash(DedupState *ds, PyObject *obj, uint8_t tag,
                                   const uint8_t *d, Py_ssize_t n) {
    if (!ds->table) return -1;
    uint32_t h = dedup_pyhash(obj, tag);
    if (!h) return dedup_find(ds, tag, d, n);
    return dedup_find_h(ds, tag, d, n, h);
}

/* dedup_register_h: registrar con hash pre-calculado (evita recalcular) */
static int dedup_register_h(DedupState *ds, PyObject *obj, uint8_t tag,
                             const uint8_t *d, Py_ssize_t n, uint32_t h) {
    if (ds->count >= (int)(ds->nbuckets * HASH_LOAD_MAX))
        if (!dedup_rehash(ds)) return -1;
    if (ds->count >= ds->cap) {
        int new_cap = ds->cap ? ds->cap * 2 : DEDUP_CAP_INIT;
        DEntry *t = realloc(ds->table, (size_t)new_cap * sizeof(DEntry));
        if (!t) { PyErr_NoMemory(); return -1; }
        ds->table = t; ds->cap = new_cap;
    }
    uint8_t *copy = arena_alloc(&ds->arena, n);
    if (!copy && n) { PyErr_NoMemory(); return -1; }
    if (n) memcpy(copy, d, n);
    int idx = ds->count;
    ds->table[idx].tag = tag; ds->table[idx].data = copy;
    ds->table[idx].len = n;
    if (!h) h = fnv1a(tag, d, n);  /* fallback si pyhash falló */
    ds->table[idx].h = h;
    int bucket = (int)(h & (uint32_t)(ds->nbuckets - 1));
    ds->table[idx].next = ds->buckets[bucket];
    ds->buckets[bucket] = idx;
    ds->count++;
    if (obj) id_tab_insert(ds, obj, idx);
    return idx;
}

/* dedup_register_with_pyhash: register using Python hash for bucket placement */
static int dedup_register_with_pyhash(DedupState *ds, PyObject *obj, uint8_t tag,
                                       const uint8_t *d, Py_ssize_t n) {
    if (ds->count >= (int)(ds->nbuckets * HASH_LOAD_MAX))
        if (!dedup_rehash(ds)) return -1;
    if (ds->count >= ds->cap) {
        int new_cap = ds->cap ? ds->cap * 2 : DEDUP_CAP_INIT;
        DEntry *t = realloc(ds->table, (size_t)new_cap * sizeof(DEntry));
        if (!t) { PyErr_NoMemory(); return -1; }
        ds->table = t; ds->cap = new_cap;
    }
    uint8_t *copy = arena_alloc(&ds->arena, n);
    if (!copy && n) { PyErr_NoMemory(); return -1; }
    if (n) memcpy(copy, d, n);
    int idx = ds->count;
    ds->table[idx].tag = tag; ds->table[idx].data = copy;
    ds->table[idx].len = n;
    Py_hash_t ph = PyObject_Hash(obj);
    if (ph == -1) { PyErr_Clear(); ph = (Py_hash_t)fnv1a(tag, d, n); }
    uint32_t h = (uint32_t)((uint64_t)(unsigned long long)ph * 2654435761ULL) ^ (uint32_t)tag * 16777619u;
    ds->table[idx].h = h;  /* store for rehash */
    int bucket = (int)(h & (uint32_t)(ds->nbuckets - 1));
    ds->table[idx].next = ds->buckets[bucket];
    ds->buckets[bucket] = idx;
    ds->count++;
    id_tab_insert(ds, obj, idx);
    return idx;
}

FORCE_INLINE int write_dedup_typed(Buf *b, DedupState *ds, PyObject *obj, uint8_t type_tag,
                                   const uint8_t *d, Py_ssize_t n) {
    /* Fast path 1: identity cache (O(1), sin hash) */
    int idx = id_tab_find(ds, obj);
    if (__builtin_expect(idx >= 0, 0)) return write_ref(b, idx);

    /* Calcular hash UNA sola vez — se reutiliza en find y register */
    uint32_t h = dedup_pyhash(obj, type_tag);
    if (__builtin_expect(h != 0, 1)) {
        /* Fast path 2: buscar con hash pre-calculado */
        idx = dedup_find_h(ds, type_tag, d, n, h);
        if (__builtin_expect(idx >= 0, 0)) {
            id_tab_insert(ds, obj, idx); return write_ref(b, idx);
        }
        /* Nuevo: registrar con el mismo hash (sin recalcular) */
        idx = dedup_register_h(ds, obj, type_tag, d, n, h);
        if (idx < 0) return 0;
    } else {
        /* Fallback FNV (raro: solo si PyObject_Hash devuelve -1) */
        idx = dedup_find(ds, type_tag, d, n);
        if (idx >= 0) { id_tab_insert(ds, obj, idx); return write_ref(b, idx); }
        idx = dedup_register_h(ds, NULL, type_tag, d, n, 0);
        if (idx < 0) return 0;
        id_tab_insert(ds, obj, idx);
    }

    /* Emitir dato nuevo */
    if (type_tag == 0x05 && n <= 0x3F) {
        if (__builtin_expect(b->len + 2 + n <= b->cap, 1)) {
            b->buf[b->len]=0x15; b->buf[b->len+1]=(uint8_t)n;
            memcpy(b->buf+b->len+2, d, n); b->len += 2+n; return 1;
        }
        return buf_u8(b,0x15) && buf_u8(b,(uint8_t)n) && buf_raw(b,d,n);
    }
    if (!buf_u8(b, type_tag)) return 0;
    if (n <= 0x3F) {
        if (__builtin_expect(b->len + 2 + n <= b->cap, 1)) {
            b->buf[b->len]=0x0E; b->buf[b->len+1]=(uint8_t)n;
            memcpy(b->buf+b->len+2, d, n); b->len += 2+n; return 1;
        }
        return buf_u8(b,0x0E) && buf_u8(b,(uint8_t)n) && buf_raw(b,d,n);
    }
    return buf_u8(b,0xFB) && buf_u8(b,type_tag) && write_len(b,n) && buf_raw(b,d,n);
}

FORCE_INLINE int write_dedup(Buf *b, DedupState *ds, uint8_t tag,
                             const uint8_t *d, Py_ssize_t n) {
    int idx = dedup_find(ds, tag, d, n);
    if (__builtin_expect(idx >= 0, 0)) return write_ref(b, idx);
    if (n <= 0x3F) {
        dedup_register(ds, tag, d, n);
        if (__builtin_expect(b->len + 2 + n <= b->cap, 1)) {
            b->buf[b->len]=0x0E; b->buf[b->len+1]=(uint8_t)n;
            memcpy(b->buf+b->len+2, d, n); b->len += 2+n; return 1;
        }
        return buf_u8(b,0x0E) && buf_u8(b,(uint8_t)n) && buf_raw(b,d,n);
    }
    dedup_register(ds, tag, d, n);
    return buf_u8(b,0xFB) && buf_u8(b,tag) && write_len(b,n) && buf_raw(b,d,n);
}

static PyObject *source_cache = NULL;
static PyObject *exec_cache   = NULL;

static PyObject *get_source(PyObject *obj) {
    PyObject *key = PyLong_FromVoidPtr(obj);
    if (!key) return NULL;
    PyObject *cached = PyDict_GetItem(source_cache, key);
    if (cached) { Py_DECREF(key); Py_INCREF(cached); return cached; }
    PyObject *inspect = PyImport_ImportModule("inspect");
    if (!inspect) { Py_DECREF(key); return NULL; }
    PyObject *src_str = PyObject_CallMethod(inspect, "getsource", "O", obj);
    Py_DECREF(inspect);
    if (!src_str) { Py_DECREF(key); return NULL; }
    PyObject *src_bytes = PyUnicode_AsUTF8String(src_str);
    Py_DECREF(src_str);
    if (!src_bytes) { Py_DECREF(key); return NULL; }
    PyDict_SetItem(source_cache, key, src_bytes);
    Py_DECREF(key);
    return src_bytes;
}

static int serialize(Buf *b, DedupState *ds, PyObject *obj);

static int serialize_dict(Buf *b, DedupState *ds, PyObject *d) {
    Py_ssize_t n = PyDict_Size(d);
    if (!buf_u8(b,0x0C) || !write_len(b,n)) return 0;
    PyObject *k, *v; Py_ssize_t pos = 0;
    while (PyDict_Next(d, &pos, &k, &v))
        if (!serialize(b,ds,k) || !serialize(b,ds,v)) return 0;
    return 1;
}

static int write_class_header(Buf *b, DedupState *ds, PyObject *cls) {
    PyObject *cls_name = PyObject_GetAttrString(cls, "__name__");
    if (!cls_name) return 0;
    PyObject *src = get_source(cls);
    if (!src) { Py_DECREF(cls_name); return 0; }
    Py_ssize_t sn = PyBytes_GET_SIZE(src);
    const uint8_t *sd = (const uint8_t*)PyBytes_AS_STRING(src);
    Py_ssize_t nn = 0;
    const char *np = PyUnicode_AsUTF8AndSize(cls_name, &nn);
    if (!np) { Py_DECREF(cls_name); Py_DECREF(src); return 0; }

    /* Emitir nombre de clase */
    if (!write_dedup_typed(b, ds, cls_name, 0x05, (const uint8_t*)np, nn)) {
        Py_DECREF(cls_name); Py_DECREF(src); return 0;
    }
    Py_DECREF(cls_name);

    /* Intentar comprimir source con zlib si es >= 64B y no está en dedup ya */
    int in_dedup = (dedup_find(ds, 0x1D, sd, sn) >= 0);
    int ok;
    if (!in_dedup && sn >= 48) {
        uLongf zbound = compressBound((uLong)sn);
        uint8_t *zbuf = malloc(zbound);
        if (zbuf) {
            uLongf zlen = zbound;
            if (compress2(zbuf, &zlen, sd, (uLong)sn, 1) == Z_OK && zlen < (uLongf)sn) {
                /* Guardar en dedup bajo tag 0x1B (comprimido) */
                ok = write_dedup(b, ds, OP_SRC_Z, zbuf, (Py_ssize_t)zlen);
                /* También registrar raw bajo 0x1D para que refs futuras funcionen */
                dedup_register(ds, 0x1D, sd, sn);
                free(zbuf); Py_DECREF(src); return ok;
            }
            free(zbuf);
        }
    }
    /* Fallback: raw sin comprimir */
    ok = write_dedup(b, ds, 0x1D, sd, sn);
    Py_DECREF(src); return ok;
}

static int serialize_slots(Buf *b, DedupState *ds, PyObject *obj, PyObject *cls);
static int has_custom_reduce(PyObject *obj);
static int serialize_via_reduce(Buf *b, DedupState *ds, PyObject *obj);
static int serialize_callable_ref(Buf *b, DedupState *ds, PyObject *callable);

static int serialize(Buf *b, DedupState *ds, PyObject *obj) {
    if (obj == Py_None)    return buf_u8(b, 0x00);
    if (PyBool_Check(obj)) return buf_u8(b, obj==Py_True ? 0x81 : 0x80);

    if (PyLong_Check(obj)) {
        int overflow;
        long v = PyLong_AsLongAndOverflow(obj, &overflow);
        if (!overflow) {
            if (v >= 0 && v <= 58)      return buf_u8(b, (uint8_t)(0xC0+v));
            if (v >= 59 && v <= 254)    return buf_u8_2(b, OP_UINT8, (uint8_t)v);
            if (v == 255)               return buf_u8_2(b, OP_UINT8, 0xFF);
            if (v >= 256 && v <=0xFFFF) return buf_u8(b,0x0F) && buf_u16be(b,(uint16_t)v);
            if (v >= -30 && v < 0)      return buf_u8_2(b, 0x10, (uint8_t)(-v-1));
            if (v >= -256 && v < -30)   return buf_u8_2(b, OP_NEG8, (uint8_t)(-v-1));
        }
        int neg = (!overflow && v<0) ? 1 : (overflow==-1 ? 1 : 0);
        PyObject *absv = PyNumber_Absolute(obj); if (!absv) return 0;
        PyObject *bl = PyObject_CallMethod(absv,"bit_length",NULL);
        if (!bl) { Py_DECREF(absv); return 0; }
        long bits = PyLong_AsLong(bl); Py_DECREF(bl);
        if (bits<0 && PyErr_Occurred()) { Py_DECREF(absv); return 0; }
        Py_ssize_t nbytes = (bits+7)/8; if (!nbytes) nbytes=1;
        uint8_t *tmp = malloc(nbytes);
        if (!tmp) { Py_DECREF(absv); PyErr_NoMemory(); return 0; }
#if PY_VERSION_HEX >= 0x030d0000
        if (_PyLong_AsByteArray((PyLongObject*)absv, tmp, (size_t)nbytes, 0, 0, 1)<0) {
#else
        if (_PyLong_AsByteArray((PyLongObject*)absv, tmp, (size_t)nbytes, 0, 0)<0) {
#endif
            free(tmp); Py_DECREF(absv); return 0;
        }
        Py_DECREF(absv);
        int ok = buf_u8(b,0x02)&&buf_u8(b,neg?1:0)&&write_len(b,nbytes)&&buf_raw(b,tmp,nbytes);
        free(tmp); return ok;
    }

    if (PyFloat_Check(obj)) {
        double v = PyFloat_AS_DOUBLE(obj);
        if (v==0.0)  { uint64_t bits; memcpy(&bits,&v,8); return buf_u8(b, bits>>63?0x83:0x82); }
        if (v==1.0)  return buf_u8(b,0x84);
        if (v==-1.0) return buf_u8(b,0x85);
        if (v!=v)    return buf_u8(b,0x86);
        if (v==(1.0/0.0))  return buf_u8(b,0x87);
        if (v==(-1.0/0.0)) return buf_u8(b,0x88);
        uint64_t bits; memcpy(&bits,&v,8);
        return buf_u8(b,0x03) && buf_u64be(b,bits);
    }

    if (PyUnicode_Check(obj)) {
        /* Fast path for ASCII/Latin1 compact strings (most dict keys are ASCII).
           PyUnicode_1BYTE_KIND + IS_ASCII means UTF-8 == raw buffer, no conversion. */
        if (PyUnicode_IS_READY(obj) && PyUnicode_KIND(obj) == PyUnicode_1BYTE_KIND
            && PyUnicode_IS_ASCII(obj)) {
            Py_ssize_t n = PyUnicode_GET_LENGTH(obj);
            const uint8_t *s = (const uint8_t*)PyUnicode_1BYTE_DATA(obj);
            return write_dedup_typed(b, ds, obj, 0x05, s, n);
        }
        Py_ssize_t n; const char *s = PyUnicode_AsUTF8AndSize(obj, &n);
        if (!s) return 0;
        return write_dedup_typed(b, ds, obj, 0x05, (const uint8_t*)s, n);
    }

    if (PyList_Check(obj)) {
        Py_ssize_t n = PyList_GET_SIZE(obj);
        if (!buf_u8(b,0x08)||!write_len(b,n)) return 0;
        for (Py_ssize_t i=0;i<n;i++)
            if (!serialize(b,ds,PyList_GET_ITEM(obj,i))) return 0;
        return 1;
    }

    if (PyDict_Check(obj)) return serialize_dict(b, ds, obj);

    if (PyBytes_Check(obj)) {
        Py_ssize_t n = PyBytes_GET_SIZE(obj);
        return write_dedup_typed(b, ds, obj, 0x06, (const uint8_t*)PyBytes_AS_STRING(obj), n);
    }

    if (PyTuple_Check(obj)) {
        Py_ssize_t n = PyTuple_GET_SIZE(obj);
        if (!buf_u8(b,0x09)||!write_len(b,n)) return 0;
        for (Py_ssize_t i=0;i<n;i++)
            if (!serialize(b,ds,PyTuple_GET_ITEM(obj,i))) return 0;
        return 1;
    }

    if (PyComplex_Check(obj)) {
        double r=PyComplex_RealAsDouble(obj), im=PyComplex_ImagAsDouble(obj);
        uint64_t rb,ib; memcpy(&rb,&r,8); memcpy(&ib,&im,8);
        return buf_u8(b,0x04)&&buf_u64be(b,rb)&&buf_u64be(b,ib);
    }

    if (PyByteArray_Check(obj)) {
        Py_ssize_t n = PyByteArray_GET_SIZE(obj);
        return write_dedup_typed(b, ds, obj, 0x07, (const uint8_t*)PyByteArray_AS_STRING(obj), n);
    }

    if (PySet_Check(obj)||PyFrozenSet_Check(obj)) {
        PyObject *lst = PySequence_List(obj); if (!lst) return 0;
        Py_ssize_t n = PyList_GET_SIZE(lst);
        PyObject *pairs = PyList_New(n); if (!pairs) { Py_DECREF(lst); return 0; }
        for (Py_ssize_t i=0;i<n;i++) {
            PyObject *item=PyList_GET_ITEM(lst,i);
            PyObject *rs=PyObject_Repr(item); if (!rs) { Py_DECREF(pairs); Py_DECREF(lst); return 0; }
            PyObject *pair=PyTuple_Pack(2,rs,item); Py_DECREF(rs);
            if (!pair) { Py_DECREF(pairs); Py_DECREF(lst); return 0; }
            PyList_SET_ITEM(pairs,i,pair);
        }
        Py_DECREF(lst);
        if (PyList_Sort(pairs)<0) { Py_DECREF(pairs); return 0; }
        uint8_t stag = PyFrozenSet_Check(obj)?0x0B:0x0A;
        if (!buf_u8(b,stag)||!write_len(b,n)) { Py_DECREF(pairs); return 0; }
        for (Py_ssize_t i=0;i<n;i++) {
            PyObject *item=PyTuple_GET_ITEM(PyList_GET_ITEM(pairs,i),1);
            if (!serialize(b,ds,item)) { Py_DECREF(pairs); return 0; }
        }
        Py_DECREF(pairs); return 1;
    }

    if (PyFunction_Check(obj)) {
        PyObject *code = PyFunction_GetCode(obj); if (!code) return 0;
        PyObject *marshal_mod = PyImport_ImportModule("marshal"); if (!marshal_mod) return 0;
        PyObject *marshalled = PyObject_CallMethod(marshal_mod,"dumps","O",code);
        Py_DECREF(marshal_mod); if (!marshalled) return 0;
        Py_ssize_t msz=PyBytes_GET_SIZE(marshalled);
        const uint8_t *mdata=(const uint8_t*)PyBytes_AS_STRING(marshalled);
        if (!buf_u8(b,0x20)||!write_dedup(b,ds,0x20,mdata,msz)) { Py_DECREF(marshalled); return 0; }
        Py_DECREF(marshalled);
        PyObject *defaults=PyFunction_GetDefaults(obj);
        Py_ssize_t nd=(defaults&&PyTuple_Check(defaults))?PyTuple_GET_SIZE(defaults):0;
        if (!write_len(b,nd)) return 0;
        for (Py_ssize_t i=0;i<nd;i++)
            if (!serialize(b,ds,PyTuple_GET_ITEM(defaults,i))) return 0;
        PyObject *closure=PyFunction_GetClosure(obj);
        Py_ssize_t nc=0;
        if (closure&&PyTuple_Check(closure)) nc=PyTuple_GET_SIZE(closure);
        if (!write_len(b,nc)) return 0;
        for (Py_ssize_t i=0;i<nc;i++) {
            PyObject *cell=PyTuple_GET_ITEM(closure,i);
            PyObject *cv=PyCell_Get(cell);
            if (!cv) { PyErr_Clear(); if (!buf_u8(b,0x00)) return 0; }
            else { int ok=serialize(b,ds,cv); Py_DECREF(cv); if (!ok) return 0; }
        }
        return 1;
    }

    if (PyCoro_CheckExact(obj)||PyGen_CheckExact(obj)) {
        PyObject *lst=PySequence_List(obj); if (!lst) return 0;
        Py_ssize_t n=PyList_GET_SIZE(lst);
        if (!buf_u8(b,0x08)||!write_len(b,n)) { Py_DECREF(lst); return 0; }
        for (Py_ssize_t i=0;i<n;i++)
            if (!serialize(b,ds,PyList_GET_ITEM(lst,i))) { Py_DECREF(lst); return 0; }
        Py_DECREF(lst); return 1;
    }

    if (has_custom_reduce(obj)) return serialize_via_reduce(b, ds, obj);

    PyObject *dunder_dict = PyObject_GetAttrString(obj,"__dict__");
    if (dunder_dict) {
        PyObject *cls=PyObject_GetAttrString(obj,"__class__");
        if (!cls) { Py_DECREF(dunder_dict); return 0; }
        int ok=buf_u8(b,0x1C)&&write_class_header(b,ds,cls)&&serialize_dict(b,ds,dunder_dict);
        Py_DECREF(dunder_dict); Py_DECREF(cls); return ok;
    }
    PyErr_Clear();

    PyObject *cls=PyObject_GetAttrString(obj,"__class__");
    if (cls) {
        PyObject *hs=PyObject_GetAttrString(cls,"__slots__");
        if (hs) { Py_DECREF(hs); int ok=serialize_slots(b,ds,obj,cls); Py_DECREF(cls); return ok; }
        PyErr_Clear(); Py_DECREF(cls);
    }
    PyErr_Clear();
    PyErr_Format(PyExc_TypeError,"Tipo no soportado: %R",PyObject_Type(obj));
    return 0;
}


static int serialize_slots(Buf *b, DedupState *ds, PyObject *obj, PyObject *cls) {
    PyObject *slot_names=PyList_New(0); if (!slot_names) return 0;
    PyObject *mro=PyObject_GetAttrString(cls,"__mro__");
    if (!mro) { Py_DECREF(slot_names); return 0; }
    Py_ssize_t mro_len=PyTuple_GET_SIZE(mro);
    for (Py_ssize_t i=0;i<mro_len;i++) {
        PyObject *base=PyTuple_GET_ITEM(mro,i);
        PyObject *slots=PyObject_GetAttrString(base,"__slots__");
        if (!slots) { PyErr_Clear(); continue; }
        if (PyUnicode_Check(slots)) PyList_Append(slot_names,slots);
        else {
            PyObject *iter=PyObject_GetIter(slots);
            if (iter) {
                PyObject *item;
                while ((item=PyIter_Next(iter))) { PyList_Append(slot_names,item); Py_DECREF(item); }
                Py_DECREF(iter);
            }
        }
        Py_DECREF(slots);
    }
    Py_DECREF(mro);
    PyObject *values=PyDict_New(); if (!values) { Py_DECREF(slot_names); return 0; }
    Py_ssize_t n=PyList_GET_SIZE(slot_names);
    for (Py_ssize_t i=0;i<n;i++) {
        PyObject *name=PyList_GET_ITEM(slot_names,i);
        PyObject *val=PyObject_GetAttr(obj,name);
        if (val) { PyDict_SetItem(values,name,val); Py_DECREF(val); } else PyErr_Clear();
    }
    Py_DECREF(slot_names);
    int ok=buf_u8(b,0x1E)&&write_class_header(b,ds,cls)&&serialize_dict(b,ds,values);
    Py_DECREF(values); return ok;
}

static int has_custom_reduce(PyObject *obj) {
    PyObject *tp=(PyObject*)Py_TYPE(obj);
    PyObject *mro=PyObject_GetAttrString(tp,"__mro__");
    if (!mro) { PyErr_Clear(); return 0; }
    static PyObject *reduce_str=NULL;
    if (!reduce_str) reduce_str=PyUnicode_InternFromString("__reduce__");
    int found=0;
    Py_ssize_t mlen=PyTuple_GET_SIZE(mro);
    for (Py_ssize_t i=0;i<mlen;i++) {
        PyObject *base=PyTuple_GET_ITEM(mro,i);
        if (base==(PyObject*)&PyBaseObject_Type) break;
        PyObject *d=PyObject_GetAttrString(base,"__dict__");
        if (!d) { PyErr_Clear(); continue; }
        PyObject *r=PyObject_GetItem(d,reduce_str); Py_DECREF(d);
        if (r) { Py_DECREF(r); found=1; break; }
        PyErr_Clear();
    }
    Py_DECREF(mro); return found;
}

static int serialize_callable_ref(Buf *b, DedupState *ds, PyObject *callable) {
    PyObject *name=PyObject_GetAttrString(callable,"__name__");
    if (!name) { PyErr_Clear(); return 0; }
    Py_ssize_t nn=0; const char *np=PyUnicode_AsUTF8AndSize(name,&nn);
    if (!np) { Py_DECREF(name); return 0; }
    PyObject *src=get_source(callable);
    if (!src) {
        PyErr_Clear();
        int ok=write_dedup_typed(b,ds,name,0x05,(const uint8_t*)np,nn)&&buf_u8(b,0x00);
        Py_DECREF(name); return ok;
    }
    Py_ssize_t sn=PyBytes_GET_SIZE(src);
    const uint8_t *sd=(const uint8_t*)PyBytes_AS_STRING(src);
    int ok=write_dedup_typed(b,ds,name,0x05,(const uint8_t*)np,nn)&&buf_u8(b,0x01)
        && write_dedup(b,ds,0x1D,sd,sn);
    Py_DECREF(name); Py_DECREF(src); return ok;
}

static int serialize_via_reduce(Buf *b, DedupState *ds, PyObject *obj) {
    PyObject *reduced=PyObject_CallMethod(obj,"__reduce__",NULL); if (!reduced) return 0;
    Py_ssize_t rlen=PyTuple_Check(reduced)?PyTuple_GET_SIZE(reduced):-1;
    if (rlen<2||rlen>5) {
        Py_DECREF(reduced);
        PyErr_SetString(PyExc_TypeError,"__reduce__ debe devolver una tupla de 2 a 5 elementos");
        return 0;
    }
    PyObject *callable=PyTuple_GET_ITEM(reduced,0);
    PyObject *args=PyTuple_GET_ITEM(reduced,1);
    PyObject *state=(rlen>=3)?PyTuple_GET_ITEM(reduced,2):NULL;
    PyObject *list_items=(rlen>=4)?PyTuple_GET_ITEM(reduced,3):NULL;
    PyObject *dict_items=(rlen>=5)?PyTuple_GET_ITEM(reduced,4):NULL;
    if (!PyTuple_Check(args)) {
        Py_DECREF(reduced);
        PyErr_SetString(PyExc_TypeError,"__reduce__: args debe ser una tupla"); return 0;
    }
    int hs=(state&&state!=Py_None), hl=(list_items&&list_items!=Py_None), hd=(dict_items&&dict_items!=Py_None);
    uint8_t flags=(hs?1:0)|(hl?2:0)|(hd?4:0);
    int ok=buf_u8(b,0x1F)&&serialize_callable_ref(b,ds,callable)&&serialize(b,ds,args)&&buf_u8(b,flags);
    if (ok&&hs) ok=serialize(b,ds,state);
    if (ok&&hl) {
        PyObject *il=PySequence_List(list_items); if (!il) { Py_DECREF(reduced); return 0; }
        ok=serialize(b,ds,il); Py_DECREF(il);
    }
    if (ok&&hd) {
        PyObject *id=PyDict_New(); if (!id) { Py_DECREF(reduced); return 0; }
        PyObject *iter=PyObject_GetIter(dict_items);
        if (!iter) { Py_DECREF(id); Py_DECREF(reduced); return 0; }
        PyObject *pair;
        while ((pair=PyIter_Next(iter))) {
            if (!PyTuple_Check(pair)||PyTuple_GET_SIZE(pair)!=2) {
                Py_DECREF(pair); Py_DECREF(iter); Py_DECREF(id); Py_DECREF(reduced);
                PyErr_SetString(PyExc_TypeError,"__reduce__ dict_items: cada elemento debe ser (key, value)");
                return 0;
            }
            PyDict_SetItem(id,PyTuple_GET_ITEM(pair,0),PyTuple_GET_ITEM(pair,1));
            Py_DECREF(pair);
        }
        Py_DECREF(iter); ok=serialize(b,ds,id); Py_DECREF(id);
    }
    Py_DECREF(reduced); return ok;
}

/* ══════════════════════════════════════════════
   DESERIALIZADOR v3
   KEY CHANGE: read_table stores decoded PyObjects directly.
   - Strings: stored as PyUnicode (not PyBytes raw)
   - Bytes: stored as PyBytes
   - ByteArray: stored as PyByteArray
   - Source bytes (0x1D): stored as PyBytes
   This eliminates PyUnicode_FromEncodedObject on every string ref.
   ══════════════════════════════════════════════ */

static PyObject **read_table       = NULL;
static int        read_table_count = 0;
static int        read_table_cap   = 0;

static void read_table_reset(void) {
    for (int i=0;i<read_table_count;i++) Py_XDECREF(read_table[i]);
    read_table_count=0;
}

/* stored_obj: the ready-to-return Python object (may be unicode or bytes).
   For source bytes (0x1D) we need the raw bytes for exec, so we store
   raw bytes there. The type tag in the stream disambiguates at the call site. */
static int read_table_push(PyObject *obj) {
    if (read_table_count >= read_table_cap) {
        int new_cap = read_table_cap ? read_table_cap*2 : 256;
        PyObject **t = realloc(read_table, (size_t)new_cap * sizeof(PyObject*));
        if (!t) { PyErr_NoMemory(); return 0; }
        read_table = t; read_table_cap = new_cap;
    }
    Py_INCREF(obj);
    read_table[read_table_count++] = obj;
    return 1;
}

/* cls_cache: (name_str, src_bytes) → cls directamente.
   Evita PyDict_GetItem en exec_ns + otra llamada para cada instancia. */
static PyObject *cls_cache = NULL;

static PyObject *exec_source(PyObject *src_bytes) {
    PyObject *cached=PyDict_GetItem(exec_cache,src_bytes);
    if (cached) { Py_INCREF(cached); return cached; }
    /* PyUnicode_DecodeUTF8 es más directo que FromEncodedObject */
    const char *sd = PyBytes_AS_STRING(src_bytes);
    Py_ssize_t sn  = PyBytes_GET_SIZE(src_bytes);
    PyObject *src_str = PyUnicode_DecodeUTF8(sd, sn, "strict");
    if (!src_str) return NULL;
    PyObject *ns=PyDict_New(); if (!ns) { Py_DECREF(src_str); return NULL; }
    PyObject *builtins=PyEval_GetBuiltins();
    PyDict_SetItemString(ns,"__builtins__",builtins);
    const char *src_utf8 = PyUnicode_AsUTF8(src_str);
    if (!src_utf8 || PyRun_String(src_utf8,Py_file_input,ns,ns)==NULL) {
        Py_DECREF(src_str); Py_DECREF(ns); return NULL;
    }
    Py_DECREF(src_str);
    PyDict_SetItem(exec_cache,src_bytes,ns);
    return ns;
}

/* read_class_cached: busca clase directamente en cls_cache antes de exec_source */
static PyObject *read_class_direct(PyObject *name_str, PyObject *src_bytes) {
    if (cls_cache) {
        /* Key = (name, src) — ambos interneados/cacheados típicamente */
        PyObject *key = PyTuple_Pack(2, name_str, src_bytes);
        if (key) {
            PyObject *cached = PyDict_GetItem(cls_cache, key);
            if (cached) { Py_INCREF(cached); Py_DECREF(key); return cached; }
            /* No encontrado: exec y cachear */
            PyObject *ns = exec_source(src_bytes);
            if (ns) {
                PyObject *cls = PyDict_GetItem(ns, name_str);
                if (cls) {
                    PyDict_SetItem(cls_cache, key, cls);
                    Py_INCREF(cls);
                }
                Py_DECREF(ns);
                Py_DECREF(key);
                return cls;  /* puede ser NULL si no encontrada */
            }
            Py_DECREF(key);
            return NULL;
        }
        PyErr_Clear();
    }
    /* Fallback sin cls_cache */
    PyObject *ns = exec_source(src_bytes);
    if (!ns) return NULL;
    PyObject *cls = PyDict_GetItem(ns, name_str);
    if (cls) Py_INCREF(cls);
    Py_DECREF(ns);
    return cls;
}

FORCE_INLINE Py_ssize_t read_len(const uint8_t *data, Py_ssize_t size, Py_ssize_t *pos) {
    if (*pos>=size) return -1;
    uint8_t bv=data[(*pos)++];
    if (bv<=0x3F) return bv;
    if ((bv&0xC0)==0x40) {
        if (*pos>=size) return -1;
        return ((Py_ssize_t)(bv&0x3F)<<8)|data[(*pos)++];
    }
    if (*pos+4>size) return -1;
    Py_ssize_t n=((Py_ssize_t)data[*pos]<<24)|((Py_ssize_t)data[*pos+1]<<16)
               | ((Py_ssize_t)data[*pos+2]<<8)|data[*pos+3];
    *pos+=4; return n;
}

/* read_ref_idx: leer índice de referencia (ya se consumió el opcode) */
FORCE_INLINE int read_ref_idx(const uint8_t *data, Py_ssize_t size, Py_ssize_t *pos, uint8_t opcode) {
    if (opcode==0xFE) {
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF ref8"); return -1; }
        return (int)data[(*pos)++];
    }
    if (opcode==0xFD) {
        if (*pos+2>size) { PyErr_SetString(PyExc_ValueError,"EOF ref16"); return -1; }
        int idx=((int)data[*pos]<<8)|data[*pos+1]; *pos+=2; return idx;
    }
    if (*pos+4>size) { PyErr_SetString(PyExc_ValueError,"EOF ref32"); return -1; }
    int idx=((int)data[*pos]<<24)|((int)data[*pos+1]<<16)|((int)data[*pos+2]<<8)|data[*pos+3];
    *pos+=4; return idx;
}

FORCE_INLINE PyObject *read_ref(const uint8_t *data, Py_ssize_t size, Py_ssize_t *pos, uint8_t opcode) {
    int idx=read_ref_idx(data,size,pos,opcode);
    if (idx<0) return NULL;
    if (idx>=read_table_count) {
        PyErr_Format(PyExc_IndexError,"ref idx %d fuera de rango [0,%d)",idx,read_table_count);
        return NULL;
    }
    /* Direct return — object is already decoded (unicode, bytes, etc.) */
    Py_INCREF(read_table[idx]);
    return read_table[idx];
}

/* read_dedup_bytes: reads raw bytes payload and pushes it to read_table as PyBytes.
   Used for source data (0x1D) that exec_source needs as bytes. */
static PyObject *read_dedup_bytes(const uint8_t *data, Py_ssize_t size, Py_ssize_t *pos) {
    if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF en dedup"); return NULL; }
    uint8_t marker=data[(*pos)++];

    if (marker==0xFE||marker==0xFD||marker==0xFC) {
        return read_ref(data,size,pos,marker);
    }
    if (marker==0x0E) {
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF short len"); return NULL; }
        Py_ssize_t n=data[(*pos)++];
        if (*pos+n>size) { PyErr_SetString(PyExc_ValueError,"short str overflow"); return NULL; }
        PyObject *raw=PyBytes_FromStringAndSize((const char*)(data+*pos),n);
        *pos+=n; if (!raw) return NULL;
        if (!read_table_push(raw)) { Py_DECREF(raw); return NULL; }
        return raw;
    }
    if (marker==0xFB) {
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF 0xFB tag"); return NULL; }
        (*pos)++; /* skip stored type tag — caller knows the type */
        Py_ssize_t n=read_len(data,size,pos);
        if (n<0||*pos+n>size) { PyErr_SetString(PyExc_ValueError,"0xFB longitud inválida"); return NULL; }
        PyObject *raw=PyBytes_FromStringAndSize((const char*)(data+*pos),n);
        *pos+=n; if (!raw) return NULL;
        if (!read_table_push(raw)) { Py_DECREF(raw); return NULL; }
        return raw;
    }
    /* OP_SRC_Z: source comprimida — descomprimir y devolver como bytes */
    if (marker==OP_SRC_Z) {
        /* El sub-marker es el payload comprimido via write_dedup con tag OP_SRC_Z */
        /* Recursivo: leer el bloque comprimido */
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF SRC_Z sub"); return NULL; }
        uint8_t sub = data[(*pos)++];
        PyObject *zbytes = NULL;
        if (sub==0xFE||sub==0xFD||sub==0xFC) {
            zbytes = read_ref(data,size,pos,sub);
        } else if (sub==0x0E) {
            if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF SRC_Z len"); return NULL; }
            Py_ssize_t zn = data[(*pos)++];
            if (*pos+zn>size) { PyErr_SetString(PyExc_ValueError,"SRC_Z overflow"); return NULL; }
            zbytes = PyBytes_FromStringAndSize((const char*)(data+*pos), zn);
            *pos += zn;
            if (zbytes) read_table_push(zbytes);
        } else if (sub==0xFB) {
            if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF SRC_Z 0xFB"); return NULL; }
            (*pos)++;
            Py_ssize_t zn = read_len(data,size,pos);
            if (zn<0||*pos+zn>size) { PyErr_SetString(PyExc_ValueError,"SRC_Z 0xFB overflow"); return NULL; }
            zbytes = PyBytes_FromStringAndSize((const char*)(data+*pos), zn);
            *pos += zn;
            if (zbytes) read_table_push(zbytes);
        } else {
            PyErr_Format(PyExc_ValueError,"SRC_Z: sub-marker desconocido 0x%02x",sub);
            return NULL;
        }
        if (!zbytes) return NULL;
        /* Descomprimir */
        const uint8_t *zdata = (const uint8_t*)PyBytes_AS_STRING(zbytes);
        Py_ssize_t zlen = PyBytes_GET_SIZE(zbytes);
        /* Estimar tamaño descomprimido: máx 64KB inicial, expandir si necesario */
        uLongf dest_len = (uLongf)(zlen * 4 + 256);
        uint8_t *dest = NULL;
        int zret = Z_BUF_ERROR;
        for (int tries=0; tries<4 && zret==Z_BUF_ERROR; tries++) {
            free(dest);
            dest = malloc(dest_len);
            if (!dest) { Py_DECREF(zbytes); PyErr_NoMemory(); return NULL; }
            uLongf dl = dest_len;
            zret = uncompress(dest, &dl, zdata, (uLong)zlen);
            if (zret==Z_OK) { dest_len=dl; break; }
            dest_len *= 4;
        }
        Py_DECREF(zbytes);
        if (zret!=Z_OK) { free(dest); PyErr_SetString(PyExc_ValueError,"SRC_Z: fallo zlib decompress"); return NULL; }
        PyObject *raw = PyBytes_FromStringAndSize((const char*)dest, (Py_ssize_t)dest_len);
        free(dest);
        if (!raw) return NULL;
        if (!read_table_push(raw)) { Py_DECREF(raw); return NULL; }
        return raw;
    }

    PyErr_Format(PyExc_ValueError,"Marker dedup_bytes desconocido: 0x%02x",marker);
    return NULL;
}

/* read_str_dedup: reads a string payload and pushes decoded PyUnicode to read_table.
   On ref hit: returns the already-decoded unicode directly (no re-decode). */
static PyObject *read_str_dedup(const uint8_t *data, Py_ssize_t size, Py_ssize_t *pos) {
    if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF str_dedup"); return NULL; }
    uint8_t marker=data[(*pos)++];

    if (marker==0xFE||marker==0xFD||marker==0xFC) {
        return read_ref(data,size,pos,marker);
    }
    if (marker==0x15) {
        /* compact string */
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF 0x15 len"); return NULL; }
        Py_ssize_t n=data[(*pos)++];
        if (*pos+n>size) { PyErr_SetString(PyExc_ValueError,"0x15 overflow"); return NULL; }
        PyObject *s=PyUnicode_FromStringAndSize((const char*)(data+*pos),n);
        *pos+=n; if (!s) return NULL;
        if (!read_table_push(s)) { Py_DECREF(s); return NULL; }
        return s;
    }
    if (marker==0x0E) {
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF short len"); return NULL; }
        Py_ssize_t n=data[(*pos)++];
        if (*pos+n>size) { PyErr_SetString(PyExc_ValueError,"short str overflow"); return NULL; }
        PyObject *s=PyUnicode_FromStringAndSize((const char*)(data+*pos),n);
        *pos+=n; if (!s) return NULL;
        if (!read_table_push(s)) { Py_DECREF(s); return NULL; }
        return s;
    }
    if (marker==0xFB) {
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF 0xFB tag"); return NULL; }
        (*pos)++; /* skip type tag */
        Py_ssize_t n=read_len(data,size,pos);
        if (n<0||*pos+n>size) { PyErr_SetString(PyExc_ValueError,"0xFB str overflow"); return NULL; }
        PyObject *s=PyUnicode_FromStringAndSize((const char*)(data+*pos),n);
        *pos+=n; if (!s) return NULL;
        if (!read_table_push(s)) { Py_DECREF(s); return NULL; }
        return s;
    }
    PyErr_Format(PyExc_ValueError,"str_dedup: marker inesperado 0x%02x",marker);
    return NULL;
}

static PyObject *read_str_any(const uint8_t *data, Py_ssize_t size, Py_ssize_t *pos) {
    if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF str_any"); return NULL; }
    uint8_t tag=data[(*pos)++];
    if (tag==0x15||tag==0x0E||tag==0xFE||tag==0xFD||tag==0xFC||tag==0xFB) {
        /* put back and re-dispatch */
        (*pos)--;
        return read_str_dedup(data,size,pos);
    }
    if (tag==0x05) return read_str_dedup(data,size,pos);
    PyErr_Format(PyExc_ValueError,"read_str_any: tag inesperado 0x%02x",tag);
    return NULL;
}

static PyObject *read_class_header(const uint8_t *data, Py_ssize_t size, Py_ssize_t *pos) {
    PyObject *name_str=read_str_any(data,size,pos); if (!name_str) return NULL;
    PyObject *src_raw=read_dedup_bytes(data,size,pos);
    if (!src_raw) { Py_DECREF(name_str); return NULL; }
    /* Usar cls_cache: evita re-exec si la clase ya fue vista */
    PyObject *cls = read_class_direct(name_str, src_raw);
    Py_DECREF(name_str); Py_DECREF(src_raw);
    if (!cls && !PyErr_Occurred())
        PyErr_SetString(PyExc_KeyError,"clase no encontrada en source ejecutado");
    return cls;
}

static PyObject *read_callable_ref(const uint8_t *data, Py_ssize_t size, Py_ssize_t *pos) {
    PyObject *name_str=read_str_any(data,size,pos); if (!name_str) return NULL;
    if (*pos>=size) { Py_DECREF(name_str); PyErr_SetString(PyExc_ValueError,"EOF source flag"); return NULL; }
    uint8_t has_src=data[(*pos)++];
    if (!has_src) {
        PyObject *builtins=PyEval_GetBuiltins();
        PyObject *fn=PyDict_GetItem(builtins,name_str); Py_DECREF(name_str);
        if (!fn) { PyErr_SetString(PyExc_KeyError,"callable sin source no en builtins"); return NULL; }
        Py_INCREF(fn); return fn;
    }
    PyObject *src_raw=read_dedup_bytes(data,size,pos);
    if (!src_raw) { Py_DECREF(name_str); return NULL; }
    PyObject *ns=exec_source(src_raw); Py_DECREF(src_raw);
    if (!ns) { Py_DECREF(name_str); return NULL; }
    PyObject *fn=PyDict_GetItem(ns,name_str); Py_DECREF(name_str);
    if (!fn) { PyErr_SetString(PyExc_KeyError,"callable no encontrado"); Py_DECREF(ns); return NULL; }
    Py_INCREF(fn); Py_DECREF(ns); return fn;
}

static PyObject *deserialize(const uint8_t *data, Py_ssize_t size, Py_ssize_t *pos);

static PyObject *deserialize_dict(const uint8_t *data, Py_ssize_t size, Py_ssize_t *pos) {
    Py_ssize_t n=read_len(data,size,pos);
    if (n<0) { PyErr_SetString(PyExc_ValueError,"EOF dict len"); return NULL; }
    PyObject *d=PyDict_New(); if (!d) return NULL;
    for (Py_ssize_t i=0;i<n;i++) {
        PyObject *k=deserialize(data,size,pos); if (!k) { Py_DECREF(d); return NULL; }
        PyObject *v=deserialize(data,size,pos);
        if (!v) { Py_DECREF(k); Py_DECREF(d); return NULL; }
        int r=PyDict_SetItem(d,k,v); Py_DECREF(k); Py_DECREF(v);
        if (r<0) { Py_DECREF(d); return NULL; }
    }
    return d;
}

static PyObject *deserialize(const uint8_t *data, Py_ssize_t size, Py_ssize_t *pos) {
    if (__builtin_expect(*pos>=size,0)) { PyErr_SetString(PyExc_ValueError,"EOF"); return NULL; }
    uint8_t tag=data[(*pos)++];

    /* Hot: small ints (range, index, count) */
    if (tag>=0xC0 && tag<=0xFA) return PyLong_FromLong(tag-0xC0);

    /* Hot: None/bool */
    if (tag==0x00) Py_RETURN_NONE;
    if (tag==0x80) Py_RETURN_FALSE;
    if (tag==0x81) Py_RETURN_TRUE;

    /* Hot: compact string (0x15) — most frequent non-scalar */
    if (tag==0x15) {
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF 0x15 len"); return NULL; }
        Py_ssize_t n=data[(*pos)++];
        if (*pos+n>size) { PyErr_SetString(PyExc_ValueError,"0x15 overflow"); return NULL; }
        /* PyUnicode_FromStringAndSize: faster than FromEncodedObject for pure UTF-8 */
        PyObject *s=PyUnicode_FromStringAndSize((const char*)(data+*pos),n);
        *pos+=n; if (!s) return NULL;
        if (!read_table_push(s)) { Py_DECREF(s); return NULL; }
        return s;
    }

    /* Hot: refs — no decoding, just incref and return stored decoded object */
    if (tag==0xFE||tag==0xFD||tag==0xFC) {
        return read_ref(data,size,pos,tag);
    }

    /* float common cases */
    if (tag==0x82) return PyFloat_FromDouble(0.0);
    if (tag==0x83) { double v=-0.0; return PyFloat_FromDouble(v); }
    if (tag==0x84) return PyFloat_FromDouble(1.0);
    if (tag==0x85) return PyFloat_FromDouble(-1.0);
    if (tag==0x86) return PyFloat_FromDouble((double)(0.0/0.0));
    if (tag==0x87) return PyFloat_FromDouble((double)(1.0/0.0));
    if (tag==0x88) return PyFloat_FromDouble((double)(-1.0/0.0));

    /* int 16-bit */
    if (tag==0x0F) {
        if (*pos+2>size) { PyErr_SetString(PyExc_ValueError,"EOF int16"); return NULL; }
        uint16_t v=((uint16_t)data[*pos]<<8)|data[*pos+1]; *pos+=2;
        return PyLong_FromLong((long)v);
    }

    /* neg int small -1..-30 */
    if (tag==0x10) {
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF neg int"); return NULL; }
        return PyLong_FromLong(-(long)(data[(*pos)++]+1));
    }

    /* OP_UINT8: int 59-255 (2B) */
    if (tag==OP_UINT8) {
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF UINT8"); return NULL; }
        return PyLong_FromLong((long)data[(*pos)++]);
    }

    /* OP_NEG8: int -31..-256 (2B, guarda abs-1) */
    if (tag==OP_NEG8) {
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF NEG8"); return NULL; }
        return PyLong_FromLong(-(long)(data[(*pos)++]+1));
    }

    /* OP_SRC_Z: source zlib-comprimida (en dedup) */
    if (tag==OP_SRC_Z) {
        /* Solo aparece en write_dedup context — no debería verse como top-level,
           pero si aparece lo manejamos igual que 0x1D (bytes raw) */
        PyObject *raw=read_dedup_bytes(data,size,pos); if (!raw) return NULL;
        return raw;
    }

    /* float 64-bit */
    if (tag==0x03) {
        if (*pos+8>size) { PyErr_SetString(PyExc_ValueError,"EOF float"); return NULL; }
        uint64_t bits=((uint64_t)data[*pos]<<56)|((uint64_t)data[*pos+1]<<48)
                    | ((uint64_t)data[*pos+2]<<40)|((uint64_t)data[*pos+3]<<32)
                    | ((uint64_t)data[*pos+4]<<24)|((uint64_t)data[*pos+5]<<16)
                    | ((uint64_t)data[*pos+6]<<8)|data[*pos+7];
        *pos+=8; double v; memcpy(&v,&bits,8); return PyFloat_FromDouble(v);
    }

    /* long string via dedup — push as decoded unicode */
    if (tag==0x05) {
        /* put back pos-1 and call read_str_dedup which handles 0x0E/0xFB sub-markers */
        (*pos)--;
        /* Actually 0x05 has already been consumed as the outer type tag.
           In the stream: [0x05][sub-marker: 0x0E or 0xFB][...data...]
           read_str_dedup expects to read the sub-marker itself, so just call it. */
        (*pos)++; /* re-consume 0x05, it was already consumed above */
        return read_str_dedup(data,size,pos);
    }

    /* bytes */
    if (tag==0x06) {
        PyObject *raw=read_dedup_bytes(data,size,pos); if (!raw) return NULL;
        /* read_dedup_bytes already pushed to table; but we need bytes, not unicode */
        /* raw is already PyBytes here since read_dedup_bytes returns PyBytes */
        return raw;
    }

    /* bytearray */
    if (tag==0x07) {
        PyObject *raw=read_dedup_bytes(data,size,pos); if (!raw) return NULL;
        PyObject *ba=PyByteArray_FromStringAndSize(PyBytes_AS_STRING(raw),PyBytes_GET_SIZE(raw));
        Py_DECREF(raw); return ba;
    }

    /* list/tuple/set/frozenset */
    if (tag==0x08||tag==0x09||tag==0x0A||tag==0x0B) {
        Py_ssize_t n=read_len(data,size,pos);
        if (n<0) { PyErr_SetString(PyExc_ValueError,"EOF seq len"); return NULL; }
        PyObject *list=PyList_New(n); if (!list) return NULL;
        for (Py_ssize_t i=0;i<n;i++) {
            PyObject *item=deserialize(data,size,pos);
            if (!item) { Py_DECREF(list); return NULL; }
            PyList_SET_ITEM(list,i,item);
        }
        if (tag==0x08) return list;
        PyObject *res;
        if      (tag==0x09) res=PyList_AsTuple(list);
        else if (tag==0x0A) res=PySet_New(list);
        else                res=PyFrozenSet_New(list);
        Py_DECREF(list); return res;
    }

    /* dict */
    if (tag==0x0C) return deserialize_dict(data,size,pos);

    /* complex */
    if (tag==0x04) {
        if (*pos+16>size) { PyErr_SetString(PyExc_ValueError,"EOF complex"); return NULL; }
        uint64_t rb=0,ib=0;
        for(int i=0;i<8;i++) rb=(rb<<8)|data[*pos+i];
        for(int i=0;i<8;i++) ib=(ib<<8)|data[*pos+8+i];
        *pos+=16; double r,im; memcpy(&r,&rb,8); memcpy(&im,&ib,8);
        return PyComplex_FromDoubles(r,im);
    }

    /* big int */
    if (tag==0x02) {
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF int sign"); return NULL; }
        int neg=data[(*pos)++];
        Py_ssize_t n=read_len(data,size,pos);
        if (n<0||*pos+n>size) { PyErr_SetString(PyExc_ValueError,"EOF int data"); return NULL; }
        PyObject *res=_PyLong_FromByteArray(data+*pos,(size_t)n,0,0);
        if (!res) return NULL;
        *pos += n;
        if (neg) { PyObject *nr=PyNumber_Negative(res); Py_DECREF(res); return nr; }
        return res;
    }

    /* instance with __dict__ */
    if (tag==0x1C) {
        PyObject *cls=read_class_header(data,size,pos); if (!cls) return NULL;
        if (*pos>=size||data[(*pos)++]!=0x0C) {
            PyErr_SetString(PyExc_ValueError,"0x1C: esperado 0x0C"); Py_DECREF(cls); return NULL;
        }
        PyObject *dd=deserialize_dict(data,size,pos);
        if (!dd) { Py_DECREF(cls); return NULL; }
        PyObject *obj=PyObject_CallMethod(cls,"__new__","O",cls); Py_DECREF(cls);
        if (!obj) { Py_DECREF(dd); return NULL; }
        PyObject *od=PyObject_GetAttrString(obj,"__dict__");
        if (od) { if (PyDict_Update(od,dd)<0) { Py_DECREF(od); Py_DECREF(dd); Py_DECREF(obj); return NULL; } Py_DECREF(od); }
        else PyErr_Clear();
        Py_DECREF(dd); return obj;
    }

    /* instance with __slots__ */
    if (tag==0x1E) {
        PyObject *cls=read_class_header(data,size,pos); if (!cls) return NULL;
        if (*pos>=size||data[(*pos)++]!=0x0C) {
            PyErr_SetString(PyExc_ValueError,"0x1E: esperado 0x0C"); Py_DECREF(cls); return NULL;
        }
        PyObject *values=deserialize_dict(data,size,pos);
        if (!values) { Py_DECREF(cls); return NULL; }
        PyObject *obj=PyObject_CallMethod(cls,"__new__","O",cls); Py_DECREF(cls);
        if (!obj) { Py_DECREF(values); return NULL; }
        PyObject *k,*v; Py_ssize_t p=0;
        while (PyDict_Next(values,&p,&k,&v))
            if (PyObject_SetAttr(obj,k,v)<0) { Py_DECREF(values); Py_DECREF(obj); return NULL; }
        Py_DECREF(values); return obj;
    }

    /* reduce protocol */
    if (tag==0x1F) {
        PyObject *callable=read_callable_ref(data,size,pos); if (!callable) return NULL;
        if (*pos>=size||data[*pos]!=0x09) {
            PyErr_SetString(PyExc_ValueError,"0x1F: esperaba tupla args"); Py_DECREF(callable); return NULL;
        }
        PyObject *args=deserialize(data,size,pos);
        if (!args) { Py_DECREF(callable); return NULL; }
        PyObject *obj=PyObject_CallObject(callable,args); Py_DECREF(callable); Py_DECREF(args);
        if (!obj) return NULL;
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF reduce flags"); Py_DECREF(obj); return NULL; }
        uint8_t flags=data[(*pos)++];
        if (flags&1) {
            PyObject *state=deserialize(data,size,pos);
            if (!state) { Py_DECREF(obj); return NULL; }
            if (PyDict_Check(state)) {
                PyObject *od=PyObject_GetAttrString(obj,"__dict__");
                if (od) { if (PyDict_Update(od,state)<0) { Py_DECREF(od); Py_DECREF(state); Py_DECREF(obj); return NULL; } Py_DECREF(od); }
                else { PyErr_Clear(); PyObject *k,*v; Py_ssize_t p=0;
                    while (PyDict_Next(state,&p,&k,&v)) if (PyObject_SetAttr(obj,k,v)<0) { Py_DECREF(state); Py_DECREF(obj); return NULL; } }
            }
            Py_DECREF(state);
        }
        if (flags&2) {
            PyObject *items=deserialize(data,size,pos); if (!items) { Py_DECREF(obj); return NULL; }
            PyObject *ext=PyObject_GetAttrString(obj,"extend");
            if (ext) { PyObject *r=PyObject_CallOneArg(ext,items); Py_DECREF(ext); if (!r) { Py_DECREF(items); Py_DECREF(obj); return NULL; } Py_DECREF(r); }
            else { PyErr_Clear(); PyObject *ap=PyObject_GetAttrString(obj,"append");
                if (!ap) { Py_DECREF(items); Py_DECREF(obj); return NULL; }
                Py_ssize_t ni=PyList_Check(items)?PyList_GET_SIZE(items):0;
                for (Py_ssize_t i=0;i<ni;i++) { PyObject *r=PyObject_CallOneArg(ap,PyList_GET_ITEM(items,i)); if (!r) { Py_DECREF(ap); Py_DECREF(items); Py_DECREF(obj); return NULL; } Py_DECREF(r); }
                Py_DECREF(ap); }
            Py_DECREF(items);
        }
        if (flags&4) {
            PyObject *dct=deserialize(data,size,pos); if (!dct) { Py_DECREF(obj); return NULL; }
            PyObject *upd=PyObject_GetAttrString(obj,"update");
            if (upd) { PyObject *r=PyObject_CallOneArg(upd,dct); Py_DECREF(upd); if (!r) { Py_DECREF(dct); Py_DECREF(obj); return NULL; } Py_DECREF(r); }
            else { PyErr_Clear(); PyObject *k,*v; Py_ssize_t p=0;
                while (PyDict_Next(dct,&p,&k,&v)) if (PyObject_SetItem(obj,k,v)<0) { Py_DECREF(dct); Py_DECREF(obj); return NULL; } }
            Py_DECREF(dct);
        }
        return obj;
    }

    /* function via marshal */
    if (tag==0x20) {
        PyObject *marshal_raw=read_dedup_bytes(data,size,pos); if (!marshal_raw) return NULL;
        PyObject *marshal_mod=PyImport_ImportModule("marshal");
        if (!marshal_mod) { Py_DECREF(marshal_raw); return NULL; }
        PyObject *code=PyObject_CallMethod(marshal_mod,"loads","O",marshal_raw);
        Py_DECREF(marshal_mod); Py_DECREF(marshal_raw); if (!code) return NULL;
        Py_ssize_t nd=read_len(data,size,pos);
        if (nd<0) { Py_DECREF(code); PyErr_SetString(PyExc_ValueError,"EOF fn defaults"); return NULL; }
        PyObject *defaults=nd>0?PyTuple_New(nd):NULL;
        for (Py_ssize_t i=0;i<nd;i++) {
            PyObject *dv=deserialize(data,size,pos);
            if (!dv) { Py_XDECREF(defaults); Py_DECREF(code); return NULL; }
            PyTuple_SET_ITEM(defaults,i,dv);
        }
        Py_ssize_t nc=read_len(data,size,pos);
        if (nc<0) { Py_XDECREF(defaults); Py_DECREF(code); PyErr_SetString(PyExc_ValueError,"EOF fn freevars"); return NULL; }
        PyObject *closure=NULL;
        if (nc>0) {
            closure=PyTuple_New(nc); if (!closure) { Py_XDECREF(defaults); Py_DECREF(code); return NULL; }
            for (Py_ssize_t i=0;i<nc;i++) {
                PyObject *val=deserialize(data,size,pos);
                if (!val) { Py_DECREF(closure); Py_XDECREF(defaults); Py_DECREF(code); return NULL; }
                PyObject *cm=PyRun_String("lambda v: (lambda: v).__closure__[0]",Py_eval_input,PyEval_GetBuiltins(),PyEval_GetBuiltins());
                PyObject *cell=cm?PyObject_CallOneArg(cm,val):NULL; Py_XDECREF(cm); Py_DECREF(val);
                if (!cell) { Py_DECREF(closure); Py_XDECREF(defaults); Py_DECREF(code); return NULL; }
                PyTuple_SET_ITEM(closure,i,cell);
            }
        }
        PyObject *bm=PyImport_ImportModule("builtins");
        PyObject *globals=bm?PyModule_GetDict(bm):PyDict_New(); Py_XDECREF(bm);
        PyObject *fn=PyFunction_New(code,globals); Py_DECREF(code); Py_XDECREF(globals);
        if (!fn) { Py_XDECREF(defaults); Py_XDECREF(closure); return NULL; }
        if (defaults) { PyFunction_SetDefaults(fn,defaults); Py_DECREF(defaults); }
        if (closure)  { PyFunction_SetClosure(fn,closure);   Py_DECREF(closure); }
        return fn;
    }

    /* legacy function (0x0D) */
    if (tag==0x0D) {
        if (*pos>=size) { PyErr_SetString(PyExc_ValueError,"EOF fn name tag"); return NULL; }
        uint8_t ntag=data[(*pos)++];
        if (ntag!=0x05) { PyErr_SetString(PyExc_ValueError,"0x0D: esperado 0x05"); return NULL; }
        PyObject *name_raw=read_dedup_bytes(data,size,pos); if (!name_raw) return NULL;
        PyObject *src_raw=read_dedup_bytes(data,size,pos);
        if (!src_raw) { Py_DECREF(name_raw); return NULL; }
        PyObject *ns=exec_source(src_raw); Py_DECREF(src_raw);
        if (!ns) { Py_DECREF(name_raw); return NULL; }
        PyObject *name_str=PyUnicode_FromEncodedObject(name_raw,"utf-8","strict"); Py_DECREF(name_raw);
        if (!name_str) { Py_DECREF(ns); return NULL; }
        PyObject *fn=PyDict_GetItem(ns,name_str); Py_DECREF(name_str);
        if (!fn) { PyErr_SetString(PyExc_KeyError,"función no encontrada"); Py_DECREF(ns); return NULL; }
        Py_INCREF(fn); Py_DECREF(ns); return fn;
    }

    PyErr_Format(PyExc_ValueError,"Tag desconocido: 0x%02x",tag);
    return NULL;
}

/* ── API pública ── */
static PyObject *py_serialize_fast(PyObject *self, PyObject *args) {
    PyObject *obj;
    if (!PyArg_ParseTuple(args,"O",&obj)) return NULL;
    DedupState ds;
    if (!dedup_init(&ds)) return NULL;
    /* Heurística de tamaño inicial según tipo: evita realloc en objetos medianos */
    Py_ssize_t init_cap = 512;
    if (PyList_Check(obj)||PyTuple_Check(obj)) {
        Py_ssize_t n = PySequence_Size(obj);
        if (n > 64) init_cap = n * 6;
    } else if (PyDict_Check(obj)) {
        Py_ssize_t n = PyDict_Size(obj);
        if (n > 32) init_cap = n * 12;
    }
    Buf b={NULL,0,0};
    b.buf=malloc((size_t)init_cap); if (b.buf) b.cap=init_cap;
    int ok=serialize(&b,&ds,obj);
    dedup_destroy(&ds);
    if (!ok) { free(b.buf); return NULL; }
    PyObject *result=PyBytes_FromStringAndSize((char*)b.buf,b.len);
    free(b.buf); return result;
}

static PyObject *py_dedup_reset(PyObject *self, PyObject *args) {
    if (source_cache) PyDict_Clear(source_cache);
    if (exec_cache)   PyDict_Clear(exec_cache);
    Py_RETURN_NONE;
}

static PyObject *py_deserialize_fast(PyObject *self, PyObject *args) {
    const uint8_t *data; Py_ssize_t size;
    if (!PyArg_ParseTuple(args,"y#",&data,&size)) return NULL;
    read_table_reset();
    Py_ssize_t pos=0;
    return deserialize(data,size,&pos);
}

static PyMethodDef methods[] = {
    {"serialize_fast",   py_serialize_fast,   METH_VARARGS, "Serializa en C"},
    {"deserialize_fast", py_deserialize_fast, METH_VARARGS, "Deserializa en C"},
    {"dedup_reset",      py_dedup_reset,      METH_NOARGS,  "Limpia caches de source/exec"},
    {NULL,NULL,0,NULL}
};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT, "_compickle", NULL, -1, methods
};

PyMODINIT_FUNC PyInit__compickle(void) {
    source_cache=PyDict_New();
    exec_cache=PyDict_New();
    cls_cache=PyDict_New();
    if (!source_cache||!exec_cache||!cls_cache) return NULL;
    return PyModule_Create(&moduledef);
}
