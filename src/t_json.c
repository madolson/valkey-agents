/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"

#include <stdatomic.h>

/* Live JSON objects. Decremented on the lazyfree thread too. */
static _Atomic size_t json_documents = 0;

robj *createJsonObject(jsonValue *root) {
    robj *o = createObject(OBJ_JSON, root);
    objectSetEncoding(o, OBJ_ENCODING_JSON);
    atomic_fetch_add_explicit(&json_documents, 1, memory_order_relaxed);
    return o;
}

void freeJsonObject(robj *o) {
    jsonFree(objectGetVal(o));
    atomic_fetch_sub_explicit(&json_documents, 1, memory_order_relaxed);
}

/* INFO json, named and scoped like the valkey-json module's section so that
 * existing scrapers keep working. */
sds genJsonInfoString(sds info) {
    return sdscatprintf(info,
                        "# json_core_metrics\r\n"
                        "json_total_memory_bytes:%zu\r\n"
                        "json_num_documents:%zu\r\n",
                        jsonUsedMemory(), atomic_load_explicit(&json_documents, memory_order_relaxed));
}

robj *jsonTypeDup(robj *o) {
    return createJsonObject(jsonDup(objectGetVal(o)));
}

size_t jsonTypeMemUsage(robj *o) {
    return jsonMemoryUsage(objectGetVal(o));
}

void jsonTypeDigest(unsigned char *digest, robj *o) {
    sds text = jsonSerialize(sdsempty(), objectGetVal(o), NULL, 0);
    mixDigest(digest, text, sdslen(text));
    sdsfree(text);
}

static size_t jsonCountValues(const jsonValue *v, size_t limit) {
    size_t count = 1;
    size_t children = jsonChildCount(v);
    for (size_t i = 0; i < children && count < limit; i++) {
        count += jsonCountValues(jsonChildAt(v, i), limit - count);
    }
    return count;
}

/* Number of values in the document, counting stops at 'limit' so that
 * deciding whether to free lazily stays cheap for large documents. */
size_t jsonTypeFreeEffort(robj *o, size_t limit) {
    return jsonCountValues(objectGetVal(o), limit);
}
