/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* JSONPath and legacy path evaluation for the native JSON type.
 *
 * This is a port of valkey-json's Selector. The module parses the path while it
 * walks the document, so which errors a path reports depends on the document:
 * a malformed tail after a step that matched nothing is never parsed, and an
 * error inside one branch of a wildcard only ends that branch. Evaluation here
 * keeps that interleaving so every command sees the module's result codes.
 *
 * A path is JSONPath (v2) if it starts with '$', legacy otherwise. The path is
 * a C string: an embedded NUL ends it, as it does in the module. */

#ifndef __JSON_PATH_H
#define __JSON_PATH_H

#include <stddef.h>

#include "json.h"
#include "sds.h"

/* Result codes. Each is one of the module's JsonUtilCode values and
 * jsonPathErrorMessage() returns the module's reply text for it. */
typedef enum jsonPathCode {
    JSON_PATH_OK = 0,
    JSON_PATH_ERR_INVALID_PATH,
    JSON_PATH_ERR_INVALID_WILDCARD,
    JSON_PATH_ERR_INVALID_MEMBER_NAME,
    JSON_PATH_ERR_INVALID_NUMBER,
    JSON_PATH_ERR_INVALID_IDENTIFIER,
    JSON_PATH_ERR_INVALID_DOT_SEQUENCE,
    JSON_PATH_ERR_EMPTY_EXPR_TOKEN,
    JSON_PATH_ERR_INDEX_NOT_NUMBER,
    JSON_PATH_ERR_STEP_ZERO,
    JSON_PATH_ERR_NOT_EXIST,
    JSON_PATH_ERR_NOT_OBJECT,
    JSON_PATH_ERR_NOT_ARRAY,
    JSON_PATH_ERR_OUT_OF_BOUNDS,
    JSON_PATH_ERR_JSON_PARSE,
    JSON_PATH_ERR_RECURSION_LIMIT,
    JSON_PATH_ERR_DESCENT_LIMIT,
    JSON_PATH_ERR_QUERY_SIZE_LIMIT,
    JSON_PATH_ERR_INSERT_NON_OBJECT,
    JSON_PATH_ERR_DOLLAR_NON_ROOT,
} jsonPathCode;

/* Evaluation mode. Reads never fail on a wrong-typed step. Writes (JSON.SET and
 * JSON.MERGE) fail on a member step into a non-object and on a missing member
 * that is not the last step, and record a missing last member as an insert.
 * Deletes fail on a member step into a non-object. */
typedef enum jsonPathMode {
    JSON_PATH_READ = 0,
    JSON_PATH_WRITE,
    JSON_PATH_DELETE,
} jsonPathMode;

typedef struct jsonPathMatch {
    jsonValue *value;
    jsonValue *parent; /* NULL for the root. */
    size_t index;      /* Array index or member position within parent. */
    size_t depth;      /* Steps from the root. */
    sds pointer;       /* JSON Pointer to value, set in write and delete modes. */
} jsonPathMatch;

typedef struct jsonPathResult {
    jsonPathMatch *matches; /* In module order, duplicates included. */
    size_t len;
    size_t cap;
    /* Write mode: members to create, as JSON Pointers built like the module's,
     * with the new member name appended unescaped. Sorted and unique. */
    sds *inserts;
    size_t ninserts;
    size_t inserts_cap;
    size_t max_depth; /* Deepest step reached, for the nesting limit on writes. */
    int error;        /* Error recorded while exploring a branch, see jsonPathCommit. */
    /* These two survive jsonPathEval, as they survive a reused selector in the
     * module's multi-path JSON.GET. */
    int v2;           /* Set once a '$' was parsed at the root, or forced. */
    size_t rd_tokens; /* '..' tokens lexed, against the limit of 20. */
} jsonPathResult;

void jsonPathInit(jsonPathResult *r);
void jsonPathFree(jsonPathResult *r);

/* Evaluate path against root, replacing r's matches and inserts. Set r->v2
 * before the call to force JSONPath behaviour on a legacy path. */
jsonPathCode jsonPathEval(jsonPathResult *r, jsonValue *root, const char *path, jsonPathMode mode);

/* Whether an error aborts a JSONPath read, or only ends one branch of it. */
int jsonPathIsSyntaxError(jsonPathCode code);
const char *jsonPathErrorMessage(jsonPathCode code);
int jsonPathIsRoot(const char *path);
int jsonPathIsV2(const char *path);

/* Drop matches whose value appeared earlier, keeping order. */
void jsonPathDedupe(jsonPathResult *r);

/* Commit a write-mode result: replace every unique match, then create every
 * insert. A lone match swaps v in, so the inserts receive its old value; several
 * matches each get a copy, skipping any whose path an earlier one removed.
 * Takes ownership of v and leaves r's value pointers possibly dangling. Returns
 * r->error when there was nothing to do; the module ignores the return value. */
jsonPathCode jsonPathCommit(jsonValue *root, jsonPathResult *r, jsonValue *v);
/* Only the inserts, for JSON.MERGE. Does not take ownership of v. */
jsonPathCode jsonPathCommitInserts(jsonValue *root, jsonPathResult *r, const jsonValue *v);
/* Delete every match of a delete-mode result. Returns how many were removed. */
size_t jsonPathDeleteMatches(jsonValue *root, jsonPathResult *r);

/* RapidJSON Pointer operations, which the module uses to apply writes. A
 * pointer that does not parse is an error for jsonPointerSet and not found for
 * the others. */
jsonValue *jsonPointerGet(jsonValue *root, const char *ptr, size_t len);
int jsonPointerSet(jsonValue *root, const char *ptr, size_t len, jsonValue *v);
int jsonPointerErase(jsonValue *root, const char *ptr, size_t len);

#endif
