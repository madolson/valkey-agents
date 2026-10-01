/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* JSON commands. Replies, error texts and the order in which arguments, keys
 * and paths are checked follow valkey-json, so that clients written against
 * the module see the same behaviour. */

#include "server.h"
#include "json_path.h"

#include <math.h>
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

/* ----------------------------------------------------------------------------
 * Shared command helpers
 * ------------------------------------------------------------------------- */

#define JSON_MSG_NOT_DOC "WRONGTYPE Not a JSON document key"
#define JSON_MSG_KEY_NOT_FOUND "NONEXISTENT Document key does not exist"
#define JSON_MSG_DOC_NOT_FOUND "NONEXISTENT JSON document is not found"
#define JSON_MSG_NEW_KEY_NOT_ROOT "SYNTAXERR A new Valkey key's path must be root"
#define JSON_MSG_SYNTAX "SYNTAXERR Command syntax error"
#define JSON_MSG_SIZE_LIMIT "LIMIT Document size limit is exceeded"
#define JSON_MSG_PATH_LIMIT "LIMIT Document path nesting limit is exceeded"
#define JSON_MSG_NOT_NUMBER "WRONGTYPE JSON element is not a number"
#define JSON_MSG_NOT_BOOL "WRONGTYPE JSON element is not a bool"
#define JSON_MSG_NOT_STRING "WRONGTYPE JSON element is not a string"
#define JSON_MSG_NOT_OBJECT "WRONGTYPE JSON element is not an object"
#define JSON_MSG_NOT_ARRAY "WRONGTYPE JSON element is not an array"
#define JSON_MSG_VALUE_NOT_NUMBER "WRONGTYPE Value is not a number"
#define JSON_MSG_VALUE_NOT_STRING "WRONGTYPE Value is not a string"
#define JSON_MSG_VALUE_NOT_INTEGER "WRONGTYPE Value is not an integer"
#define JSON_MSG_MULT_OVERFLOW "OVERFLOW Multiplication would overflow"
#define JSON_MSG_ADD_OVERFLOW "OVERFLOW Addition would overflow"
#define JSON_MSG_OUT_OF_BOUNDS "OUTOFBOUNDARIES Array index is out of bounds"

/* The messages above carry their own error code, so no ERR prefix. */
static void jsonReplyError(client *c, const char *msg) {
    addReplyErrorFormat(c, "-%s", msg);
}

static void jsonReplyPathError(client *c, jsonPathCode rc) {
    jsonReplyError(c, jsonPathErrorMessage(rc));
}

static void jsonReplyParseError(client *c, int err) {
    jsonReplyError(c, jsonErrorMessage(err));
}

#define JSON_KEY_OK 0
#define JSON_KEY_MISSING 1
#define JSON_KEY_WRONGTYPE 2

static int jsonLookup(client *c, robj *key, int write, robj **o) {
    *o = write ? lookupKeyWrite(c->db, key) : lookupKeyRead(c->db, key);
    if (*o == NULL) return JSON_KEY_MISSING;
    if (objectGetType(*o) != OBJ_JSON) return JSON_KEY_WRONGTYPE;
    return JSON_KEY_OK;
}

/* Look up a key a command needs to exist. Replies and returns NULL if it is
 * missing or not JSON. */
static jsonValue *jsonLookupOrReply(client *c, robj *key, int write) {
    robj *o;
    switch (jsonLookup(c, key, write, &o)) {
    case JSON_KEY_MISSING: jsonReplyError(c, JSON_MSG_KEY_NOT_FOUND); return NULL;
    case JSON_KEY_WRONGTYPE: jsonReplyError(c, JSON_MSG_NOT_DOC); return NULL;
    }
    return objectGetVal(o);
}

/* Like jsonLookupOrReply, but a missing key replies null. */
static jsonValue *jsonLookupOrNull(client *c, robj *key) {
    robj *o;
    switch (jsonLookup(c, key, 0, &o)) {
    case JSON_KEY_MISSING: addReplyNull(c); return NULL;
    case JSON_KEY_WRONGTYPE: jsonReplyError(c, JSON_MSG_NOT_DOC); return NULL;
    }
    return objectGetVal(o);
}

static void jsonKeyModified(client *c, robj *key, const char *event) {
    signalModifiedKey(c, c->db, key);
    notifyKeyspaceEvent(NOTIFY_JSON, (char *)event, key, c->db->id);
    server.dirty++;
}

/* A command that failed after changing part of the document, as the module's
 * do. The module then neither notifies nor replicates, so replicas and the AOF
 * diverge. Propagate the whole document instead of the failed command, which
 * a replica would have to answer with an error. */
static void jsonKeyPartiallyModified(client *c, robj *key, const jsonValue *root) {
    signalModifiedKey(c, c->db, key);
    server.dirty++;
    preventCommandPropagation(c);

    robj *argv[4] = {createStringObject("JSON.SET", 8), key, createStringObject(".", 1),
                     createObject(OBJ_STRING, jsonSerialize(sdsempty(), root, NULL, 0))};
    alsoPropagate(c->db->id, argv, 4, PROPAGATE_AOF | PROPAGATE_REPL, c->slot);
    decrRefCount(argv[0]);
    decrRefCount(argv[2]);
    decrRefCount(argv[3]);

    /* JSON.SET at the root drops the TTL. */
    long long when = getExpire(c->db, key);
    if (when != -1) {
        robj *expargv[3] = {shared.pexpireat, key, createStringObjectFromLongLong(when)};
        alsoPropagate(c->db->id, expargv, 3, PROPAGATE_AOF | PROPAGATE_REPL, c->slot);
        decrRefCount(expargv[2]);
    }
}

static const char *jsonArgPath(client *c, int idx) {
    return objectGetVal(c->argv[idx]);
}

/* JSON.<cmd> key [path], the path defaulting to the root. */
static const char *jsonOptionalPath(client *c) {
    return c->argc > 2 ? jsonArgPath(c, 2) : ".";
}

static int jsonArgLongLong(client *c, int idx, long long *ll) {
    sds s = objectGetVal(c->argv[idx]);
    if (string2ll(s, sdslen(s), ll)) return 1;
    jsonReplyError(c, JSON_MSG_VALUE_NOT_INTEGER);
    return 0;
}

/* json.max-path-limit. The AOF, a primary and slot migration replay documents
 * that RDB loading may have accepted deeper, so they get the loading limit. */
static size_t jsonPathLimit(client *c) {
    return mustObeyClient(c) ? JSON_MAX_DEPTH_LIMIT : (size_t)server.json_max_path_limit;
}

static jsonValue *jsonParseArg(client *c, int idx, int *err, size_t *depth) {
    sds s = objectGetVal(c->argv[idx]);
    return jsonParse(s, sdslen(s), jsonPathLimit(c), err, depth);
}

/* json.max-document-size, which the module does not apply to commands from
 * its primary. */
static int jsonSizeLimitExceeded(client *c, size_t size) {
    return server.json_max_document_size > 0 && !mustObeyClient(c) &&
           size > (unsigned long long)server.json_max_document_size;
}

static size_t jsonDocumentSize(client *c, const jsonValue *root) {
    if (server.json_max_document_size <= 0 || mustObeyClient(c)) return 0;
    return jsonMemoryUsage(root);
}

/* Evaluate a read path the way most module reads do: any failure ends a legacy
 * path, while JSONPath only stops on syntax errors and keeps what matched. */
static jsonPathCode jsonEvalRead(jsonPathResult *r, jsonValue *root, const char *path) {
    jsonPathCode rc = jsonPathEval(r, root, path, JSON_PATH_READ);
    if (rc != JSON_PATH_OK && (!r->v2 || jsonPathIsSyntaxError(rc))) return rc;
    return JSON_PATH_OK;
}

static int jsonAnyOfType(const jsonPathResult *r, jsonType t1, jsonType t2) {
    for (size_t i = 0; i < r->len; i++) {
        jsonType t = r->matches[i].value->type;
        if (t == t1 || t == t2) return 1;
    }
    return 0;
}

/* The module's check for legacy paths: there must be a match, and at least one
 * match of the type the command works on. Replies and returns 0 if not. */
static int jsonLegacyCheck(client *c, const jsonPathResult *r, jsonType t1, jsonType t2, const char *msg) {
    if (r->v2) return 1;
    if (r->len == 0) {
        jsonReplyPathError(c, JSON_PATH_ERR_NOT_EXIST);
        return 0;
    }
    if (!jsonAnyOfType(r, t1, t2)) {
        jsonReplyError(c, msg);
        return 0;
    }
    return 1;
}

static int jsonIsNumber(const jsonValue *v) {
    return v->type == JSON_INTEGER || v->type == JSON_NUMBER;
}

typedef struct jsonDepthRef {
    size_t depth;
    size_t pos;
} jsonDepthRef;

static int jsonDepthRefCompare(const void *a, const void *b) {
    const jsonDepthRef *x = a, *y = b;
    if (x->depth != y->depth) return x->depth > y->depth ? -1 : 1;
    return x->pos < y->pos ? -1 : (x->pos > y->pos);
}

/* Match positions deepest first, keeping the original order between equal
 * depths. The module mutates in this order so that a container is changed
 * only after everything matched inside it. */
static size_t *jsonDeepestFirst(const jsonPathResult *r) {
    jsonDepthRef *refs = zmalloc(sizeof(jsonDepthRef) * (r->len ? r->len : 1));
    size_t *order = zmalloc(sizeof(size_t) * (r->len ? r->len : 1));
    for (size_t i = 0; i < r->len; i++) {
        refs[i].depth = r->matches[i].depth;
        refs[i].pos = i;
    }
    qsort(refs, r->len, sizeof(jsonDepthRef), jsonDepthRefCompare);
    for (size_t i = 0; i < r->len; i++) order[i] = refs[i].pos;
    zfree(refs);
    return order;
}

/* printf("%.17g"), the module's rendering of arithmetic results. */
static int jsonFormatResult(double d, char *buf, size_t len) {
    return snprintf(buf, len, "%.17g", d);
}

/* Reply for commands whose JSONPath form returns one integer or null per
 * match, and whose legacy form returns the first (or last) non-null one. */
static void jsonReplyIntegers(client *c, const jsonPathResult *r, const long long *vals, const unsigned char *isnull, int last) {
    if (!r->v2) {
        for (size_t k = 0; k < r->len; k++) {
            size_t i = last ? r->len - 1 - k : k;
            if (!isnull[i]) {
                addReplyLongLong(c, vals[i]);
                return;
            }
        }
        serverPanic("JSON legacy reply without a value");
    }
    addReplyArrayLen(c, r->len);
    for (size_t i = 0; i < r->len; i++) {
        if (isnull[i])
            addReplyNull(c);
        else
            addReplyLongLong(c, vals[i]);
    }
}

/* ----------------------------------------------------------------------------
 * JSON.GET, JSON.MGET
 * ------------------------------------------------------------------------- */

static int jsonHasFormat(const jsonFormat *f) {
    return f->indent || f->newline || f->space;
}

static sds jsonCatFormat(sds s, const char *part) {
    return part ? sdscat(s, part) : s;
}

/* The values a JSONPath matched, as a JSON array laid out like the module's
 * build_json_array. */
static sds jsonCatMatches(sds s, const jsonPathResult *r, const jsonFormat *fmt, size_t level) {
    if (fmt) s = jsonCatFormat(s, fmt->newline);
    for (size_t i = 0; i < r->len; i++) {
        if (fmt)
            for (size_t l = 0; l < level; l++) s = jsonCatFormat(s, fmt->indent);
        s = jsonSerialize(s, r->matches[i].value, fmt, level);
        if (i + 1 < r->len) s = sdscatlen(s, ",", 1);
        if (fmt) s = jsonCatFormat(s, fmt->newline);
    }
    return s;
}

/* The text JSON.GET replies with for one path, or the error. */
static jsonPathCode jsonGetPath(jsonValue *root, const char *path, const jsonFormat *fmt, sds *out) {
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonEvalRead(&r, root, path);
    if (rc == JSON_PATH_OK) {
        if (!r.v2) {
            if (r.len == 0)
                rc = JSON_PATH_ERR_NOT_EXIST;
            else
                *out = jsonSerialize(sdsempty(), r.matches[0].value, fmt, 0);
        } else if (r.len == 0) {
            *out = sdsnewlen("[]", 2);
        } else {
            sds s = sdsnewlen("[", 1);
            s = jsonCatMatches(s, &r, fmt, 1);
            *out = sdscatlen(s, "]", 1);
        }
    }
    jsonPathFree(&r);
    return rc;
}

/* Several paths: an object keyed by path. One result is reused across the
 * paths, as the module reuses one selector, and it is JSONPath throughout if
 * any path is. */
static jsonPathCode jsonGetPaths(jsonValue *root, const char **paths, int npaths, const jsonFormat *fmt, sds *out) {
    jsonPathResult r;
    int v2 = 0;
    for (int i = 0; i < npaths; i++) {
        if (paths[i][0] == '$') v2 = 1;
    }
    jsonPathInit(&r);
    r.v2 = v2;
    jsonPathCode rc = JSON_PATH_OK;
    sds s = sdsnewlen("{", 1);
    if (fmt) s = jsonCatFormat(s, fmt->newline);
    for (int i = 0; i < npaths; i++) {
        rc = jsonPathEval(&r, root, paths[i], JSON_PATH_READ);
        if (rc != JSON_PATH_OK && (!v2 || jsonPathIsSyntaxError(rc))) break;
        rc = JSON_PATH_OK;
        if (!v2 && r.len == 0) {
            rc = JSON_PATH_ERR_NOT_EXIST;
            break;
        }
        if (fmt) s = jsonCatFormat(s, fmt->indent);
        s = jsonCatString(s, paths[i], strlen(paths[i]));
        s = sdscatlen(s, ":", 1);
        if (fmt) s = jsonCatFormat(s, fmt->space);
        if (!v2) {
            s = jsonSerialize(s, r.matches[0].value, fmt, 1);
        } else {
            s = sdscatlen(s, "[", 1);
            s = jsonCatMatches(s, &r, fmt, 2);
            if (fmt) s = jsonCatFormat(s, fmt->indent);
            s = sdscatlen(s, "]", 1);
        }
        if (i + 1 < npaths) s = sdscatlen(s, ",", 1);
        if (fmt) s = jsonCatFormat(s, fmt->newline);
    }
    jsonPathFree(&r);
    if (rc != JSON_PATH_OK) {
        sdsfree(s);
        return rc;
    }
    *out = sdscatlen(s, "}", 1);
    return JSON_PATH_OK;
}

/* JSON.GET key [INDENT indent] [NEWLINE newline] [SPACE space] [NOESCAPE] [path ...] */
void jsonGetCommand(client *c) {
    jsonFormat f = {0};
    const char **paths = zmalloc(sizeof(char *) * c->argc);
    int npaths = 0;

    for (int i = 2; i < c->argc; i++) {
        const char *arg = objectGetVal(c->argv[i]);
        const char **opt = NULL;
        size_t *optlen = NULL;
        if (!strcasecmp(arg, "NEWLINE")) {
            opt = &f.newline, optlen = &f.newline_len;
        } else if (!strcasecmp(arg, "SPACE")) {
            opt = &f.space, optlen = &f.space_len;
        } else if (!strcasecmp(arg, "INDENT")) {
            opt = &f.indent, optlen = &f.indent_len;
        } else if (!strcasecmp(arg, "NOESCAPE")) {
            continue;
        } else {
            paths[npaths++] = arg;
            continue;
        }
        if (i == c->argc - 1) {
            zfree(paths);
            jsonReplyError(c, JSON_MSG_SYNTAX);
            return;
        }
        /* C strings, as the module reads them. */
        *opt = objectGetVal(c->argv[++i]);
        *optlen = strlen(*opt);
    }

    jsonValue *root = jsonLookupOrNull(c, c->argv[1]);
    if (root == NULL) {
        zfree(paths);
        return;
    }
    const jsonFormat *fmt = jsonHasFormat(&f) ? &f : NULL;
    sds out = NULL;
    jsonPathCode rc;
    if (npaths <= 1)
        rc = jsonGetPath(root, npaths ? paths[0] : ".", fmt, &out);
    else
        rc = jsonGetPaths(root, paths, npaths, fmt, &out);
    zfree(paths);
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
        return;
    }
    addReplyBulkSds(c, out);
}

/* JSON.MGET key [key ...] path */
void jsonMgetCommand(client *c) {
    int nkeys = c->argc - 2;
    const char *path = jsonArgPath(c, c->argc - 1);
    sds *outs = zcalloc(sizeof(sds) * nkeys);
    int i;

    for (i = 0; i < nkeys; i++) {
        robj *o;
        int status = jsonLookup(c, c->argv[i + 1], 0, &o);
        if (status == JSON_KEY_MISSING) continue;
        if (status == JSON_KEY_WRONGTYPE) {
            jsonReplyError(c, JSON_MSG_NOT_DOC);
            break;
        }
        jsonPathCode rc = jsonGetPath(objectGetVal(o), path, NULL, &outs[i]);
        if (rc != JSON_PATH_OK && rc != JSON_PATH_ERR_INVALID_PATH && rc != JSON_PATH_ERR_NOT_EXIST) {
            jsonReplyPathError(c, rc);
            break;
        }
    }
    if (i == nkeys) {
        addReplyArrayLen(c, nkeys);
        for (i = 0; i < nkeys; i++) {
            if (outs[i]) {
                addReplyBulkSds(c, outs[i]);
                outs[i] = NULL;
            } else {
                addReplyNull(c);
            }
        }
    }
    for (i = 0; i < nkeys; i++) sdsfree(outs[i]);
    zfree(outs);
}

/* ----------------------------------------------------------------------------
 * JSON.SET, JSON.MSET
 * ------------------------------------------------------------------------- */

/* Set the value at a non-root path of an existing document, as the module's
 * dom_set_value does. Returns 1 if the document was written, else replies
 * unless reply is 0. NX and XX unsatisfied reply null. */
static int jsonSetAtPath(client *c, jsonValue *root, const char *path, int value_idx, int nx, int xx, int reply) {
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonPathEval(&r, root, path, JSON_PATH_WRITE);
    int ok = 0;
    jsonValue *v = NULL;
    int err;
    size_t depth;

    if (rc != JSON_PATH_OK) {
        if (reply) jsonReplyPathError(c, rc);
    } else if ((nx && r.len > 0) || (xx && r.ninserts > 0)) {
        if (reply) addReplyNull(c);
    } else if ((v = jsonParseArg(c, value_idx, &err, &depth)) == NULL) {
        if (reply) jsonReplyParseError(c, err);
    } else if (r.max_depth + depth > jsonPathLimit(c)) {
        if (reply) jsonReplyError(c, JSON_MSG_PATH_LIMIT);
    } else if (jsonSizeLimitExceeded(c, jsonDocumentSize(c, root) + jsonDocumentSize(c, v))) {
        if (reply) jsonReplyError(c, JSON_MSG_SIZE_LIMIT);
    } else {
        /* Nothing to do is still a success. */
        jsonPathCommit(root, &r, v);
        v = NULL;
        ok = 1;
    }
    jsonFree(v);
    jsonPathFree(&r);
    return ok;
}

/* Parse a whole new document, with the size limit the module applies there.
 * Replies and returns NULL on failure unless reply is 0. */
static jsonValue *jsonParseDocument(client *c, int idx, int reply) {
    int err;
    jsonValue *v = jsonParseArg(c, idx, &err, NULL);
    if (v == NULL) {
        if (reply) jsonReplyParseError(c, err);
        return NULL;
    }
    if (jsonSizeLimitExceeded(c, jsonDocumentSize(c, v))) {
        jsonFree(v);
        if (reply) jsonReplyError(c, JSON_MSG_SIZE_LIMIT);
        return NULL;
    }
    return v;
}

static void jsonSetKey(client *c, robj *key, jsonValue *root) {
    robj *o = createJsonObject(root);
    /* Replacing the document drops the TTL, as with the module. The caller
     * signals the change. */
    setKey(c, c->db, key, &o, SETKEY_NO_SIGNAL);
}

/* JSON.SET key path json [NX | XX] */
void jsonSetCommand(client *c) {
    int nx = 0, xx = 0;
    if (c->argc > 5) {
        addReplyErrorArity(c);
        return;
    }
    if (c->argc == 5) {
        const char *cond = objectGetVal(c->argv[4]);
        if (!strcasecmp(cond, "NX")) {
            nx = 1;
        } else if (!strcasecmp(cond, "XX")) {
            xx = 1;
        } else {
            jsonReplyError(c, JSON_MSG_SYNTAX);
            return;
        }
    }

    robj *o;
    int status = jsonLookup(c, c->argv[1], 1, &o);
    if (status == JSON_KEY_WRONGTYPE) {
        jsonReplyError(c, JSON_MSG_NOT_DOC);
        return;
    }
    const char *path = jsonArgPath(c, 2);
    int root_path = jsonPathIsRoot(path);
    if (status == JSON_KEY_MISSING) {
        if (!root_path) {
            jsonReplyError(c, JSON_MSG_NEW_KEY_NOT_ROOT);
            return;
        }
        if (xx) {
            addReplyNull(c);
            return;
        }
    } else if (root_path && nx) {
        addReplyNull(c);
        return;
    }

    if (root_path) {
        jsonValue *root = jsonParseDocument(c, 3, 1);
        if (root == NULL) return;
        jsonSetKey(c, c->argv[1], root);
    } else if (!jsonSetAtPath(c, objectGetVal(o), path, 3, nx, xx, 1)) {
        return;
    }
    jsonKeyModified(c, c->argv[1], "json.set");
    addReply(c, shared.ok);
}

/* JSON.MSET key path json [key path json ...]. Every triple is validated
 * against the keyspace as it was, then each is applied on a best-effort basis:
 * one that no longer applies after an earlier one is skipped. */
void jsonMsetCommand(client *c) {
    if ((c->argc - 1) % 3 != 0) {
        addReplyErrorArity(c);
        return;
    }
    for (int i = 1; i < c->argc; i += 3) {
        robj *o;
        int status = jsonLookup(c, c->argv[i], 1, &o);
        if (status == JSON_KEY_WRONGTYPE) {
            jsonReplyError(c, JSON_MSG_NOT_DOC);
            return;
        }
        const char *path = jsonArgPath(c, i + 1);
        if (jsonPathIsRoot(path)) {
            jsonValue *v = jsonParseDocument(c, i + 2, 1);
            if (v == NULL) return;
            jsonFree(v);
            continue;
        }
        if (status == JSON_KEY_MISSING) {
            jsonReplyError(c, JSON_MSG_SYNTAX);
            return;
        }

        jsonValue *root = objectGetVal(o);
        jsonPathResult r;
        jsonPathInit(&r);
        jsonPathCode rc = jsonPathEval(&r, root, path, JSON_PATH_WRITE);
        int err;
        size_t depth;
        jsonValue *v = NULL;
        const char *msg = NULL;
        if (rc != JSON_PATH_OK) {
            msg = jsonPathErrorMessage(rc);
        } else if ((v = jsonParseArg(c, i + 2, &err, &depth)) == NULL) {
            msg = jsonErrorMessage(err);
        } else if (r.max_depth + depth > jsonPathLimit(c)) {
            msg = JSON_MSG_PATH_LIMIT;
        } else if (jsonSizeLimitExceeded(c, jsonDocumentSize(c, root) + jsonDocumentSize(c, v))) {
            msg = JSON_MSG_SIZE_LIMIT;
        }
        jsonFree(v);
        jsonPathFree(&r);
        if (msg) {
            jsonReplyError(c, msg);
            return;
        }
    }

    for (int i = 1; i < c->argc; i += 3) {
        robj *o;
        int status = jsonLookup(c, c->argv[i], 1, &o);
        if (jsonPathIsRoot(jsonArgPath(c, i + 1))) {
            jsonValue *root = jsonParseDocument(c, i + 2, 0);
            if (root == NULL) continue;
            jsonSetKey(c, c->argv[i], root);
        } else if (status != JSON_KEY_OK || !jsonSetAtPath(c, objectGetVal(o), jsonArgPath(c, i + 1), i + 2, 0, 0, 0)) {
            continue;
        }
        jsonKeyModified(c, c->argv[i], "json.mset");
    }
    addReply(c, shared.ok);
}

/* ----------------------------------------------------------------------------
 * JSON.DEL, JSON.FORGET
 * ------------------------------------------------------------------------- */

/* JSON.DEL key [path] */
void jsonDelCommand(client *c) {
    if (c->argc > 3) {
        addReplyErrorArity(c);
        return;
    }
    robj *o;
    int status = jsonLookup(c, c->argv[1], 1, &o);
    if (status == JSON_KEY_MISSING) {
        addReplyLongLong(c, 0);
        return;
    }
    if (status == JSON_KEY_WRONGTYPE) {
        jsonReplyError(c, JSON_MSG_NOT_DOC);
        return;
    }
    const char *path = jsonOptionalPath(c);
    if (jsonPathIsRoot(path)) {
        dbDelete(c->db, c->argv[1]);
        jsonKeyModified(c, c->argv[1], "json.del");
        addReplyLongLong(c, 1);
        return;
    }

    jsonValue *root = objectGetVal(o);
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonPathEval(&r, root, path, JSON_PATH_DELETE);
    if (rc == JSON_PATH_OK && !r.v2 && r.len == 0) rc = JSON_PATH_ERR_NOT_EXIST;
    if (rc == JSON_PATH_OK && r.len == 0) rc = r.error;
    size_t deleted = rc == JSON_PATH_OK ? jsonPathDeleteMatches(root, &r) : 0;
    jsonPathFree(&r);
    if (rc == JSON_PATH_ERR_INVALID_PATH || rc == JSON_PATH_ERR_NOT_EXIST) {
        addReplyLongLong(c, 0);
        return;
    }
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
        return;
    }
    jsonKeyModified(c, c->argv[1], "json.del");
    addReplyLongLong(c, deleted);
}

/* ----------------------------------------------------------------------------
 * JSON.TYPE
 * ------------------------------------------------------------------------- */

static const char *jsonTypeName(const jsonValue *v) {
    switch (v->type) {
    case JSON_NULL: return "null";
    case JSON_FALSE:
    case JSON_TRUE: return "boolean";
    case JSON_STRING: return "string";
    case JSON_NUMBER: return "number";
    case JSON_INTEGER: return "integer";
    case JSON_OBJECT: return "object";
    case JSON_ARRAY: return "array";
    }
    return "";
}

/* JSON.TYPE key [path] */
void jsonTypeCommand(client *c) {
    if (c->argc > 3) {
        addReplyErrorArity(c);
        return;
    }
    jsonValue *root = jsonLookupOrNull(c, c->argv[1]);
    if (root == NULL) return;
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonEvalRead(&r, root, jsonOptionalPath(c));
    if (rc == JSON_PATH_OK && !r.v2 && r.len == 0) rc = JSON_PATH_ERR_NOT_EXIST;
    if (rc == JSON_PATH_ERR_INVALID_PATH || rc == JSON_PATH_ERR_NOT_EXIST) {
        addReplyNull(c);
    } else if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
    } else if (!r.v2) {
        addReplyStatus(c, jsonTypeName(r.matches[0].value));
    } else {
        addReplyArrayLen(c, r.len);
        for (size_t i = 0; i < r.len; i++) addReplyStatus(c, jsonTypeName(r.matches[i].value));
    }
    jsonPathFree(&r);
}

/* ----------------------------------------------------------------------------
 * JSON.NUMINCRBY, JSON.NUMMULTBY
 * ------------------------------------------------------------------------- */

/* Whether d is exactly an int64_t, as the module's jsonutil_is_int64 decides
 * it on x86, where an out of range conversion yields INT64_MIN. */
static int jsonDoubleIsInt64(double d) {
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) return 0;
    return (double)(int64_t)d == d;
}

static void jsonReplyArithmetic(client *c, const jsonPathResult *r, const double *vals, const unsigned char *isnull) {
    char buf[64];
    if (!r->v2) {
        for (size_t k = r->len; k > 0; k--) {
            if (!isnull[k - 1]) {
                int len = jsonFormatResult(vals[k - 1], buf, sizeof(buf));
                addReplyBulkCBuffer(c, buf, len);
                return;
            }
        }
        serverPanic("JSON legacy arithmetic reply without a number");
    }
    sds s = sdsnewlen("[", 1);
    for (size_t i = 0; i < r->len; i++) {
        if (i) s = sdscatlen(s, ",", 1);
        if (isnull[i]) {
            s = sdscatlen(s, "null", 4);
        } else {
            int len = jsonFormatResult(vals[i], buf, sizeof(buf));
            s = sdscatlen(s, buf, len);
        }
    }
    s = sdscatlen(s, "]", 1);
    addReplyBulkSds(c, s);
}

/* Store a non-integral arithmetic result as the module does: as a number whose
 * text is %.17g of the double. */
static void jsonStoreDouble(jsonValue *v, double d) {
    char buf[64];
    int len = jsonFormatResult(d, buf, sizeof(buf));
    jsonReplace(v, jsonCreateNumber(buf, len));
}

static void jsonArithmeticCommand(client *c, int mult) {
    const char *event = mult ? "json.nummultby" : "json.numincrby";
    int err;
    jsonValue *by = jsonParseArg(c, 3, &err, NULL);
    if (by == NULL || !jsonIsNumber(by)) {
        jsonFree(by);
        jsonReplyError(c, JSON_MSG_VALUE_NOT_NUMBER);
        return;
    }
    jsonValue *root = jsonLookupOrReply(c, c->argv[1], 1);
    if (root == NULL) {
        jsonFree(by);
        return;
    }

    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonPathEval(&r, root, jsonArgPath(c, 2), JSON_PATH_READ);
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
        goto cleanup;
    }
    if (!jsonLegacyCheck(c, &r, JSON_INTEGER, JSON_NUMBER, JSON_MSG_NOT_NUMBER)) goto cleanup;

    jsonPathDedupe(&r);
    double *vals = zmalloc(sizeof(double) * (r.len ? r.len : 1));
    unsigned char *isnull = zcalloc(r.len ? r.len : 1);
    const char *overflow = NULL;
    size_t changed = 0;
    double by_d = jsonGetDouble(by);
    for (size_t i = 0; i < r.len; i++) {
        jsonValue *v = r.matches[i].value;
        if (!jsonIsNumber(v)) {
            isnull[i] = 1;
            continue;
        }
        double res;
        if (!mult) {
            if (v->type == JSON_INTEGER && by->type == JSON_INTEGER) {
                int64_t a = v->integer, b = by->integer;
                if ((a >= 0 && b <= INT64_MAX - a) || (a < 0 && b >= INT64_MIN - a)) {
                    v->integer = a + b;
                    vals[i] = (double)v->integer;
                    changed++;
                    continue;
                }
            }
            res = jsonGetDouble(v) + by_d;
        } else {
            res = jsonGetDouble(v) * by_d;
        }
        /* The module stores a NaN result (infinity times zero, or infinities
         * of opposite sign added), which is not JSON, and asserts replying to a
         * legacy path. Refuse it like an overflow. */
        if (isinf(res) || isnan(res)) {
            overflow = mult ? JSON_MSG_MULT_OVERFLOW : JSON_MSG_ADD_OVERFLOW;
            break;
        }
        if (mult && jsonDoubleIsInt64(res))
            jsonReplace(v, jsonCreateInteger((int64_t)res));
        else
            jsonStoreDouble(v, res);
        vals[i] = res;
        changed++;
    }
    if (overflow) {
        if (changed) jsonKeyPartiallyModified(c, c->argv[1], root);
        jsonReplyError(c, overflow);
    } else {
        jsonKeyModified(c, c->argv[1], event);
        jsonReplyArithmetic(c, &r, vals, isnull);
    }
    zfree(vals);
    zfree(isnull);
cleanup:
    jsonPathFree(&r);
    jsonFree(by);
}

/* JSON.NUMINCRBY key path number */
void jsonNumincrbyCommand(client *c) {
    jsonArithmeticCommand(c, 0);
}

/* JSON.NUMMULTBY key path number */
void jsonNummultbyCommand(client *c) {
    jsonArithmeticCommand(c, 1);
}

/* ----------------------------------------------------------------------------
 * JSON.TOGGLE, JSON.CLEAR
 * ------------------------------------------------------------------------- */

/* JSON.TOGGLE key [path] */
void jsonToggleCommand(client *c) {
    if (c->argc > 3) {
        addReplyErrorArity(c);
        return;
    }
    jsonValue *root = jsonLookupOrReply(c, c->argv[1], 1);
    if (root == NULL) return;
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonEvalRead(&r, root, jsonOptionalPath(c));
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
        goto cleanup;
    }
    if (!jsonLegacyCheck(c, &r, JSON_TRUE, JSON_FALSE, JSON_MSG_NOT_BOOL)) goto cleanup;

    jsonPathDedupe(&r);
    long long *vals = zmalloc(sizeof(long long) * (r.len ? r.len : 1));
    unsigned char *isnull = zcalloc(r.len ? r.len : 1);
    for (size_t i = 0; i < r.len; i++) {
        jsonValue *v = r.matches[i].value;
        if (v->type == JSON_TRUE) {
            v->type = JSON_FALSE;
            vals[i] = 0;
        } else if (v->type == JSON_FALSE) {
            v->type = JSON_TRUE;
            vals[i] = 1;
        } else {
            isnull[i] = 1;
        }
    }
    jsonKeyModified(c, c->argv[1], "json.toggle");
    if (!r.v2) {
        /* Legacy paths reply with the first new value as JSON text. */
        for (size_t i = 0; i < r.len; i++) {
            if (!isnull[i]) {
                if (vals[i])
                    addReplyBulkCBuffer(c, "true", 4);
                else
                    addReplyBulkCBuffer(c, "false", 5);
                break;
            }
        }
    } else {
        jsonReplyIntegers(c, &r, vals, isnull, 0);
    }
    zfree(vals);
    zfree(isnull);
cleanup:
    jsonPathFree(&r);
}

/* Reset one value as JSON.CLEAR does. Returns 1 if it changed. */
static int jsonClearValue(jsonValue *v) {
    switch (v->type) {
    case JSON_ARRAY:
        if (v->array.len == 0) return 0;
        jsonReplace(v, jsonCreateArray());
        return 1;
    case JSON_OBJECT:
        if (v->object.len == 0) return 0;
        jsonReplace(v, jsonCreateObject());
        return 1;
    case JSON_TRUE: v->type = JSON_FALSE; return 1;
    case JSON_STRING:
        if (sdslen(v->string) == 0) return 0;
        jsonReplace(v, jsonCreateString("", 0));
        return 1;
    case JSON_INTEGER:
        if (v->integer == 0) return 0;
        v->integer = 0;
        return 1;
    case JSON_NUMBER:
        if (!(v->number.value < 0.0 || v->number.value > 0.0)) return 0;
        jsonReplace(v, jsonCreateNumber("0.0", 3));
        return 1;
    default: return 0;
    }
}

/* JSON.CLEAR key [path] */
void jsonClearCommand(client *c) {
    if (c->argc > 3) {
        addReplyErrorArity(c);
        return;
    }
    jsonValue *root = jsonLookupOrReply(c, c->argv[1], 1);
    if (root == NULL) return;
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonPathEval(&r, root, jsonOptionalPath(c), JSON_PATH_READ);
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
        jsonPathFree(&r);
        return;
    }
    jsonPathDedupe(&r);
    size_t *order = jsonDeepestFirst(&r);
    long long cleared = 0;
    for (size_t k = 0; k < r.len; k++) cleared += jsonClearValue(r.matches[order[k]].value);
    zfree(order);
    jsonPathFree(&r);
    jsonKeyModified(c, c->argv[1], "json.clear");
    addReplyLongLong(c, cleared);
}

/* ----------------------------------------------------------------------------
 * JSON.STRLEN, JSON.STRAPPEND
 * ------------------------------------------------------------------------- */

/* JSON.STRLEN key [path] */
void jsonStrlenCommand(client *c) {
    if (c->argc > 3) {
        addReplyErrorArity(c);
        return;
    }
    jsonValue *root = jsonLookupOrNull(c, c->argv[1]);
    if (root == NULL) return;
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonEvalRead(&r, root, jsonOptionalPath(c));
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
    } else if (jsonLegacyCheck(c, &r, JSON_STRING, JSON_STRING, JSON_MSG_NOT_STRING)) {
        long long *vals = zmalloc(sizeof(long long) * (r.len ? r.len : 1));
        unsigned char *isnull = zcalloc(r.len ? r.len : 1);
        for (size_t i = 0; i < r.len; i++) {
            jsonValue *v = r.matches[i].value;
            if (v->type == JSON_STRING)
                vals[i] = sdslen(v->string);
            else
                isnull[i] = 1;
        }
        jsonReplyIntegers(c, &r, vals, isnull, 0);
        zfree(vals);
        zfree(isnull);
    }
    jsonPathFree(&r);
}

/* JSON.STRAPPEND key [path] json */
void jsonStrappendCommand(client *c) {
    if (c->argc > 4) {
        addReplyErrorArity(c);
        return;
    }
    int value_idx = c->argc - 1;
    const char *path = c->argc == 4 ? jsonArgPath(c, 2) : ".";
    jsonValue *root = jsonLookupOrReply(c, c->argv[1], 1);
    if (root == NULL) return;
    if (jsonSizeLimitExceeded(c, jsonDocumentSize(c, root) + sdslen(objectGetVal(c->argv[value_idx])))) {
        jsonReplyError(c, JSON_MSG_SIZE_LIMIT);
        return;
    }

    jsonPathResult r;
    jsonPathInit(&r);
    jsonValue *append = NULL;
    int err;
    jsonPathCode rc = jsonPathEval(&r, root, path, JSON_PATH_READ);
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
        goto cleanup;
    }
    if (!jsonLegacyCheck(c, &r, JSON_STRING, JSON_STRING, JSON_MSG_NOT_STRING)) goto cleanup;
    if ((append = jsonParseArg(c, value_idx, &err, NULL)) == NULL) {
        jsonReplyParseError(c, err);
        goto cleanup;
    }
    if (append->type != JSON_STRING) {
        jsonReplyError(c, JSON_MSG_VALUE_NOT_STRING);
        goto cleanup;
    }

    jsonPathDedupe(&r);
    long long *vals = zmalloc(sizeof(long long) * (r.len ? r.len : 1));
    unsigned char *isnull = zcalloc(r.len ? r.len : 1);
    /* The module joins both strings as C strings, so each is cut at its first
     * NUL character. */
    size_t append_len = strlen(append->string);
    for (size_t i = 0; i < r.len; i++) {
        jsonValue *v = r.matches[i].value;
        if (v->type != JSON_STRING) {
            isnull[i] = 1;
            continue;
        }
        sds s = sdsnewlen(v->string, strlen(v->string));
        s = sdscatlen(s, append->string, append_len);
        vals[i] = sdslen(s);
        jsonReplace(v, jsonCreateString(s, sdslen(s)));
        sdsfree(s);
    }
    jsonKeyModified(c, c->argv[1], "json.strappend");
    jsonReplyIntegers(c, &r, vals, isnull, 1);
    zfree(vals);
    zfree(isnull);
cleanup:
    jsonFree(append);
    jsonPathFree(&r);
}

/* ----------------------------------------------------------------------------
 * JSON.OBJLEN, JSON.OBJKEYS
 * ------------------------------------------------------------------------- */

/* JSON.OBJLEN key [path] */
void jsonObjlenCommand(client *c) {
    if (c->argc > 3) {
        addReplyErrorArity(c);
        return;
    }
    jsonValue *root = jsonLookupOrNull(c, c->argv[1]);
    if (root == NULL) return;
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonEvalRead(&r, root, jsonOptionalPath(c));
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
    } else if (jsonLegacyCheck(c, &r, JSON_OBJECT, JSON_OBJECT, JSON_MSG_NOT_OBJECT)) {
        long long *vals = zmalloc(sizeof(long long) * (r.len ? r.len : 1));
        unsigned char *isnull = zcalloc(r.len ? r.len : 1);
        for (size_t i = 0; i < r.len; i++) {
            jsonValue *v = r.matches[i].value;
            if (v->type == JSON_OBJECT)
                vals[i] = v->object.len;
            else
                isnull[i] = 1;
        }
        jsonReplyIntegers(c, &r, vals, isnull, 0);
        zfree(vals);
        zfree(isnull);
    }
    jsonPathFree(&r);
}

static void jsonReplyMemberNames(client *c, const jsonValue *v) {
    size_t n = v->type == JSON_OBJECT ? v->object.len : 0;
    addReplyArrayLen(c, n);
    for (size_t i = 0; i < n; i++) {
        sds name = v->object.members[i].name;
        addReplyBulkCBuffer(c, name, sdslen(name));
    }
}

/* JSON.OBJKEYS key [path] */
void jsonObjkeysCommand(client *c) {
    if (c->argc > 3) {
        addReplyErrorArity(c);
        return;
    }
    jsonValue *root = jsonLookupOrNull(c, c->argv[1]);
    if (root == NULL) return;
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonEvalRead(&r, root, jsonOptionalPath(c));
    if (rc == JSON_PATH_OK && !r.v2 && r.len == 0) rc = JSON_PATH_ERR_NOT_EXIST;
    if (rc == JSON_PATH_ERR_INVALID_PATH || rc == JSON_PATH_ERR_NOT_EXIST) {
        addReplyNull(c);
    } else if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
    } else if (!r.v2 && !jsonAnyOfType(&r, JSON_OBJECT, JSON_OBJECT)) {
        jsonReplyError(c, JSON_MSG_NOT_OBJECT);
    } else if (!r.v2) {
        /* The first object that has members, else an empty array. */
        size_t i;
        for (i = 0; i < r.len; i++) {
            jsonValue *v = r.matches[i].value;
            if (v->type == JSON_OBJECT && v->object.len) break;
        }
        if (i < r.len)
            jsonReplyMemberNames(c, r.matches[i].value);
        else
            addReplyArrayLen(c, 0);
    } else {
        addReplyArrayLen(c, r.len);
        for (size_t i = 0; i < r.len; i++) jsonReplyMemberNames(c, r.matches[i].value);
    }
    jsonPathFree(&r);
}

/* ----------------------------------------------------------------------------
 * Array commands
 * ------------------------------------------------------------------------- */

/* JSON.ARRLEN key [path] */
void jsonArrlenCommand(client *c) {
    if (c->argc > 3) {
        addReplyErrorArity(c);
        return;
    }
    jsonValue *root = jsonLookupOrNull(c, c->argv[1]);
    if (root == NULL) return;
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonEvalRead(&r, root, jsonOptionalPath(c));
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
    } else if (jsonLegacyCheck(c, &r, JSON_ARRAY, JSON_ARRAY, JSON_MSG_NOT_ARRAY)) {
        long long *vals = zmalloc(sizeof(long long) * (r.len ? r.len : 1));
        unsigned char *isnull = zcalloc(r.len ? r.len : 1);
        for (size_t i = 0; i < r.len; i++) {
            jsonValue *v = r.matches[i].value;
            if (v->type == JSON_ARRAY)
                vals[i] = v->array.len;
            else
                isnull[i] = 1;
        }
        jsonReplyIntegers(c, &r, vals, isnull, 0);
        zfree(vals);
        zfree(isnull);
    }
    jsonPathFree(&r);
}

/* Parse the values JSON.ARRAPPEND and JSON.ARRINSERT add, with the module's
 * per value nesting check and total size check. Replies and returns NULL on
 * failure. */
static jsonValue **jsonParseValues(client *c, int first, const jsonPathResult *r, jsonValue *root) {
    int n = c->argc - first;
    jsonValue **vals = zcalloc(sizeof(jsonValue *) * n);
    size_t total = 0;
    const char *msg = NULL;
    for (int i = 0; i < n && !msg; i++) {
        int err;
        size_t depth;
        vals[i] = jsonParseArg(c, first + i, &err, &depth);
        if (vals[i] == NULL)
            msg = jsonErrorMessage(err);
        else if (r->max_depth + depth > jsonPathLimit(c))
            msg = JSON_MSG_PATH_LIMIT;
        else
            total += jsonDocumentSize(c, vals[i]);
    }
    if (!msg && jsonSizeLimitExceeded(c, jsonDocumentSize(c, root) + total)) msg = JSON_MSG_SIZE_LIMIT;
    if (msg) {
        for (int i = 0; i < n; i++) jsonFree(vals[i]);
        zfree(vals);
        jsonReplyError(c, msg);
        return NULL;
    }
    return vals;
}

static void jsonFreeValues(jsonValue **vals, int n) {
    for (int i = 0; i < n; i++) jsonFree(vals[i]);
    zfree(vals);
}

/* JSON.ARRAPPEND key path json [json ...] */
void jsonArrappendCommand(client *c) {
    jsonValue *root = jsonLookupOrReply(c, c->argv[1], 1);
    if (root == NULL) return;
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonPathEval(&r, root, jsonArgPath(c, 2), JSON_PATH_READ);
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
        jsonPathFree(&r);
        return;
    }
    jsonValue **vals;
    if (!jsonLegacyCheck(c, &r, JSON_ARRAY, JSON_ARRAY, JSON_MSG_NOT_ARRAY) ||
        (vals = jsonParseValues(c, 3, &r, root)) == NULL) {
        jsonPathFree(&r);
        return;
    }
    int n = c->argc - 3;

    jsonPathDedupe(&r);
    long long *lens = zmalloc(sizeof(long long) * (r.len ? r.len : 1));
    unsigned char *isnull = zcalloc(r.len ? r.len : 1);
    size_t *order = jsonDeepestFirst(&r);
    for (size_t k = 0; k < r.len; k++) {
        size_t i = order[k];
        jsonValue *arr = r.matches[i].value;
        if (arr->type != JSON_ARRAY) {
            isnull[i] = 1;
            continue;
        }
        for (int j = 0; j < n; j++) jsonArrayAppend(arr, jsonDup(vals[j]));
        lens[i] = arr->array.len;
    }
    jsonKeyModified(c, c->argv[1], "json.arrappend");
    jsonReplyIntegers(c, &r, lens, isnull, 0);
    zfree(order);
    zfree(lens);
    zfree(isnull);
    jsonFreeValues(vals, n);
    jsonPathFree(&r);
}

/* JSON.ARRINSERT key path index json [json ...] */
void jsonArrinsertCommand(client *c) {
    long long index;
    if (!jsonArgLongLong(c, 3, &index)) return;
    jsonValue *root = jsonLookupOrReply(c, c->argv[1], 1);
    if (root == NULL) return;
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonPathEval(&r, root, jsonArgPath(c, 2), JSON_PATH_READ);
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
        jsonPathFree(&r);
        return;
    }
    jsonValue **vals;
    if (!jsonLegacyCheck(c, &r, JSON_ARRAY, JSON_ARRAY, JSON_MSG_NOT_ARRAY) ||
        (vals = jsonParseValues(c, 4, &r, root)) == NULL) {
        jsonPathFree(&r);
        return;
    }
    int n = c->argc - 4;

    jsonPathDedupe(&r);
    long long *lens = zmalloc(sizeof(long long) * (r.len ? r.len : 1));
    unsigned char *isnull = zcalloc(r.len ? r.len : 1);
    size_t *order = jsonDeepestFirst(&r);
    size_t changed = 0;
    int out_of_bounds = 0;
    for (size_t k = 0; k < r.len; k++) {
        size_t i = order[k];
        jsonValue *arr = r.matches[i].value;
        if (arr->type != JSON_ARRAY) {
            isnull[i] = 1;
            continue;
        }
        long long size = arr->array.len, at = index;
        if (at < 0) at = size == 0 ? 0 : size + at;
        if (at < 0 || at > size) {
            /* Arrays already changed stay changed, as in the module. */
            out_of_bounds = 1;
            break;
        }
        for (int j = 0; j < n; j++) jsonArrayInsert(arr, at + j, jsonDup(vals[j]));
        lens[i] = arr->array.len;
        changed++;
    }
    if (out_of_bounds) {
        if (changed) jsonKeyPartiallyModified(c, c->argv[1], root);
        jsonReplyError(c, JSON_MSG_OUT_OF_BOUNDS);
    } else {
        jsonKeyModified(c, c->argv[1], "json.arrinsert");
        jsonReplyIntegers(c, &r, lens, isnull, 0);
    }
    zfree(order);
    zfree(lens);
    zfree(isnull);
    jsonFreeValues(vals, n);
    jsonPathFree(&r);
}

/* JSON.ARRPOP key [path [index]] */
void jsonArrpopCommand(client *c) {
    long long index = -1;
    if (c->argc > 4) {
        addReplyErrorArity(c);
        return;
    }
    if (c->argc > 3 && !jsonArgLongLong(c, 3, &index)) return;
    jsonValue *root = jsonLookupOrReply(c, c->argv[1], 1);
    if (root == NULL) return;
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonPathEval(&r, root, jsonOptionalPath(c), JSON_PATH_READ);
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
        jsonPathFree(&r);
        return;
    }
    if (!jsonLegacyCheck(c, &r, JSON_ARRAY, JSON_ARRAY, JSON_MSG_NOT_ARRAY)) {
        jsonPathFree(&r);
        return;
    }

    jsonPathDedupe(&r);
    sds *popped = zcalloc(sizeof(sds) * (r.len ? r.len : 1));
    size_t *order = jsonDeepestFirst(&r);
    for (size_t k = 0; k < r.len; k++) {
        size_t i = order[k];
        jsonValue *arr = r.matches[i].value;
        if (arr->type != JSON_ARRAY || arr->array.len == 0) continue;
        long long size = arr->array.len, at = index;
        if (at < 0) at = size + at;
        if (at >= size) at = size - 1;
        if (at < 0) at = 0;
        jsonValue *v = jsonArrayDetach(arr, at);
        popped[i] = jsonSerialize(sdsempty(), v, NULL, 0);
        jsonFree(v);
    }
    zfree(order);
    jsonKeyModified(c, c->argv[1], "json.arrpop");
    if (!r.v2) {
        size_t i;
        for (i = 0; i < r.len && popped[i] == NULL; i++);
        if (i < r.len)
            addReplyBulkCBuffer(c, popped[i], sdslen(popped[i]));
        else
            addReplyNull(c);
    } else {
        addReplyArrayLen(c, r.len);
        for (size_t i = 0; i < r.len; i++) {
            if (popped[i])
                addReplyBulkCBuffer(c, popped[i], sdslen(popped[i]));
            else
                addReplyNull(c);
        }
    }
    for (size_t i = 0; i < r.len; i++) sdsfree(popped[i]);
    zfree(popped);
    jsonPathFree(&r);
}

/* JSON.ARRTRIM key path start stop, both inclusive. */
void jsonArrtrimCommand(client *c) {
    long long start, stop;
    if (!jsonArgLongLong(c, 3, &start) || !jsonArgLongLong(c, 4, &stop)) return;
    jsonValue *root = jsonLookupOrReply(c, c->argv[1], 1);
    if (root == NULL) return;
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonPathEval(&r, root, jsonArgPath(c, 2), JSON_PATH_READ);
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
        jsonPathFree(&r);
        return;
    }
    if (!jsonLegacyCheck(c, &r, JSON_ARRAY, JSON_ARRAY, JSON_MSG_NOT_ARRAY)) {
        jsonPathFree(&r);
        return;
    }

    jsonPathDedupe(&r);
    long long *lens = zmalloc(sizeof(long long) * (r.len ? r.len : 1));
    unsigned char *isnull = zcalloc(r.len ? r.len : 1);
    size_t *order = jsonDeepestFirst(&r);
    for (size_t k = 0; k < r.len; k++) {
        size_t i = order[k];
        jsonValue *arr = r.matches[i].value;
        if (arr->type != JSON_ARRAY) {
            isnull[i] = 1;
            continue;
        }
        long long size = arr->array.len, from = start, to = stop;
        lens[i] = 0;
        if (size == 0) continue;
        if (from < 0) from = 0;
        if (to >= size) to = size - 1;
        if (from >= size || from > to) {
            jsonArrayDeleteRange(arr, 0, size);
            continue;
        }
        if (to < size - 1) jsonArrayDeleteRange(arr, to + 1, size - to - 1);
        if (from > 0) jsonArrayDeleteRange(arr, 0, from);
        lens[i] = arr->array.len;
    }
    jsonKeyModified(c, c->argv[1], "json.arrtrim");
    jsonReplyIntegers(c, &r, lens, isnull, 0);
    zfree(order);
    zfree(lens);
    zfree(isnull);
    jsonPathFree(&r);
}

/* RapidJSON's operator==: numbers compare as doubles unless both are
 * integers, objects compare by member name regardless of order. */
static int jsonValueEqual(const jsonValue *a, const jsonValue *b) {
    if (jsonIsNumber(a) && jsonIsNumber(b)) {
        if (a->type == JSON_INTEGER && b->type == JSON_INTEGER) return a->integer == b->integer;
        double x = jsonGetDouble(a), y = jsonGetDouble(b);
        return x >= y && x <= y;
    }
    if (a->type != b->type) return 0;
    switch (a->type) {
    case JSON_STRING: return sdslen(a->string) == sdslen(b->string) && !memcmp(a->string, b->string, sdslen(a->string));
    case JSON_ARRAY:
        if (a->array.len != b->array.len) return 0;
        for (size_t i = 0; i < a->array.len; i++) {
            if (!jsonValueEqual(a->array.items[i], b->array.items[i])) return 0;
        }
        return 1;
    case JSON_OBJECT:
        if (a->object.len != b->object.len) return 0;
        for (size_t i = 0; i < a->object.len; i++) {
            const jsonMember *m = &a->object.members[i];
            const jsonValue *other = jsonObjectFind(b, m->name, sdslen(m->name), NULL);
            if (other == NULL || !jsonValueEqual(m->value, other)) return 0;
        }
        return 1;
    default: return 1;
    }
}

/* JSON.ARRINDEX key path value [start [stop]], stop exclusive with 0 and -1
 * meaning the end. */
void jsonArrindexCommand(client *c) {
    long long start = 0, stop = 0;
    if (c->argc > 6) {
        addReplyErrorArity(c);
        return;
    }
    if (c->argc > 4 && !jsonArgLongLong(c, 4, &start)) return;
    if (c->argc > 5 && !jsonArgLongLong(c, 5, &stop)) return;
    jsonValue *root = jsonLookupOrReply(c, c->argv[1], 0);
    if (root == NULL) return;
    jsonPathResult r;
    jsonPathInit(&r);
    jsonValue *needle = NULL;
    int err;
    jsonPathCode rc = jsonEvalRead(&r, root, jsonArgPath(c, 2));
    if (rc != JSON_PATH_OK) {
        jsonReplyPathError(c, rc);
        goto cleanup;
    }
    if (!jsonLegacyCheck(c, &r, JSON_ARRAY, JSON_ARRAY, JSON_MSG_NOT_ARRAY)) goto cleanup;
    if (start < 0) start = 0;
    if ((needle = jsonParseArg(c, 3, &err, NULL)) == NULL) {
        jsonReplyParseError(c, err);
        goto cleanup;
    }

    long long *found = zmalloc(sizeof(long long) * (r.len ? r.len : 1));
    unsigned char *isnull = zcalloc(r.len ? r.len : 1);
    for (size_t i = 0; i < r.len; i++) {
        jsonValue *arr = r.matches[i].value;
        if (arr->type != JSON_ARRAY) {
            isnull[i] = 1;
            continue;
        }
        long long size = arr->array.len, end = stop;
        found[i] = -1;
        if (size == 0) continue;
        if (end == 0 || end == -1) end = size;
        if (end > size) end = size;
        for (long long j = start; j < end; j++) {
            if (jsonValueEqual(arr->array.items[j], needle)) {
                found[i] = j;
                break;
            }
        }
    }
    jsonReplyIntegers(c, &r, found, isnull, 0);
    zfree(found);
    zfree(isnull);
cleanup:
    jsonFree(needle);
    jsonPathFree(&r);
}
