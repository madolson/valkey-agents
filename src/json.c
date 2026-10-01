/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "json.h"

#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "fpconv_dtoa.h"
#include "hashtable.h"
#include "serverassert.h"
#include "util.h"
#include "valkey_strtod.h"
#include "zmalloc.h"

/* ----------------------------------------------------------------------------
 * Memory accounting
 *
 * jsonUsedMemory() is the total size of all live values. Each outermost call
 * into this file that allocates or frees adds the change in the calling
 * thread's allocator counter, so single allocations cost nothing extra. Calls
 * made inside it, such as the parser creating values, are covered by the
 * outer measurement. So every exported function that allocates or frees value
 * memory must run inside jsonTrackBegin() and jsonTrackEnd(), and must not
 * allocate anything else that outlives the call. Values are freed on the
 * lazyfree thread too.
 * ------------------------------------------------------------------------- */

static _Atomic size_t json_used_memory = 0;
static _Thread_local int json_track_depth = 0;

size_t jsonUsedMemory(void) {
    return atomic_load_explicit(&json_used_memory, memory_order_relaxed);
}

static inline size_t jsonTrackBegin(void) {
    return json_track_depth++ == 0 ? zmalloc_thread_used_memory() : 0;
}

static inline void jsonTrackEnd(size_t before) {
    if (--json_track_depth == 0) {
        atomic_fetch_add_explicit(&json_used_memory, zmalloc_thread_used_memory() - before, memory_order_relaxed);
    }
}

/* ----------------------------------------------------------------------------
 * Values
 * ------------------------------------------------------------------------- */

/* Unaccounted constructors, for the parser and the public ones below. */
static jsonValue *jsonNew(jsonType type) {
    jsonValue *v = zcalloc(sizeof(*v));
    v->type = type;
    return v;
}

static jsonValue *jsonNewInteger(int64_t i) {
    jsonValue *v = jsonNew(JSON_INTEGER);
    v->integer = i;
    return v;
}

static double jsonStrtod(sds text) {
    char *end;
    errno = 0;
    double d = valkey_strtod_n(text, sdslen(text), &end);
    /* valkey_strtod reports overflow as 0.0. The module stores strtod()'s
     * result, which is +/-inf there, so let libc decide every hard case. */
    if (errno != 0 || end != text + sdslen(text)) d = strtod(text, NULL);
    return d;
}

static jsonValue *jsonNewNumber(const char *text, size_t len) {
    jsonValue *v = jsonNew(JSON_NUMBER);
    v->number.text = sdsnewlen(text, len);
    v->number.value = jsonStrtod(v->number.text);
    return v;
}

static jsonValue *jsonNewTracked(jsonType type) {
    size_t mem = jsonTrackBegin();
    jsonValue *v = jsonNew(type);
    jsonTrackEnd(mem);
    return v;
}

jsonValue *jsonCreateNull(void) {
    return jsonNewTracked(JSON_NULL);
}

jsonValue *jsonCreateBool(int b) {
    return jsonNewTracked(b ? JSON_TRUE : JSON_FALSE);
}

jsonValue *jsonCreateInteger(int64_t i) {
    jsonValue *v = jsonNewTracked(JSON_INTEGER);
    v->integer = i;
    return v;
}

jsonValue *jsonCreateNumber(const char *text, size_t len) {
    size_t mem = jsonTrackBegin();
    jsonValue *v = jsonNewNumber(text, len);
    jsonTrackEnd(mem);
    return v;
}

jsonValue *jsonCreateString(const char *s, size_t len) {
    size_t mem = jsonTrackBegin();
    jsonValue *v = jsonNew(JSON_STRING);
    v->string = sdsnewlen(s, len);
    jsonTrackEnd(mem);
    return v;
}

jsonValue *jsonCreateArray(void) {
    return jsonNewTracked(JSON_ARRAY);
}

jsonValue *jsonCreateObject(void) {
    return jsonNewTracked(JSON_OBJECT);
}

static void jsonFreeValue(jsonValue *v);

static void jsonFreeContents(jsonValue *v) {
    size_t i;
    switch (v->type) {
    case JSON_NUMBER: sdsfree(v->number.text); break;
    case JSON_STRING: sdsfree(v->string); break;
    case JSON_ARRAY:
        for (i = 0; i < v->array.len; i++) jsonFreeValue(v->array.items[i]);
        zfree(v->array.items);
        break;
    case JSON_OBJECT:
        for (i = 0; i < v->object.len; i++) {
            sdsfree(v->object.members[i].name);
            jsonFreeValue(v->object.members[i].value);
        }
        zfree(v->object.members);
        zfree(v->object.index);
        break;
    default: break;
    }
}

static void jsonFreeValue(jsonValue *v) {
    jsonFreeContents(v);
    zfree(v);
}

void jsonFree(jsonValue *v) {
    if (v == NULL) return;
    size_t mem = jsonTrackBegin();
    jsonFreeValue(v);
    jsonTrackEnd(mem);
}

/* Replace the contents of dst with src, keeping dst's address, and free the
 * src shell. Pointers to dst stay valid, pointers to src do not. src must not
 * be part of dst. */
void jsonReplace(jsonValue *dst, jsonValue *src) {
    size_t mem = jsonTrackBegin();
    jsonFreeContents(dst);
    *dst = *src;
    zfree(src);
    jsonTrackEnd(mem);
}

jsonValue *jsonDup(const jsonValue *v) {
    size_t mem = jsonTrackBegin();
    jsonValue *d = zmalloc(sizeof(*d));
    *d = *v;
    size_t i;
    switch (v->type) {
    case JSON_NUMBER: d->number.text = sdsdup(v->number.text); break;
    case JSON_STRING: d->string = sdsdup(v->string); break;
    case JSON_ARRAY:
        d->array.cap = v->array.len;
        d->array.items = v->array.len ? zmalloc(sizeof(jsonValue *) * v->array.len) : NULL;
        for (i = 0; i < v->array.len; i++) d->array.items[i] = jsonDup(v->array.items[i]);
        break;
    case JSON_OBJECT:
        d->object.cap = v->object.len;
        d->object.members = v->object.len ? zmalloc(sizeof(jsonMember) * v->object.len) : NULL;
        for (i = 0; i < v->object.len; i++) {
            d->object.members[i].name = sdsdup(v->object.members[i].name);
            d->object.members[i].value = jsonDup(v->object.members[i].value);
        }
        if (v->object.index) {
            d->object.index = zmalloc(sizeof(uint32_t) * v->object.index_cap);
            memcpy(d->object.index, v->object.index, sizeof(uint32_t) * v->object.index_cap);
        }
        break;
    default: break;
    }
    jsonTrackEnd(mem);
    return d;
}

double jsonGetDouble(const jsonValue *v) {
    if (v->type == JSON_INTEGER) return (double)v->integer;
    if (v->type == JSON_NUMBER) return v->number.value;
    return 0;
}

size_t jsonDepth(const jsonValue *v) {
    size_t max = 0, n = jsonChildCount(v);
    if (v->type != JSON_ARRAY && v->type != JSON_OBJECT) return 0;
    for (size_t i = 0; i < n; i++) {
        size_t d = jsonDepth(jsonChildAt(v, i));
        if (d > max) max = d;
    }
    return max + 1;
}

size_t jsonMemoryUsage(const jsonValue *v) {
    size_t size = zmalloc_size((void *)v), i;
    switch (v->type) {
    case JSON_NUMBER: size += sdsAllocSize(v->number.text); break;
    case JSON_STRING: size += sdsAllocSize(v->string); break;
    case JSON_ARRAY:
        if (v->array.items) size += zmalloc_size(v->array.items);
        for (i = 0; i < v->array.len; i++) size += jsonMemoryUsage(v->array.items[i]);
        break;
    case JSON_OBJECT:
        if (v->object.members) size += zmalloc_size(v->object.members);
        if (v->object.index) size += zmalloc_size(v->object.index);
        for (i = 0; i < v->object.len; i++) {
            size += sdsAllocSize(v->object.members[i].name);
            size += jsonMemoryUsage(v->object.members[i].value);
        }
        break;
    default: break;
    }
    return size;
}

size_t jsonChildCount(const jsonValue *v) {
    if (v->type == JSON_ARRAY) return v->array.len;
    if (v->type == JSON_OBJECT) return v->object.len;
    return 0;
}

jsonValue *jsonChildAt(const jsonValue *v, size_t i) {
    if (v->type == JSON_ARRAY) {
        assert(i < v->array.len);
        return v->array.items[i];
    }
    assert(v->type == JSON_OBJECT && i < v->object.len);
    return v->object.members[i].value;
}

/* ----------------------------------------------------------------------------
 * Arrays
 * ------------------------------------------------------------------------- */

static void jsonArrayReserve(jsonArray *a, size_t need) {
    if (need <= a->cap) return;
    size_t cap = a->cap + a->cap / 2;
    if (cap < need) cap = need;
    if (cap < 4) cap = 4;
    a->items = zrealloc(a->items, sizeof(jsonValue *) * cap);
    a->cap = cap;
}

void jsonArrayAppend(jsonValue *arr, jsonValue *v) {
    jsonArrayInsert(arr, arr->array.len, v);
}

void jsonArrayInsert(jsonValue *arr, size_t idx, jsonValue *v) {
    jsonArray *a = &arr->array;
    assert(arr->type == JSON_ARRAY && idx <= a->len);
    size_t mem = jsonTrackBegin();
    jsonArrayReserve(a, a->len + 1);
    jsonTrackEnd(mem);
    memmove(a->items + idx + 1, a->items + idx, sizeof(jsonValue *) * (a->len - idx));
    a->items[idx] = v;
    a->len++;
}

void jsonArraySet(jsonValue *arr, size_t idx, jsonValue *v) {
    assert(arr->type == JSON_ARRAY && idx < arr->array.len);
    jsonFree(arr->array.items[idx]);
    arr->array.items[idx] = v;
}

/* Remove the element at idx and return it without freeing it. */
jsonValue *jsonArrayDetach(jsonValue *arr, size_t idx) {
    jsonArray *a = &arr->array;
    assert(arr->type == JSON_ARRAY && idx < a->len);
    jsonValue *v = a->items[idx];
    memmove(a->items + idx, a->items + idx + 1, sizeof(jsonValue *) * (a->len - idx - 1));
    a->len--;
    return v;
}

void jsonArrayDeleteRange(jsonValue *arr, size_t idx, size_t count) {
    jsonArray *a = &arr->array;
    assert(arr->type == JSON_ARRAY && idx <= a->len && count <= a->len - idx);
    size_t mem = jsonTrackBegin();
    for (size_t i = idx; i < idx + count; i++) jsonFree(a->items[i]);
    jsonTrackEnd(mem);
    memmove(a->items + idx, a->items + idx + count, sizeof(jsonValue *) * (a->len - idx - count));
    a->len -= count;
}

/* ----------------------------------------------------------------------------
 * Objects
 *
 * Members stay in insertion order in a vector. Past JSON_OBJECT_INDEX_MIN
 * members a linear probing index over the vector keeps lookups, and so parsing
 * objects with many members, from going quadratic.
 * ------------------------------------------------------------------------- */

static size_t jsonNameSlot(const jsonObject *o, const char *name, size_t len) {
    return hashtableGenHashFunction(name, len) & (o->index_cap - 1);
}

static void jsonIndexInsert(jsonObject *o, size_t pos) {
    size_t mask = o->index_cap - 1;
    sds name = o->members[pos].name;
    size_t i = jsonNameSlot(o, name, sdslen(name));
    while (o->index[i]) i = (i + 1) & mask;
    o->index[i] = (uint32_t)(pos + 1);
}

static void jsonIndexBuild(jsonObject *o) {
    size_t cap = 64;
    while (cap < o->len * 2) cap <<= 1;
    zfree(o->index);
    o->index = zcalloc(sizeof(uint32_t) * cap);
    o->index_cap = cap;
    for (size_t i = 0; i < o->len; i++) jsonIndexInsert(o, i);
}

/* Drop member pos from the index, then renumber the members after it. Must run
 * before the member vector is shifted, since it rehashes names by position. */
static void jsonIndexRemove(jsonObject *o, size_t pos) {
    size_t mask = o->index_cap - 1;
    sds name = o->members[pos].name;
    size_t i = jsonNameSlot(o, name, sdslen(name)), j;
    while (o->index[i] != pos + 1) i = (i + 1) & mask;
    /* Backward shift deletion keeps every probe chain unbroken. */
    for (j = i;;) {
        j = (j + 1) & mask;
        if (o->index[j] == 0) break;
        sds other = o->members[o->index[j] - 1].name;
        size_t k = jsonNameSlot(o, other, sdslen(other));
        if ((j > i && (k <= i || k > j)) || (j < i && k <= i && k > j)) {
            o->index[i] = o->index[j];
            i = j;
        }
    }
    o->index[i] = 0;
    for (j = 0; j < o->index_cap; j++) {
        if (o->index[j] > pos + 1) o->index[j]--;
    }
}

static ssize_t jsonMemberPos(const jsonObject *o, const char *name, size_t len) {
    if (o->index) {
        size_t mask = o->index_cap - 1, i = jsonNameSlot(o, name, len);
        for (; o->index[i]; i = (i + 1) & mask) {
            jsonMember *m = &o->members[o->index[i] - 1];
            if (sdslen(m->name) == len && memcmp(m->name, name, len) == 0) return o->index[i] - 1;
        }
        return -1;
    }
    for (size_t i = 0; i < o->len; i++) {
        sds n = o->members[i].name;
        if (sdslen(n) == len && memcmp(n, name, len) == 0) return i;
    }
    return -1;
}

/* Append a member whose name is known to be absent. Takes ownership of name. */
static void jsonMemberAppend(jsonObject *o, sds name, jsonValue *v) {
    if (o->len == o->cap) {
        size_t cap = o->cap + o->cap / 2;
        if (cap < 4) cap = 4;
        o->members = zrealloc(o->members, sizeof(jsonMember) * cap);
        o->cap = cap;
    }
    o->members[o->len].name = name;
    o->members[o->len].value = v;
    o->len++;
    if (o->index && o->len * 2 <= o->index_cap) {
        jsonIndexInsert(o, o->len - 1);
    } else if (o->len > JSON_OBJECT_INDEX_MIN) {
        jsonIndexBuild(o);
    }
}

jsonValue *jsonObjectFind(const jsonValue *obj, const char *name, size_t len, size_t *pos) {
    assert(obj->type == JSON_OBJECT);
    ssize_t p = jsonMemberPos(&obj->object, name, len);
    if (p < 0) return NULL;
    if (pos) *pos = p;
    return obj->object.members[p].value;
}

/* Set member name to v. An existing member keeps its position, as with a
 * duplicate name in parsed input. */
void jsonObjectSet(jsonValue *obj, const char *name, size_t len, jsonValue *v) {
    assert(obj->type == JSON_OBJECT);
    ssize_t p = jsonMemberPos(&obj->object, name, len);
    if (p >= 0) {
        jsonFree(obj->object.members[p].value);
        obj->object.members[p].value = v;
    } else {
        size_t mem = jsonTrackBegin();
        jsonMemberAppend(&obj->object, sdsnewlen(name, len), v);
        jsonTrackEnd(mem);
    }
}

void jsonObjectDeleteAt(jsonValue *obj, size_t pos) {
    jsonObject *o = &obj->object;
    assert(obj->type == JSON_OBJECT && pos < o->len);
    if (o->index) jsonIndexRemove(o, pos);
    size_t mem = jsonTrackBegin();
    sdsfree(o->members[pos].name);
    jsonFree(o->members[pos].value);
    jsonTrackEnd(mem);
    memmove(o->members + pos, o->members + pos + 1, sizeof(jsonMember) * (o->len - pos - 1));
    o->len--;
}

int jsonObjectDelete(jsonValue *obj, const char *name, size_t len) {
    size_t pos;
    if (jsonObjectFind(obj, name, len, &pos) == NULL) return 0;
    jsonObjectDeleteAt(obj, pos);
    return 1;
}

/* ----------------------------------------------------------------------------
 * Byte scanning shared by the parser and serializer
 * ------------------------------------------------------------------------- */

#define JSON_ONES 0x0101010101010101ULL
#define JSON_HIGHS 0x8080808080808080ULL

static inline int jsonIsSpecial(unsigned char c) {
    return c < 0x20 || c == '"' || c == '\\';
}

/* Return the first byte in [p, end) that is '"', '\\' or a control character,
 * or end. Eight bytes at a time: each term is nonzero only if some byte of w is
 * < 0x20, == '"' or == '\\' respectively. */
static inline const char *jsonScanPlain(const char *p, const char *end) {
    while (end - p >= 8) {
        uint64_t w, q, b;
        memcpy(&w, p, 8);
        q = w ^ (JSON_ONES * '"');
        b = w ^ (JSON_ONES * '\\');
        if ((((w - JSON_ONES * 0x20) & ~w) | ((q - JSON_ONES) & ~q) | ((b - JSON_ONES) & ~b)) & JSON_HIGHS) break;
        p += 8;
    }
    while (p < end && !jsonIsSpecial((unsigned char)*p)) p++;
    return p;
}

/* ----------------------------------------------------------------------------
 * Parser
 *
 * A port of the RapidJSON reader valkey-json uses (non-insitu, default flags),
 * including where it is looser or stricter than RFC 8259.
 * ------------------------------------------------------------------------- */

typedef struct jsonParser {
    const char *p;
    const char *end;
    size_t max_depth;
    size_t depth;
    size_t deepest;
    int err;
    /* Children of the containers being parsed. Object members are pushed as
     * name, value pairs. */
    void **stack;
    size_t sp;
    size_t stack_cap;
} jsonParser;

static void jsonPush(jsonParser *P, void *item) {
    if (P->sp == P->stack_cap) {
        P->stack_cap = P->stack_cap ? P->stack_cap * 2 : 64;
        P->stack = zrealloc(P->stack, sizeof(void *) * P->stack_cap);
    }
    P->stack[P->sp++] = item;
}

static inline int jsonPeek(const jsonParser *P) {
    return P->p < P->end ? (unsigned char)*P->p : 0;
}

static inline void jsonSkipSpace(jsonParser *P) {
    while (P->p < P->end && (*P->p == ' ' || *P->p == '\n' || *P->p == '\r' || *P->p == '\t')) P->p++;
}

static inline int jsonIsDigit(const char *p, const char *end) {
    return p < end && *p >= '0' && *p <= '9';
}

static int jsonHex4(const char *p, const char *end, unsigned *out) {
    unsigned cp = 0;
    if (end - p < 4) return 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        cp <<= 4;
        if (c >= '0' && c <= '9')
            cp |= c - '0';
        else if (c >= 'A' && c <= 'F')
            cp |= c - 'A' + 10;
        else if (c >= 'a' && c <= 'f')
            cp |= c - 'a' + 10;
        else
            return 0;
    }
    *out = cp;
    return 1;
}

/* Decode the escape sequence at p (just past the backslash) into a code point.
 * Returns the position after it, or NULL if the escape is invalid. */
static const char *jsonScanEscape(const char *p, const char *end, unsigned *cp) {
    if (p >= end) return NULL;
    switch (*p) {
    case '"': *cp = '"'; return p + 1;
    case '\\': *cp = '\\'; return p + 1;
    case '/': *cp = '/'; return p + 1;
    case 'b': *cp = '\b'; return p + 1;
    case 'f': *cp = '\f'; return p + 1;
    case 'n': *cp = '\n'; return p + 1;
    case 'r': *cp = '\r'; return p + 1;
    case 't': *cp = '\t'; return p + 1;
    case 'u': break;
    default: return NULL;
    }
    unsigned hi, lo;
    if (!jsonHex4(p + 1, end, &hi)) return NULL;
    p += 5;
    if (hi >= 0xD800 && hi <= 0xDFFF) {
        if (hi > 0xDBFF) return NULL;
        if (end - p < 2 || p[0] != '\\' || p[1] != 'u') return NULL;
        if (!jsonHex4(p + 2, end, &lo) || lo < 0xDC00 || lo > 0xDFFF) return NULL;
        hi = (((hi - 0xD800) << 10) | (lo - 0xDC00)) + 0x10000;
        p += 6;
    }
    *cp = hi;
    return p;
}

static inline size_t jsonUtf8Len(unsigned cp) {
    if (cp < 0x80) return 1;
    if (cp < 0x800) return 2;
    if (cp < 0x10000) return 3;
    return 4;
}

static inline char *jsonUtf8Put(char *o, unsigned cp) {
    if (cp < 0x80) {
        *o++ = (char)cp;
    } else if (cp < 0x800) {
        *o++ = (char)(0xC0 | (cp >> 6));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | (cp >> 12));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | (cp >> 18));
        *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

/* Parse the string at P->p (on the opening quote). The decoded length is
 * found first so the result is allocated exactly once at its final size.
 * Bytes >= 0x80 are copied through without UTF-8 validation. */
static sds jsonParseString(jsonParser *P) {
    const char *start = P->p + 1, *end = P->end, *r;
    const char *q = jsonScanPlain(start, end);
    if (q < end && *q == '"') {
        P->p = q + 1;
        return sdsnewlen(start, q - start);
    }

    size_t len = q - start;
    unsigned cp;
    for (r = q;;) {
        if (r >= end) return NULL;
        if (*r == '"') break;
        if (*r == '\\') {
            if ((r = jsonScanEscape(r + 1, end, &cp)) == NULL) return NULL;
            len += jsonUtf8Len(cp);
        } else if ((unsigned char)*r < 0x20) {
            return NULL;
        } else {
            q = jsonScanPlain(r, end);
            len += q - r;
            r = q;
        }
    }

    sds s = sdsnewlen(SDS_NOINIT, len);
    char *o = s;
    for (r = start; *r != '"';) {
        if (*r == '\\') {
            r = jsonScanEscape(r + 1, end, &cp);
            o = jsonUtf8Put(o, cp);
        } else {
            q = jsonScanPlain(r, end);
            memcpy(o, r, q - r);
            o += q - r;
            r = q;
        }
    }
    P->p = r + 1;
    return s;
}

/* RapidJSON's number grammar and its overflow guard. Accepted numbers are
 * classified exactly as RapidJSON does: an integer that fits int64_t, or
 * anything else, which keeps its text. A positive exponent is rejected once it
 * exceeds 308 plus the count of fraction digits RapidJSON tracked (at most 17
 * significant ones), whatever the value would be: 9e308 and 0.0e309 pass, 1e309
 * and 0e309 fail. Negative exponents are never rejected. */
static jsonValue *jsonParseNumber(jsonParser *P) {
    const char *s = P->p, *p = s, *end = P->end;
    int minus = 0, is_double = 0;
    uint64_t i64 = 0;
    long long sig = 0, exp_frac = 0;

    if (p < end && *p == '-') {
        minus = 1;
        p++;
    }
    if (p < end && *p == '0') {
        p++;
    } else if (p < end && *p >= '1' && *p <= '9') {
        const uint64_t cut = 922337203685477580ULL; /* INT64_MAX / 10 */
        const char last = minus ? '8' : '7';
        i64 = *p++ - '0';
        while (jsonIsDigit(p, end)) {
            if (i64 >= cut && (i64 != cut || *p > last)) {
                is_double = 1;
                break;
            }
            i64 = i64 * 10 + (*p++ - '0');
            sig++;
        }
        while (jsonIsDigit(p, end)) p++;
    } else {
        return NULL;
    }

    if (p < end && *p == '.') {
        p++;
        if (!jsonIsDigit(p, end)) return NULL;
        if (!is_double) {
            while (jsonIsDigit(p, end) && i64 <= 0x1FFFFFFFFFFFFFULL) {
                i64 = i64 * 10 + (*p++ - '0');
                exp_frac--;
                if (i64 != 0) sig++;
            }
            is_double = 1;
        }
        for (; jsonIsDigit(p, end); p++) {
            if (sig < 17) {
                exp_frac--;
                sig++;
            }
        }
    }

    if (p < end && (*p == 'e' || *p == 'E')) {
        int exp_minus = 0;
        is_double = 1;
        p++;
        if (p < end && *p == '+') {
            p++;
        } else if (p < end && *p == '-') {
            exp_minus = 1;
            p++;
        }
        if (!jsonIsDigit(p, end)) return NULL;
        long long exp = *p++ - '0';
        if (exp_minus) {
            while (jsonIsDigit(p, end)) p++;
        } else {
            long long max_exp = 308 - exp_frac;
            while (jsonIsDigit(p, end)) {
                exp = exp * 10 + (*p++ - '0');
                if (exp > max_exp) return NULL;
            }
        }
    }

    P->p = p;
    if (is_double) return jsonNewNumber(s, p - s);
    /* -0 is the integer 0. */
    return jsonNewInteger(minus ? (int64_t)(0 - i64) : (int64_t)i64);
}

static int jsonParseLiteral(jsonParser *P, const char *lit, size_t len) {
    if ((size_t)(P->end - P->p) < len || memcmp(P->p, lit, len) != 0) return 0;
    P->p += len;
    return 1;
}

static jsonValue *jsonParseValue(jsonParser *P);

static int jsonEnter(jsonParser *P) {
    P->p++;
    if (++P->depth > P->deepest) P->deepest = P->depth;
    if (P->depth > P->max_depth) {
        P->err = JSON_ERR_DEPTH;
        return 0;
    }
    return 1;
}

static jsonValue *jsonParseArray(jsonParser *P) {
    if (!jsonEnter(P)) return NULL;
    size_t base = P->sp;
    jsonSkipSpace(P);
    if (jsonPeek(P) == ']') {
        P->p++;
    } else {
        for (;;) {
            jsonValue *v = jsonParseValue(P);
            if (v == NULL) goto fail;
            jsonPush(P, v);
            jsonSkipSpace(P);
            if (jsonPeek(P) == ',') {
                P->p++;
                jsonSkipSpace(P);
            } else if (jsonPeek(P) == ']') {
                P->p++;
                break;
            } else {
                goto fail;
            }
        }
    }

    jsonValue *arr = jsonNew(JSON_ARRAY);
    size_t n = P->sp - base;
    if (n) {
        arr->array.items = zmalloc(sizeof(jsonValue *) * n);
        memcpy(arr->array.items, P->stack + base, sizeof(jsonValue *) * n);
        arr->array.len = arr->array.cap = n;
    }
    P->sp = base;
    P->depth--;
    return arr;

fail:
    while (P->sp > base) jsonFreeValue(P->stack[--P->sp]);
    return NULL;
}

static jsonValue *jsonParseObject(jsonParser *P) {
    if (!jsonEnter(P)) return NULL;
    size_t base = P->sp;
    jsonSkipSpace(P);
    if (jsonPeek(P) == '}') {
        P->p++;
    } else {
        for (;;) {
            sds name;
            jsonValue *v;
            if (jsonPeek(P) != '"' || (name = jsonParseString(P)) == NULL) goto fail;
            jsonPush(P, name);
            jsonSkipSpace(P);
            if (jsonPeek(P) != ':') goto fail;
            P->p++;
            jsonSkipSpace(P);
            if ((v = jsonParseValue(P)) == NULL) goto fail;
            jsonPush(P, v);
            jsonSkipSpace(P);
            if (jsonPeek(P) == ',') {
                P->p++;
                jsonSkipSpace(P);
            } else if (jsonPeek(P) == '}') {
                P->p++;
                break;
            } else {
                goto fail;
            }
        }
    }

    jsonValue *obj = jsonNew(JSON_OBJECT);
    jsonObject *o = &obj->object;
    size_t n = (P->sp - base) / 2;
    if (n) {
        o->members = zmalloc(sizeof(jsonMember) * n);
        o->cap = n;
    }
    /* A repeated name keeps the first position and takes the last value. */
    for (size_t i = 0; i < n; i++) {
        sds name = P->stack[base + 2 * i];
        jsonValue *v = P->stack[base + 2 * i + 1];
        ssize_t pos = jsonMemberPos(o, name, sdslen(name));
        if (pos >= 0) {
            jsonFreeValue(o->members[pos].value);
            o->members[pos].value = v;
            sdsfree(name);
        } else {
            jsonMemberAppend(o, name, v);
        }
    }
    P->sp = base;
    P->depth--;
    return obj;

fail:
    while (P->sp > base) {
        P->sp--;
        if ((P->sp - base) % 2 == 0)
            sdsfree(P->stack[P->sp]);
        else
            jsonFreeValue(P->stack[P->sp]);
    }
    return NULL;
}

static jsonValue *jsonParseValue(jsonParser *P) {
    switch (jsonPeek(P)) {
    case 'n': return jsonParseLiteral(P, "null", 4) ? jsonNew(JSON_NULL) : NULL;
    case 't': return jsonParseLiteral(P, "true", 4) ? jsonNew(JSON_TRUE) : NULL;
    case 'f': return jsonParseLiteral(P, "false", 5) ? jsonNew(JSON_FALSE) : NULL;
    case '"': {
        sds s = jsonParseString(P);
        if (s == NULL) return NULL;
        jsonValue *v = jsonNew(JSON_STRING);
        v->string = s;
        return v;
    }
    case '[': return jsonParseArray(P);
    case '{': return jsonParseObject(P);
    default: return jsonParseNumber(P);
    }
}

/* Parse len bytes at buf. On failure returns NULL and sets *err to
 * JSON_ERR_SYNTAX, or JSON_ERR_DEPTH once more than max_depth arrays and
 * objects are open. On success *depth, if not NULL, is the nesting depth
 * (0 for a scalar). */
jsonValue *jsonParse(const char *buf, size_t len, size_t max_depth, int *err, size_t *depth) {
    size_t mem = jsonTrackBegin();
    jsonParser P = {0};
    /* RapidJSON reads a NUL byte as end of input, so anything after it is
     * ignored. */
    const char *nul = memchr(buf, '\0', len);
    P.p = buf;
    P.end = nul ? nul : buf + len;
    P.max_depth = max_depth;
    P.err = JSON_ERR_SYNTAX;

    /* RapidJSON's UTF-8 memory stream skips each byte of a BOM
     * independently. */
    if (P.p < P.end && (unsigned char)*P.p == 0xEF) P.p++;
    if (P.p < P.end && (unsigned char)*P.p == 0xBB) P.p++;
    if (P.p < P.end && (unsigned char)*P.p == 0xBF) P.p++;

    jsonValue *v = NULL;
    jsonSkipSpace(&P);
    if (P.p < P.end) v = jsonParseValue(&P);
    if (v) {
        jsonSkipSpace(&P);
        if (P.p != P.end) {
            jsonFree(v);
            v = NULL;
        }
    }
    zfree(P.stack);
    jsonTrackEnd(mem);
    if (err) *err = v ? JSON_OK : P.err;
    if (depth) *depth = P.deepest;
    return v;
}

const char *jsonErrorMessage(int err) {
    switch (err) {
    case JSON_ERR_DEPTH: return "LIMIT Document path nesting limit is exceeded";
    case JSON_ERR_SYNTAX: return "SYNTAXERR Failed to parse JSON string due to syntax error";
    default: return NULL;
    }
}

/* ----------------------------------------------------------------------------
 * Serializer
 *
 * The output length is computed up front (exact unless strings need escapes)
 * and reserved once. Tokens are then written through a cursor, checking only
 * that the reservation still holds.
 * ------------------------------------------------------------------------- */

typedef struct jsonWriter {
    sds s;
    char *p;
    char *end;
    const jsonFormat *fmt;
    size_t level;
} jsonWriter;

static void jsonWriterGrow(jsonWriter *w, size_t n) {
    sdssetlen(w->s, w->p - w->s);
    w->s = sdsMakeRoomFor(w->s, n);
    w->p = w->s + sdslen(w->s);
    w->end = w->p + sdsavail(w->s);
}

static inline void jsonEnsure(jsonWriter *w, size_t n) {
    if ((size_t)(w->end - w->p) < n) jsonWriterGrow(w, n);
}

static inline void jsonPut(jsonWriter *w, const char *s, size_t len) {
    jsonEnsure(w, len);
    memcpy(w->p, s, len);
    w->p += len;
}

static inline void jsonPutc(jsonWriter *w, char c) {
    jsonEnsure(w, 1);
    *w->p++ = c;
}

static void jsonPutString(jsonWriter *w, const char *str, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    const char *end = str + len, *q = jsonScanPlain(str, end), *r;
    jsonEnsure(w, len + 2);
    *w->p++ = '"';
    memcpy(w->p, str, q - str);
    w->p += q - str;
    if (q < end) jsonEnsure(w, (end - q) * 6 + 1);
    while (q < end) {
        unsigned char c = *q++;
        char *o = w->p;
        *o++ = '\\';
        switch (c) {
        case '"': *o++ = '"'; break;
        case '\\': *o++ = '\\'; break;
        case '\b': *o++ = 'b'; break;
        case '\t': *o++ = 't'; break;
        case '\n': *o++ = 'n'; break;
        case '\f': *o++ = 'f'; break;
        case '\r': *o++ = 'r'; break;
        default:
            *o++ = 'u';
            *o++ = '0';
            *o++ = '0';
            *o++ = hex[c >> 4];
            *o++ = hex[c & 0xF];
            break;
        }
        r = jsonScanPlain(q, end);
        memcpy(o, q, r - q);
        w->p = o + (r - q);
        q = r;
    }
    *w->p++ = '"';
}

static void jsonPutScalar(jsonWriter *w, const jsonValue *v) {
    switch (v->type) {
    case JSON_NULL: jsonPut(w, "null", 4); break;
    case JSON_TRUE: jsonPut(w, "true", 4); break;
    case JSON_FALSE: jsonPut(w, "false", 5); break;
    case JSON_INTEGER:
        jsonEnsure(w, 21);
        w->p += ll2string(w->p, 21, v->integer);
        break;
    case JSON_NUMBER: jsonPut(w, v->number.text, sdslen(v->number.text)); break;
    case JSON_STRING: jsonPutString(w, v->string, sdslen(v->string)); break;
    default: break;
    }
}

static void jsonWriteCompact(jsonWriter *w, const jsonValue *v) {
    size_t i;
    if (v->type == JSON_ARRAY) {
        jsonPutc(w, '[');
        for (i = 0; i < v->array.len; i++) {
            if (i) jsonPutc(w, ',');
            jsonWriteCompact(w, v->array.items[i]);
        }
        jsonPutc(w, ']');
    } else if (v->type == JSON_OBJECT) {
        jsonPutc(w, '{');
        for (i = 0; i < v->object.len; i++) {
            const jsonMember *m = &v->object.members[i];
            if (i) jsonPutc(w, ',');
            jsonPutString(w, m->name, sdslen(m->name));
            jsonPutc(w, ':');
            jsonWriteCompact(w, m->value);
        }
        jsonPutc(w, '}');
    } else {
        jsonPutScalar(w, v);
    }
}

static void jsonPutLine(jsonWriter *w, size_t indents) {
    const jsonFormat *f = w->fmt;
    jsonPut(w, f->newline, f->newline_len);
    for (size_t i = 0; i < indents; i++) jsonPut(w, f->indent, f->indent_len);
}

/* Same layout as RapidJSON's PrettyWriter: every member or element on its own
 * line indented one step deeper than its container, the closing bracket on a
 * line of its own unless the container is empty, SPACE only after ':', and
 * every indent offset by the starting level. */
static void jsonWritePretty(jsonWriter *w, const jsonValue *v, size_t depth) {
    size_t i, n = jsonChildCount(v), indents = w->level + depth;
    if (v->type == JSON_ARRAY) {
        jsonPutc(w, '[');
        for (i = 0; i < n; i++) {
            if (i) jsonPutc(w, ',');
            jsonPutLine(w, indents + 1);
            jsonWritePretty(w, v->array.items[i], depth + 1);
        }
        if (n) jsonPutLine(w, indents);
        jsonPutc(w, ']');
    } else if (v->type == JSON_OBJECT) {
        jsonPutc(w, '{');
        for (i = 0; i < n; i++) {
            const jsonMember *m = &v->object.members[i];
            if (i) jsonPutc(w, ',');
            jsonPutLine(w, indents + 1);
            jsonPutString(w, m->name, sdslen(m->name));
            jsonPutc(w, ':');
            jsonPut(w, w->fmt->space, w->fmt->space_len);
            jsonWritePretty(w, m->value, depth + 1);
        }
        if (n) jsonPutLine(w, indents);
        jsonPutc(w, '}');
    } else {
        jsonPutScalar(w, v);
    }
}

/* Output length of v, exact except for string escapes. */
static size_t jsonEstimate(const jsonValue *v, const jsonFormat *f, size_t indents) {
    size_t i, n, len;
    switch (v->type) {
    case JSON_NULL:
    case JSON_TRUE: return 4;
    case JSON_FALSE: return 5;
    case JSON_INTEGER: return sdigits10(v->integer);
    case JSON_NUMBER: return sdslen(v->number.text);
    case JSON_STRING: return sdslen(v->string) + 2;
    default: break;
    }
    n = jsonChildCount(v);
    if (n == 0) return 2;
    len = 2 + (n - 1);
    if (f) len += (n + 1) * f->newline_len + (n * (indents + 1) + indents) * f->indent_len;
    for (i = 0; i < n; i++) {
        if (v->type == JSON_OBJECT) {
            len += sdslen(v->object.members[i].name) + 3;
            if (f) len += f->space_len;
        }
        len += jsonEstimate(jsonChildAt(v, i), f, indents + 1);
    }
    return len;
}

/* Append v to s. With fmt NULL the output is compact; otherwise it uses
 * fmt's strings and indents as if v were nested level containers deep. */
sds jsonSerialize(sds s, const jsonValue *v, const jsonFormat *fmt, size_t level) {
    jsonWriter w;
    jsonFormat f;
    if (fmt) {
        /* NULL strings write nothing, like empty ones. */
        f = *fmt;
        if (!f.indent) f.indent = "", f.indent_len = 0;
        if (!f.newline) f.newline = "", f.newline_len = 0;
        if (!f.space) f.space = "", f.space_len = 0;
        w.fmt = &f;
    } else {
        w.fmt = NULL;
    }
    w.level = level;
    w.s = sdsMakeRoomForNonGreedy(s, jsonEstimate(v, w.fmt, level));
    w.p = w.s + sdslen(w.s);
    w.end = w.p + sdsavail(w.s);
    if (w.fmt)
        jsonWritePretty(&w, v, 0);
    else
        jsonWriteCompact(&w, v);
    sdssetlen(w.s, w.p - w.s);
    w.s[sdslen(w.s)] = '\0';
    return w.s;
}

/* Append str as a quoted, escaped JSON string. */
sds jsonCatString(sds s, const char *str, size_t len) {
    jsonWriter w;
    w.s = sdsMakeRoomForNonGreedy(s, len + 2);
    w.p = w.s + sdslen(w.s);
    w.end = w.p + sdsavail(w.s);
    jsonPutString(&w, str, len);
    sdssetlen(w.s, w.p - w.s);
    w.s[sdslen(w.s)] = '\0';
    return w.s;
}

/* ----------------------------------------------------------------------------
 * Doubles as RapidJSON's Writer prints them (JSON.RESP)
 * ------------------------------------------------------------------------- */

/* Render d like RapidJSON's internal::dtoa with its default precision:
 * shortest round-trip digits (Grisu2, the same algorithm fpconv uses), plain
 * notation while the decimal exponent is at most 21 with ".0" added to
 * integral values, otherwise d[.ddd]e[-]x. */
int jsonFormatDouble(double d, char *buf) {
    char tmp[32], digits[24];
    char *o = buf;
    int n, i, nd = 0, dot = -1, total = 0, exp10 = 0, k, kk;

    if (isnan(d)) return fpconv_dtoa(d, buf);
    if (signbit(d)) *o++ = '-';
    if (isinf(d)) {
        /* RapidJSON runs Grisu2 on the infinity bit pattern and prints the
         * largest double. Stored numbers that overflowed (9e308) hit this. */
        memcpy(o, "1.797693134862316e308", 21);
        return (int)(o - buf) + 21;
    }
    if (d == 0) {
        memcpy(o, "0.0", 3);
        return (int)(o - buf) + 3;
    }

    /* Recover Grisu2's digits and exponent from fpconv's %g-like output. */
    n = fpconv_dtoa(fabs(d), tmp);
    tmp[n] = '\0';
    for (i = 0; i < n && tmp[i] != 'e'; i++) {
        if (tmp[i] == '.') {
            dot = total;
            continue;
        }
        total++;
        if (nd == 0 && tmp[i] == '0') continue;
        digits[nd++] = tmp[i];
    }
    if (i < n) exp10 = atoi(tmp + i + 1);
    if (dot < 0) dot = total;
    k = dot - total + exp10;
    while (nd > 1 && digits[nd - 1] == '0') {
        nd--;
        k++;
    }
    kk = nd + k;

    if (k >= 0 && kk <= 21) {
        memcpy(o, digits, nd);
        memset(o + nd, '0', k);
        o += kk;
        *o++ = '.';
        *o++ = '0';
    } else if (kk > 0 && kk <= 21) {
        memcpy(o, digits, kk);
        o += kk;
        *o++ = '.';
        memcpy(o, digits + kk, nd - kk);
        o += nd - kk;
    } else if (kk > -6 && kk <= 0) {
        *o++ = '0';
        *o++ = '.';
        memset(o, '0', -kk);
        o += -kk;
        memcpy(o, digits, nd);
        o += nd;
    } else {
        *o++ = digits[0];
        if (nd > 1) {
            *o++ = '.';
            memcpy(o, digits + 1, nd - 1);
            o += nd - 1;
        }
        *o++ = 'e';
        o += ll2string(o, 8, kk - 1);
    }
    return (int)(o - buf);
}
