/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* JSON document model, parser and serializer for the native JSON type.
 *
 * Behaviour follows valkey-json (RapidJSON based) byte for byte: which inputs
 * are accepted, how numbers and strings are re-emitted, how duplicate member
 * names resolve, and how JSON.GET formatting options lay out the output.
 *
 * Values are heap allocated and containers hold pointers to their children, so
 * a jsonValue * stays valid until that value itself is removed or freed. */

#ifndef __JSON_H
#define __JSON_H

#include <stddef.h>
#include <stdint.h>

#include "sds.h"

#define JSON_DEFAULT_MAX_DEPTH 128
/* Deepest document RDB loading accepts, and so the json.max-path-limit
 * ceiling: anything a write allowed must load again. */
#define JSON_MAX_DEPTH_LIMIT 10000
#define JSON_OBJECT_INDEX_MIN 32
#define JSON_DOUBLE_BUFSIZE 32

typedef enum jsonType {
    JSON_NULL = 0,
    JSON_FALSE,
    JSON_TRUE,
    JSON_INTEGER, /* Integer literal that fits int64_t. */
    JSON_NUMBER,  /* Any other number. Keeps its source text. */
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} jsonType;

typedef struct jsonValue jsonValue;

typedef struct jsonMember {
    sds name;
    jsonValue *value;
} jsonMember;

typedef struct jsonNumber {
    sds text;     /* Re-emitted verbatim by the serializer. */
    double value; /* strtod() of text, may be +/-inf. */
} jsonNumber;

typedef struct jsonArray {
    jsonValue **items;
    size_t len;
    size_t cap;
} jsonArray;

typedef struct jsonObject {
    jsonMember *members; /* Insertion order. */
    size_t len;
    size_t cap;
    /* Open addressing hash of member positions (position + 1, 0 is empty),
     * built once an object grows past JSON_OBJECT_INDEX_MIN members. */
    uint32_t *index;
    size_t index_cap;
} jsonObject;

struct jsonValue {
    jsonType type;
    union {
        int64_t integer;
        jsonNumber number;
        sds string;
        jsonArray array;
        jsonObject object;
    };
};

/* Parse status codes. */
#define JSON_OK 0
#define JSON_ERR_SYNTAX 1
#define JSON_ERR_DEPTH 2

/* JSON.GET INDENT, NEWLINE and SPACE. NULL or empty strings write nothing. */
typedef struct jsonFormat {
    const char *indent;
    size_t indent_len;
    const char *newline;
    size_t newline_len;
    const char *space;
    size_t space_len;
} jsonFormat;

/* Creation and destruction. */
jsonValue *jsonCreateNull(void);
jsonValue *jsonCreateBool(int b);
jsonValue *jsonCreateInteger(int64_t i);
jsonValue *jsonCreateNumber(const char *text, size_t len);
jsonValue *jsonCreateString(const char *s, size_t len);
jsonValue *jsonCreateArray(void);
jsonValue *jsonCreateObject(void);
jsonValue *jsonDup(const jsonValue *v);
void jsonFree(jsonValue *v);
void jsonReplace(jsonValue *dst, jsonValue *src);

/* Parsing. */
jsonValue *jsonParse(const char *buf, size_t len, size_t max_depth, int *err, size_t *depth);
const char *jsonErrorMessage(int err);

/* Serialization. */
sds jsonSerialize(sds s, const jsonValue *v, const jsonFormat *fmt, size_t level);
sds jsonCatString(sds s, const char *str, size_t len);
int jsonFormatDouble(double d, char *buf); /* buf holds JSON_DOUBLE_BUFSIZE */

/* Inspection. */
double jsonGetDouble(const jsonValue *v);
size_t jsonDepth(const jsonValue *v);
size_t jsonMemoryUsage(const jsonValue *v);
size_t jsonUsedMemory(void); /* Total jsonMemoryUsage() of all live values. */
size_t jsonChildCount(const jsonValue *v);
jsonValue *jsonChildAt(const jsonValue *v, size_t i);

/* Arrays. Indexes are not range checked beyond assertions. */
void jsonArrayAppend(jsonValue *arr, jsonValue *v);
void jsonArrayInsert(jsonValue *arr, size_t idx, jsonValue *v);
void jsonArraySet(jsonValue *arr, size_t idx, jsonValue *v);
jsonValue *jsonArrayDetach(jsonValue *arr, size_t idx);
void jsonArrayDeleteRange(jsonValue *arr, size_t idx, size_t count);

/* Objects. */
jsonValue *jsonObjectFind(const jsonValue *obj, const char *name, size_t len, size_t *pos);
void jsonObjectSet(jsonValue *obj, const char *name, size_t len, jsonValue *v);
int jsonObjectDelete(jsonValue *obj, const char *name, size_t len);
void jsonObjectDeleteAt(jsonValue *obj, size_t pos);

#endif
