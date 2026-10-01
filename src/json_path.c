/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Port of valkey-json's Selector (src/json/selector.cc). Function names and
 * grammar comments follow the module so the two can be read side by side;
 * behaviour, oddities included, must stay identical. The module's grammar:
 *
 *   SupportedPath        ::= ["$" | "."] RelativePath
 *   RelativePath         ::= empty | RecursivePath | DotPath | BracketPath | QualifiedPath
 *   RecursivePath        ::= ".." SupportedPath
 *   DotPath              ::= "." QualifiedPath
 *   QualifiedPath        ::= QualifiedPathElement RelativePath
 *   QualifiedPathElement ::= Key | BracketPathElement
 *   Key                  ::= "*" [ [ "." ] WildcardFilter ] | UnquotedMemberName
 *   WildcardFilter       ::= "[" "?" "(" FilterExpr ")" "]"
 *   BracketPath          ::= BracketPathElement [ RelativePath ]
 *   BracketPathElement   ::= "[" {SPACE} ( WildcardInBrackets | ((NameInBrackets | IndexExpr) ) {SPACE} "]")
 *   WildcardInBrackets   ::= "*" {SPACE} "]" [ "[" {SPACE} "?" "(" FilterExpr ")" {SPACE} "]" ]
 *   NameInBrackets       ::= QuotedMemberName [ ({SPACE} "," {SPACE} QuotedMemberName)+ ]
 *   IndexExpr            ::= Filter | SliceStartsWithColon | SliceOrUnionOrIndex
 *   Filter               ::= "?" "(" FilterExpr ")"
 *   FilterExpr           ::= {SPACE} Term { {SPACE} "||" {SPACE} Term {SPACE} }
 *   Term                 ::= Factor { {SPACE} "&&" {SPACE} Factor }
 *   Factor               ::= ( "@" ( MemberName | ( [ MemberName ] ComparisonOp ComparisonValue) ) ) |
 *                            ( ComparisonValue ComparisonOp "@" ( MemberName | ( [ MemberName ]) ) ) |
 *                            ( {SPACE} "(" FilterExpr ")" {SPACE} )
 *   ComparisonValue      ::= "null" | Bool | Number | QuotedString | PartialPath
 *
 * Parsing and evaluation are interleaved: each parse function acts on the
 * current node as soon as it has read its step, and a step that selects
 * nothing ends the parse of that branch. Branching steps (wildcards, unions,
 * slices, filters, recursive descent) snapshot the lexer and parse the rest of
 * the path again for every branch. */

#include "json_path.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zmalloc.h"

#define JSON_PATH_MAX_RECURSION 200
#define JSON_PATH_MAX_DESCENT_TOKENS 20
#define JSON_PATH_MAX_QUERY_SIZE (128 * 1024)

/* RapidJSON's kPointerInvalidIndex. */
#define PTR_INVALID_INDEX UINT32_MAX

/* ----------------------------------------------------------------------------
 * Lexer
 * ------------------------------------------------------------------------- */

enum {
    TOK_UNKNOWN = 0,
    TOK_DOLLAR,
    TOK_DOT,
    TOK_DOTDOT,
    TOK_WILDCARD,
    TOK_COLON,
    TOK_COMMA,
    TOK_AT,
    TOK_QUESTION_MARK,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_SINGLE_QUOTE,
    TOK_DOUBLE_QUOTE,
    TOK_PLUS,
    TOK_MINUS,
    TOK_DIV,
    TOK_PCT,
    TOK_EQ,
    TOK_NE,
    TOK_GT,
    TOK_LT,
    TOK_GE,
    TOK_LE,
    TOK_NOT,
    TOK_ASSIGN,
    TOK_ALPHA,
    TOK_DIGIT,
    TOK_SPACE,
    TOK_AND,
    TOK_OR,
    TOK_SPECIAL_CHAR,
    TOK_END
};

/* str is the token's text in the path. END keeps the previous token's text,
 * and the scanners that start from str depend on that, as in the module.
 * skipSpaces() can move p past spaces that follow the current token; the
 * scanners then return text that starts at str but whose length counts only
 * the characters read from p, so the text is cut short, as in the module. */
typedef struct pathToken {
    int type;
    const char *str;
} pathToken;

typedef struct pathLexer {
    const char *p; /* Next character to read, after the current token. */
    pathToken next;
    size_t *rd_tokens; /* '..' tokens lexed so far, re-lexing included. */
} pathLexer;

static int isDigitChar(char c) {
    return c >= '0' && c <= '9';
}

static int isAlnumChar(char c) {
    return isDigitChar(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int peekToken(const pathLexer *l) {
    const char *p = l->p;
    switch (*p) {
    case '\0': return TOK_END;
    case '$': return TOK_DOLLAR;
    case '.': return p[1] == '.' ? TOK_DOTDOT : TOK_DOT;
    case '*': return TOK_WILDCARD;
    case ':': return TOK_COLON;
    case ',': return TOK_COMMA;
    case '?': return TOK_QUESTION_MARK;
    case '@': return TOK_AT;
    case '[': return TOK_LBRACKET;
    case ']': return TOK_RBRACKET;
    case '(': return TOK_LPAREN;
    case ')': return TOK_RPAREN;
    case '\'': return TOK_SINGLE_QUOTE;
    case '"': return TOK_DOUBLE_QUOTE;
    case '+': return TOK_PLUS;
    case '-': return TOK_MINUS;
    case '/': return TOK_DIV;
    case '%': return TOK_PCT;
    case ' ': return TOK_SPACE;
    case '&': return p[1] == '&' ? TOK_AND : TOK_SPECIAL_CHAR;
    case '|': return p[1] == '|' ? TOK_OR : TOK_SPECIAL_CHAR;
    case '=': return p[1] == '=' ? TOK_EQ : TOK_ASSIGN;
    case '!': return p[1] == '=' ? TOK_NE : TOK_NOT;
    case '>': return p[1] == '=' ? TOK_GE : TOK_GT;
    case '<': return p[1] == '=' ? TOK_LE : TOK_LT;
    default:
        if (isDigitChar(*p)) return TOK_DIGIT;
        if (isAlnumChar(*p)) return TOK_ALPHA;
        return TOK_SPECIAL_CHAR;
    }
}

static void nextToken(pathLexer *l, int skip_space) {
    for (;;) {
        l->next.type = peekToken(l);
        switch (l->next.type) {
        case TOK_END: return;
        case TOK_DOTDOT: (*l->rd_tokens)++; /* fall through */
        case TOK_NE:
        case TOK_GE:
        case TOK_LE:
        case TOK_EQ:
        case TOK_AND:
        case TOK_OR:
            l->next.str = l->p;
            l->p += 2;
            return;
        case TOK_SPACE:
            if (skip_space) {
                while (*l->p == ' ') l->p++;
                skip_space = 0;
                continue;
            }
            /* fall through */
        default:
            l->next.str = l->p;
            l->p++;
            return;
        }
    }
}

static int matchToken(pathLexer *l, int type, int skip_space) {
    if (skip_space && l->next.type == TOK_SPACE) {
        while (*l->p == ' ') l->p++;
        nextToken(l, 0);
        return matchToken(l, type, 0);
    }
    if (l->next.type == type) {
        nextToken(l, skip_space);
        return 1;
    }
    return 0;
}

/* Skip spaces including the current token. If the current token is not a
 * space, only the spaces after it are skipped and the token stays. */
static void skipSpaces(pathLexer *l) {
    if (l->next.type == TOK_SPACE) {
        nextToken(l, 1);
    } else {
        while (*l->p == ' ') l->p++;
    }
}

/* Digits accumulate with int64 wraparound, as the module's do. */
static uint64_t scanUnsignedInteger(pathLexer *l) {
    uint64_t val = (uint64_t)(*l->next.str - '0');
    while (*l->p != '\0' && isDigitChar(*l->p)) {
        val = val * 10 + (uint64_t)(*l->p - '0');
        l->p++;
    }
    return val;
}

/* Returns 0 if the current token cannot start an integer. */
static int scanInteger(pathLexer *l, int64_t *val) {
    *val = 0;
    int t = l->next.type;
    if (t != TOK_DIGIT && t != TOK_PLUS && t != TOK_MINUS) return 0;
    if (t == TOK_DIGIT) {
        *val = (int64_t)scanUnsignedInteger(l);
    } else {
        nextToken(l, 0);
        if (l->next.type != TOK_DIGIT) return 0;
        uint64_t u = scanUnsignedInteger(l);
        *val = (int64_t)(t == TOK_PLUS ? u : (uint64_t)0 - u);
    }
    nextToken(l, 0);
    return 1;
}

/* An unquoted member name runs from the current token to the next terminator.
 * strchr() also matches the terminating NUL, so a name cannot start at one. */
static jsonPathCode scanUnquotedMemberName(pathLexer *l, const char **name, size_t *len) {
    static const char *terminators = ".[]()<>=!'\" |&";
    const char *start = l->next.str;
    if (strchr(terminators, *start) != NULL) return JSON_PATH_ERR_INVALID_MEMBER_NAME;
    size_t n = strcspn(l->p, terminators);
    l->p += n;
    *name = start;
    *len = n + 1;
    nextToken(l, 0);
    return JSON_PATH_OK;
}

static jsonPathCode scanNumberInFilterExpr(pathLexer *l, const char **num, size_t *len) {
    static const char *chars = "+-0123456789.Ee";
    const char *start = l->next.str;
    if (strchr(chars, *start) == NULL) return JSON_PATH_ERR_INVALID_NUMBER;
    size_t n = strspn(l->p, chars);
    l->p += n;
    *num = start;
    *len = n + 1;
    nextToken(l, 0);
    return JSON_PATH_OK;
}

static jsonPathCode scanIdentifier(pathLexer *l, const char **id, size_t *len) {
    const char *start = l->next.str;
    if (!isAlnumChar(*start)) return JSON_PATH_ERR_INVALID_IDENTIFIER;
    size_t n = 1;
    while (*l->p != '\0' && isAlnumChar(*l->p)) {
        l->p++;
        n++;
    }
    *id = start;
    *len = n;
    nextToken(l, 0);
    return JSON_PATH_OK;
}

/* A '$' path inside a filter. Outside brackets it ends at a terminator; inside
 * brackets only signs, digits and quoted strings are allowed. */
static jsonPathCode scanPathValue(pathLexer *l, const char **path, size_t *len) {
    static const char *terminators = "]()<>=!'\" |&";
    static const char *numerics = "-+0123456789";
    const char *start = l->next.str, *p = l->p, *from = l->p;
    char quote = '"';
    int in_brackets = 0, in_quotes = 0;

    while (*p != '\0') {
        if (!in_brackets) {
            if (*p == '[') {
                in_brackets = 1;
            } else if (strchr(terminators, *p) != NULL) {
                break;
            }
            p++;
        } else if (!in_quotes) {
            if (*p == '"' || *p == '\'') {
                in_quotes = 1;
                quote = *p;
            } else if (*p == ']') {
                in_brackets = 0;
            } else if (strchr(numerics, *p) == NULL) {
                l->p = p;
                return JSON_PATH_ERR_INVALID_PATH;
            }
            p++;
        } else {
            if (*p == '\\' && p[1] == quote) {
                p++;
            } else if (*p == quote) {
                in_quotes = 0;
            }
            p++;
        }
    }
    l->p = p;
    *path = start;
    *len = (size_t)(p - from) + 1;
    nextToken(l, 0);
    return JSON_PATH_OK;
}

/* Parse text as a JSON document the way the module's JParser does. */
static jsonPathCode parseJsonText(const char *text, size_t len, jsonValue **v) {
    int err;
    size_t depth;
    *v = jsonParse(text, len, JSON_DEFAULT_MAX_DEPTH, &err, &depth);
    return *v ? JSON_PATH_OK : JSON_PATH_ERR_JSON_PARSE;
}

/* A double quoted string ends at the first quote not preceded by a backslash,
 * even if that backslash is itself escaped. The text is decoded by the JSON
 * parser. */
static jsonPathCode scanDoubleQuotedString(pathLexer *l, jsonValue **v) {
    const char *start = l->next.str, *from = l->p, *prev = NULL;
    while (*l->p != '\0') {
        if (*l->p == '"' && (prev == NULL || *prev != '\\')) {
            l->p++;
            break;
        }
        prev = l->p;
        l->p++;
    }
    jsonPathCode rc = parseJsonText(start, (size_t)(l->p - from) + 1, v);
    if (rc != JSON_PATH_OK) return rc;
    nextToken(l, 0);
    return JSON_PATH_OK;
}

/* Returns the quoted text without its quotes and leaves l->p after the closing
 * quote, or at the end of the path if there is none. */
static const char *scanSingleQuoted(pathLexer *l, size_t *len, int *escaped) {
    const char *start = l->p, *prev = NULL;
    size_t n = 0;
    *escaped = 0;
    while (*l->p != '\0') {
        if (*l->p == '\\') *escaped = 1;
        if (*l->p == '\'' && (prev == NULL || *prev != '\\')) {
            l->p++;
            break;
        }
        prev = l->p;
        l->p++;
        n++;
    }
    *len = n;
    return start;
}

/* Single quoted member names only decode \\ \t \b \f \n \r and \'. */
static sds unescapeSingleQuoted(const char *s, size_t len) {
    static const char *second = "\\tbfnr'";
    static const char *decoded = "\\\t\b\f\n\r\'";
    sds out = sdsempty();
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\\' && i != len - 1) {
            const char *c = strchr(second, s[i + 1]);
            if (c != NULL) {
                i++;
                out = sdscatlen(out, decoded + (c - second), 1);
                continue;
            }
        }
        out = sdscatlen(out, s + i, 1);
    }
    return out;
}

static sds scanSingleQuotedString(pathLexer *l) {
    size_t len;
    int escaped;
    const char *s = scanSingleQuoted(l, &len, &escaped);
    sds out = escaped ? unescapeSingleQuoted(s, len) : sdsnewlen(s, len);
    nextToken(l, 0);
    return out;
}

/* A single quoted comparison value is rewritten as a double quoted JSON string:
 * '"' is escaped, a backslash before a quote is dropped, and any other
 * backslash is kept for the JSON parser to decode. The character after the
 * text is the closing quote or the path's NUL, so s[i + 1] is always
 * readable. */
static sds scanSingleQuotedStringAsJson(pathLexer *l) {
    size_t len;
    int escaped;
    const char *s = scanSingleQuoted(l, &len, &escaped);
    sds out = sdsnewlen("\"", 1);
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"') {
            out = sdscatlen(out, "\\\"", 2);
        } else if (s[i] == '\\') {
            if (s[i + 1] != '\'') out = sdscatlen(out, "\\", 1);
        } else {
            out = sdscatlen(out, s + i, 1);
        }
    }
    out = sdscatlen(out, "\"", 1);
    nextToken(l, 0);
    return out;
}

/* ----------------------------------------------------------------------------
 * Result sets
 * ------------------------------------------------------------------------- */

void jsonPathInit(jsonPathResult *r) {
    memset(r, 0, sizeof(*r));
}

static void jsonPathClear(jsonPathResult *r) {
    for (size_t i = 0; i < r->len; i++) sdsfree(r->matches[i].pointer);
    for (size_t i = 0; i < r->ninserts; i++) sdsfree(r->inserts[i]);
    r->len = 0;
    r->ninserts = 0;
    r->max_depth = 0;
    r->error = JSON_PATH_OK;
}

void jsonPathFree(jsonPathResult *r) {
    jsonPathClear(r);
    zfree(r->matches);
    zfree(r->inserts);
    jsonPathInit(r);
}

static void addMatch(jsonPathResult *r, const jsonPathMatch *m) {
    if (r->len == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 4;
        r->matches = zrealloc(r->matches, sizeof(jsonPathMatch) * r->cap);
    }
    r->matches[r->len++] = *m;
}

static void addInsert(jsonPathResult *r, sds pointer) {
    if (r->ninserts == r->inserts_cap) {
        r->inserts_cap = r->inserts_cap ? r->inserts_cap * 2 : 4;
        r->inserts = zrealloc(r->inserts, sizeof(sds) * r->inserts_cap);
    }
    r->inserts[r->ninserts++] = pointer;
}

typedef struct matchRef {
    const jsonValue *value;
    size_t pos;
} matchRef;

static int matchRefCompare(const void *a, const void *b) {
    const matchRef *x = a, *y = b;
    if (x->value != y->value) return x->value < y->value ? -1 : 1;
    return x->pos < y->pos ? -1 : (x->pos > y->pos);
}

void jsonPathDedupe(jsonPathResult *r) {
    if (r->len <= 1) return;
    matchRef *refs = zmalloc(sizeof(matchRef) * r->len);
    for (size_t i = 0; i < r->len; i++) {
        refs[i].value = r->matches[i].value;
        refs[i].pos = i;
    }
    qsort(refs, r->len, sizeof(matchRef), matchRefCompare);
    unsigned char *dup = zcalloc(r->len);
    for (size_t i = 1; i < r->len; i++) {
        if (refs[i].value == refs[i - 1].value) dup[refs[i].pos] = 1;
    }
    size_t j = 0;
    for (size_t i = 0; i < r->len; i++) {
        if (dup[i]) {
            sdsfree(r->matches[i].pointer);
        } else {
            r->matches[j++] = r->matches[i];
        }
    }
    r->len = j;
    zfree(dup);
    zfree(refs);
}

static int sdsPtrCompare(const void *a, const void *b) {
    return sdscmp(*(const sds *)a, *(const sds *)b);
}

/* The module keeps insert paths in an unordered set. They are all distinct
 * members of distinct parents, so the order they are applied in is not
 * observable. */
static void uniqueInserts(jsonPathResult *r) {
    if (r->ninserts <= 1) return;
    qsort(r->inserts, r->ninserts, sizeof(sds), sdsPtrCompare);
    size_t j = 1;
    for (size_t i = 1; i < r->ninserts; i++) {
        if (sdscmp(r->inserts[i], r->inserts[j - 1]) == 0) {
            sdsfree(r->inserts[i]);
        } else {
            r->inserts[j++] = r->inserts[i];
        }
    }
    r->ninserts = j;
}

/* ----------------------------------------------------------------------------
 * Selector state
 * ------------------------------------------------------------------------- */

typedef struct selector {
    jsonPathResult *r;
    jsonValue *root;
    jsonValue *node; /* NULL ends the current branch. */
    jsonValue *parent;
    size_t index;
    size_t depth;
    sds node_path; /* JSON Pointer of node, NULL in read mode. */
    pathLexer lex;
    size_t curr_path_depth;
    jsonPathMode mode;
    int recursive_search; /* Set by '..': nothing below it is inserted. */
    int *recursion;       /* Shared with nested selectors, like the module's thread local. */
} selector;

typedef struct selState {
    jsonValue *node;
    jsonValue *parent;
    size_t index;
    size_t depth;
    size_t path_len;
    const char *p;
    pathToken tok;
    size_t curr_path_depth;
} selState;

static void snapshotState(selector *s, selState *st) {
    st->node = s->node;
    st->parent = s->parent;
    st->index = s->index;
    st->depth = s->depth;
    st->path_len = s->node_path ? sdslen(s->node_path) : 0;
    st->p = s->lex.p;
    st->tok = s->lex.next;
    st->curr_path_depth = s->curr_path_depth;
}

static void restoreState(selector *s, const selState *st) {
    s->node = st->node;
    s->parent = st->parent;
    s->index = st->index;
    s->depth = st->depth;
    if (s->node_path) sdssetlen(s->node_path, st->path_len);
    s->lex.p = st->p;
    s->lex.next = st->tok;
    s->curr_path_depth = st->curr_path_depth;
}

static void incrPathDepth(selector *s) {
    s->curr_path_depth++;
    if (s->curr_path_depth > s->r->max_depth) s->r->max_depth = s->curr_path_depth;
}

static void appendEscapedName(selector *s, const char *name, size_t len) {
    if (!s->node_path) return;
    s->node_path = sdscatlen(s->node_path, "/", 1);
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '~') {
            s->node_path = sdscatlen(s->node_path, "~0", 2);
        } else if (name[i] == '/') {
            s->node_path = sdscatlen(s->node_path, "~1", 2);
        } else {
            s->node_path = sdscatlen(s->node_path, name + i, 1);
        }
    }
}

static void appendIndex(selector *s, size_t idx) {
    if (s->node_path) s->node_path = sdscatfmt(s->node_path, "/%U", (unsigned long long)idx);
}

/* Step to child pos of the current node. */
static void enterChild(selector *s, jsonValue *child, size_t pos) {
    s->parent = s->node;
    s->index = pos;
    s->depth++;
    s->node = child;
}

static int enterRecursion(selector *s) {
    return ++*s->recursion > JSON_PATH_MAX_RECURSION;
}

static void leaveRecursion(selector *s) {
    --*s->recursion;
}

int jsonPathIsSyntaxError(jsonPathCode code) {
    switch (code) {
    case JSON_PATH_ERR_INVALID_PATH:
    case JSON_PATH_ERR_INVALID_MEMBER_NAME:
    case JSON_PATH_ERR_INVALID_NUMBER:
    case JSON_PATH_ERR_INVALID_IDENTIFIER:
    case JSON_PATH_ERR_EMPTY_EXPR_TOKEN:
    case JSON_PATH_ERR_INDEX_NOT_NUMBER:
    case JSON_PATH_ERR_STEP_ZERO:
    case JSON_PATH_ERR_RECURSION_LIMIT:
    case JSON_PATH_ERR_DESCENT_LIMIT:
    case JSON_PATH_ERR_QUERY_SIZE_LIMIT: return 1;
    default: return 0;
    }
}

/* ----------------------------------------------------------------------------
 * Filter index vectors
 * ------------------------------------------------------------------------- */

typedef struct idxVec {
    int64_t *v;
    size_t len;
    size_t cap;
} idxVec;

static void idxPush(idxVec *x, int64_t i) {
    if (x->len == x->cap) {
        x->cap = x->cap ? x->cap * 2 : 8;
        x->v = zrealloc(x->v, sizeof(int64_t) * x->cap);
    }
    x->v[x->len++] = i;
}

static void idxFree(idxVec *x) {
    zfree(x->v);
    x->v = NULL;
    x->len = x->cap = 0;
}

/* Filter results are indexes of the current node's children, or 0 for the node
 * itself, so membership fits a bitmap of the child count. */
static unsigned char *idxBitmap(const selector *s, const idxVec *x) {
    size_t n = jsonChildCount(s->node);
    unsigned char *bits = zcalloc(n ? n : 1);
    for (size_t k = 0; k < x->len; k++) bits[x->v[k]] = 1;
    return bits;
}

/* r gains the elements of v it lacks, in v's order. */
static void idxUnion(const selector *s, const idxVec *v, idxVec *r) {
    unsigned char *bits = idxBitmap(s, r);
    for (size_t k = 0; k < v->len; k++) {
        if (!bits[v->v[k]]) {
            bits[v->v[k]] = 1;
            idxPush(r, v->v[k]);
        }
    }
    zfree(bits);
}

/* r gets the elements of v1 that are in v2, in v1's order. */
static void idxIntersect(const selector *s, const idxVec *v1, const idxVec *v2, idxVec *r) {
    unsigned char *bits = idxBitmap(s, v2);
    for (size_t k = 0; k < v1->len; k++) {
        if (bits[v1->v[k]]) idxPush(r, v1->v[k]);
    }
    zfree(bits);
}

/* ----------------------------------------------------------------------------
 * Comparisons
 * ------------------------------------------------------------------------- */

/* RapidJSON value type, with true and false distinct. */
static int rjType(const jsonValue *v) {
    return v->type == JSON_INTEGER ? JSON_NUMBER : (int)v->type;
}

static int isBool(const jsonValue *v) {
    return v->type == JSON_TRUE || v->type == JSON_FALSE;
}

#define CMP_OP(op, a, b)           \
    ((op) == TOK_EQ   ? (a) == (b) \
     : (op) == TOK_NE ? (a) != (b) \
     : (op) == TOK_LT ? (a) < (b)  \
     : (op) == TOK_LE ? (a) <= (b) \
     : (op) == TOK_GT ? (a) > (b)  \
     : (op) == TOK_GE ? (a) >= (b) \
                      : 0)

static int evalOp(const jsonValue *v, int op, const jsonValue *c) {
    if (rjType(v) != rjType(c) && !(isBool(v) && isBool(c))) return 0;
    switch (v->type) {
    case JSON_NULL: return op == TOK_EQ;
    case JSON_TRUE:
    case JSON_FALSE: {
        int a = v->type == JSON_TRUE, b = c->type == JSON_TRUE;
        return CMP_OP(op, a, b);
    }
    case JSON_STRING: {
        size_t la = sdslen(v->string), lb = sdslen(c->string);
        int cmp = memcmp(v->string, c->string, la < lb ? la : lb);
        if (cmp == 0) cmp = la < lb ? -1 : (la > lb);
        return CMP_OP(op, cmp, 0);
    }
    case JSON_INTEGER:
    case JSON_NUMBER: {
        /* The module's reader keeps integers that fit int64_t as integers and
         * reads every other number, larger integers included, as a double. */
        if (v->type == JSON_INTEGER && c->type == JSON_INTEGER) return CMP_OP(op, v->integer, c->integer);
        double a = jsonGetDouble(v), b = jsonGetDouble(c);
        if (op == TOK_EQ) return a <= b && a >= b;
        if (op == TOK_NE) return a < b || a > b;
        return CMP_OP(op, a, b);
    }
    default: return 0;
    }
}

/* ----------------------------------------------------------------------------
 * Parser and evaluator
 * ------------------------------------------------------------------------- */

static jsonPathCode eval(selector *s);
static jsonPathCode parseRelativePath(selector *s);
static jsonPathCode parseQualifiedPath(selector *s);
static jsonPathCode parseBracketPathElement(selector *s);
static jsonPathCode parseFilterExpr(selector *s, idxVec *result);
static jsonPathCode runSelector(jsonPathResult *r, jsonValue *root, const char *path, jsonPathMode mode, int *recursion);

static jsonPathCode evalMember(selector *s) {
    jsonPathCode rc = JSON_PATH_ERR_RECURSION_LIMIT;
    if (!enterRecursion(s)) {
        incrPathDepth(s);
        rc = eval(s);
    }
    leaveRecursion(s);
    return rc;
}

static jsonPathCode evalObjectMember(selector *s, size_t pos) {
    if (s->node->type != JSON_OBJECT) return JSON_PATH_ERR_NOT_OBJECT;
    selState st;
    snapshotState(s, &st);
    jsonMember *m = &s->node->object.members[pos];
    appendEscapedName(s, m->name, sdslen(m->name));
    enterChild(s, m->value, pos);
    jsonPathCode rc = evalMember(s);
    restoreState(s, &st);
    return rc;
}

static jsonPathCode evalArrayMember(selector *s, int64_t idx) {
    if (s->node->type != JSON_ARRAY) return JSON_PATH_ERR_NOT_ARRAY;
    if (idx < 0 || idx >= (int64_t)s->node->array.len) return JSON_PATH_ERR_OUT_OF_BOUNDS;
    selState st;
    snapshotState(s, &st);
    appendIndex(s, (size_t)idx);
    enterChild(s, s->node->array.items[idx], (size_t)idx);
    jsonPathCode rc = evalMember(s);
    restoreState(s, &st);
    return rc;
}

static jsonPathCode traverseToObjectMember(selector *s, const char *name, size_t len) {
    if (s->node->type != JSON_OBJECT) {
        if (s->mode != JSON_PATH_READ) return JSON_PATH_ERR_INSERT_NON_OBJECT;
        s->node = NULL;
        return JSON_PATH_OK;
    }
    size_t pos;
    jsonValue *child = jsonObjectFind(s->node, name, len, &pos);
    if (child == NULL) {
        if (s->mode == JSON_PATH_WRITE && !s->recursive_search) {
            /* A member can only be created by the last step. This peeks one
             * token beyond the current one, as the module does. */
            if (peekToken(&s->lex) == TOK_END) {
                sds ins = sdsdup(s->node_path);
                ins = sdscatlen(ins, "/", 1);
                ins = sdscatlen(ins, name, len);
                addInsert(s->r, ins);
                incrPathDepth(s);
            } else {
                s->r->error = JSON_PATH_ERR_NOT_EXIST;
                return JSON_PATH_ERR_NOT_EXIST;
            }
        }
        s->node = NULL;
        return JSON_PATH_OK;
    }
    appendEscapedName(s, name, len);
    enterChild(s, child, pos);
    incrPathDepth(s);
    return JSON_PATH_OK;
}

static jsonPathCode traverseToArrayIndex(selector *s, int64_t idx) {
    if (s->node->type != JSON_ARRAY) {
        s->node = NULL;
        return JSON_PATH_OK;
    }
    int64_t size = (int64_t)s->node->array.len;
    if (idx < 0) idx += size;
    if (idx >= size || idx < 0) return JSON_PATH_ERR_OUT_OF_BOUNDS;
    appendIndex(s, (size_t)idx);
    enterChild(s, s->node->array.items[idx], (size_t)idx);
    incrPathDepth(s);
    return JSON_PATH_OK;
}

/* DFS preorder: run the rest of the path at v, then at each descendant. */
static jsonPathCode recursiveSearch(selector *s, jsonValue *v) {
    int t = s->lex.next.type;
    if (t == TOK_DOTDOT || t == TOK_DOT) return JSON_PATH_ERR_INVALID_DOT_SEQUENCE;
    if (v->type != JSON_OBJECT && v->type != JSON_ARRAY) {
        s->node = NULL;
        return JSON_PATH_OK;
    }

    selState st;
    snapshotState(s, &st);
    s->node = v;
    jsonPathCode rc = eval(s);
    restoreState(s, &st);
    if (jsonPathIsSyntaxError(rc)) return rc;

    size_t n = jsonChildCount(v);
    for (size_t i = 0; i < n; i++) {
        size_t path_len = s->node_path ? sdslen(s->node_path) : 0;
        jsonValue *parent = s->parent;
        size_t index = s->index, depth = s->depth;
        if (v->type == JSON_OBJECT) {
            jsonMember *m = &v->object.members[i];
            appendEscapedName(s, m->name, sdslen(m->name));
        } else {
            appendIndex(s, i);
        }
        s->parent = v;
        s->index = i;
        s->depth++;
        incrPathDepth(s);
        rc = recursiveSearch(s, jsonChildAt(v, i));
        s->curr_path_depth--;
        if (jsonPathIsSyntaxError(rc)) return rc;
        if (s->node_path) sdssetlen(s->node_path, path_len);
        s->parent = parent;
        s->index = index;
        s->depth = depth;
    }
    s->node = NULL;
    return JSON_PATH_OK;
}

/* RecursivePath ::= ".." SupportedPath */
static jsonPathCode parseRecursivePath(selector *s) {
    s->recursive_search = 1;
    matchToken(&s->lex, TOK_DOTDOT, 0);
    if (*s->lex.rd_tokens > JSON_PATH_MAX_DESCENT_TOKENS) return JSON_PATH_ERR_DESCENT_LIMIT;
    jsonPathCode rc = recursiveSearch(s, s->node);
    if (rc != JSON_PATH_OK) return rc;
    jsonPathDedupe(s->r);
    return JSON_PATH_OK;
}

static jsonPathCode processWildcardKey(selector *s) {
    size_t n = s->node->object.len;
    for (size_t i = 0; i < n; i++) {
        selState st;
        snapshotState(s, &st);
        jsonPathCode rc = evalObjectMember(s, i);
        restoreState(s, &st);
        if (jsonPathIsSyntaxError(rc)) return rc;
    }
    s->node = NULL;
    return JSON_PATH_OK;
}

static jsonPathCode processWildcardIndex(selector *s) {
    for (size_t i = 0; i < s->node->array.len; i++) {
        jsonPathCode rc = evalArrayMember(s, (int64_t)i);
        if (jsonPathIsSyntaxError(rc)) return rc;
    }
    s->node = NULL;
    return JSON_PATH_OK;
}

/* A wildcard on a scalar fails a legacy path outright; for JSONPath the error
 * only ends this branch. */
static jsonPathCode processWildcard(selector *s) {
    if (s->node->type == JSON_OBJECT) return processWildcardKey(s);
    if (s->node->type == JSON_ARRAY) return processWildcardIndex(s);
    return s->r->v2 ? JSON_PATH_ERR_INVALID_WILDCARD : JSON_PATH_ERR_INVALID_PATH;
}

/* Continue at the elements a filter selected. On an object a non-empty result
 * selects the object itself, and on a scalar it selects the scalar. */
static jsonPathCode processFilterResult(selector *s, idxVec *result) {
    jsonPathCode rc;
    if (s->node->type == JSON_ARRAY) {
        for (size_t k = 0; k < result->len; k++) {
            rc = evalArrayMember(s, result->v[k]);
            if (jsonPathIsSyntaxError(rc)) return rc;
        }
        s->node = NULL;
        return JSON_PATH_OK;
    }
    if (s->node->type == JSON_OBJECT) {
        if (result->len == 0) s->node = NULL;
        return JSON_PATH_OK;
    }
    if (result->len) {
        rc = evalMember(s);
        if (jsonPathIsSyntaxError(rc)) return rc;
    }
    s->node = NULL;
    return JSON_PATH_OK;
}

static jsonPathCode processComparisonExpr(selector *s, int is_self, const char *name, size_t len, int op, const jsonValue *cv, idxVec *result) {
    jsonValue *node = s->node, *v;
    if (node->type == JSON_ARRAY) {
        for (size_t i = 0; i < node->array.len; i++) {
            jsonValue *m = node->array.items[i];
            if (is_self) {
                v = m;
            } else {
                if (m->type != JSON_OBJECT) continue;
                if ((v = jsonObjectFind(m, name, len, NULL)) == NULL) continue;
            }
            if (evalOp(v, op, cv)) idxPush(result, (int64_t)i);
        }
    } else if (node->type == JSON_OBJECT) {
        /* The object's own member, even for a bare '@'. */
        if ((v = jsonObjectFind(node, name, len, NULL)) != NULL && evalOp(v, op, cv)) idxPush(result, 0);
    } else if (is_self) {
        if (evalOp(node, op, cv)) idxPush(result, 0);
    }
    return JSON_PATH_OK;
}

static jsonPathCode processComparisonExprAtIndex(selector *s, int64_t idx, const char *name, size_t len, int op, const jsonValue *cv, idxVec *result) {
    jsonValue *node = s->node;
    if (node->type != JSON_ARRAY) return JSON_PATH_OK;
    for (size_t i = 0; i < node->array.len; i++) {
        jsonValue *m = node->array.items[i], *v;
        if (m->type != JSON_OBJECT) continue;
        if ((v = jsonObjectFind(m, name, len, NULL)) == NULL || v->type != JSON_ARRAY) continue;
        int64_t inner = idx;
        if (inner < 0) inner += (int64_t)v->array.len;
        if (inner < (int64_t)v->array.len && inner >= 0 && evalOp(v->array.items[inner], op, cv)) {
            idxPush(result, (int64_t)i);
        }
    }
    return JSON_PATH_OK;
}

/* Elements whose member name is an array with an element satisfying op. */
static jsonPathCode processArrayContains(selector *s, const char *name, size_t len, int op, const jsonValue *cv, idxVec *result) {
    jsonValue *node = s->node;
    if (node->type == JSON_ARRAY) {
        for (size_t i = 0; i < node->array.len; i++) {
            jsonValue *m = node->array.items[i], *v;
            if (m->type != JSON_OBJECT) continue;
            if ((v = jsonObjectFind(m, name, len, NULL)) == NULL || v->type != JSON_ARRAY) continue;
            for (size_t j = 0; j < v->array.len; j++) {
                if (evalOp(v->array.items[j], op, cv)) {
                    idxPush(result, (int64_t)i);
                    break;
                }
            }
        }
    }
    if (!matchToken(&s->lex, TOK_RPAREN, 1)) return JSON_PATH_ERR_INVALID_PATH;
    if (!matchToken(&s->lex, TOK_RBRACKET, 0)) return JSON_PATH_ERR_INVALID_PATH;
    return JSON_PATH_OK;
}

static jsonPathCode processAttributeFilter(selector *s, const char *name, size_t len, idxVec *result) {
    jsonValue *node = s->node;
    if (node->type == JSON_ARRAY) {
        for (size_t i = 0; i < node->array.len; i++) {
            jsonValue *m = node->array.items[i];
            if (m->type == JSON_OBJECT && jsonObjectFind(m, name, len, NULL) != NULL) idxPush(result, (int64_t)i);
        }
    } else if (node->type == JSON_OBJECT) {
        if (jsonObjectFind(node, name, len, NULL) != NULL) idxPush(result, 0);
    } else {
        return JSON_PATH_ERR_INVALID_PATH;
    }
    return JSON_PATH_OK;
}

static jsonPathCode parseIndex(selector *s, int64_t *val) {
    return scanInteger(&s->lex, val) ? JSON_PATH_OK : JSON_PATH_ERR_INDEX_NOT_NUMBER;
}

/* QuotedMemberName ::= "\"" {char} "\"" | "'" {char} "'" */
static jsonPathCode parseQuotedMemberName(selector *s, sds *name) {
    int t = s->lex.next.type;
    if (t == TOK_DOUBLE_QUOTE) {
        jsonValue *v;
        jsonPathCode rc = scanDoubleQuotedString(&s->lex, &v);
        if (rc != JSON_PATH_OK) return rc;
        *name = sdsdup(v->string);
        jsonFree(v);
        return JSON_PATH_OK;
    }
    if (t == TOK_SINGLE_QUOTE) {
        *name = scanSingleQuotedString(&s->lex);
        return JSON_PATH_OK;
    }
    return JSON_PATH_ERR_INVALID_PATH;
}

/* BracketedMemberName ::= "[" {SPACE} QuotedMemberName {SPACE} "]" */
static jsonPathCode parseBracketedMemberName(selector *s, sds *name) {
    skipSpaces(&s->lex);
    jsonPathCode rc = parseQuotedMemberName(s, name);
    if (rc != JSON_PATH_OK) return rc;
    if (!matchToken(&s->lex, TOK_RBRACKET, 1)) {
        sdsfree(*name);
        *name = NULL;
        return JSON_PATH_ERR_INVALID_PATH;
    }
    return JSON_PATH_OK;
}

/* MemberName ::= ("." (UnquotedMemberName | BracketedMemberName)) | BracketedMemberName */
static jsonPathCode parseMemberName(selector *s, sds *name) {
    const char *n;
    size_t len;
    if (matchToken(&s->lex, TOK_DOT, 0)) {
        if (matchToken(&s->lex, TOK_LBRACKET, 0)) return parseBracketedMemberName(s, name);
        jsonPathCode rc = scanUnquotedMemberName(&s->lex, &n, &len);
        if (rc == JSON_PATH_OK) *name = sdsnewlen(n, len);
        return rc;
    }
    if (matchToken(&s->lex, TOK_LBRACKET, 0)) return parseBracketedMemberName(s, name);
    return JSON_PATH_ERR_INVALID_PATH;
}

/* ComparisonOp ::= {SPACE} "<" | "<=" | ">" | ">=" | "==" | "!=" {SPACE} */
static jsonPathCode parseComparisonOp(selector *s, int *op) {
    skipSpaces(&s->lex);
    int t = s->lex.next.type;
    if (t != TOK_EQ && t != TOK_NE && t != TOK_LT && t != TOK_LE && t != TOK_GT && t != TOK_GE) {
        return JSON_PATH_ERR_INVALID_PATH;
    }
    *op = t;
    skipSpaces(&s->lex);
    nextToken(&s->lex, 1);
    return JSON_PATH_OK;
}

static int swapComparisonOpSide(int op) {
    switch (op) {
    case TOK_GT: return TOK_LT;
    case TOK_LT: return TOK_GT;
    case TOK_GE: return TOK_LE;
    case TOK_LE: return TOK_GE;
    default: return op;
    }
}

/* ComparisonValue ::= "null" | Bool | Number | QuotedString | PartialPath
 * *owned tells the caller whether to free *v. A PartialPath must select exactly
 * one scalar. */
static jsonPathCode parseComparisonValueInner(selector *s, jsonValue **v, int *owned) {
    pathLexer *l = &s->lex;
    const char *text;
    size_t len;
    jsonPathCode rc;
    *owned = 1;
    if (l->next.type == TOK_DOLLAR) {
        rc = scanPathValue(l, &text, &len);
        if (rc != JSON_PATH_OK) return rc;
        sds path = sdsnewlen(text, len);
        jsonPathResult sub;
        jsonPathInit(&sub);
        rc = runSelector(&sub, s->root, path, JSON_PATH_READ, s->recursion);
        sdsfree(path);
        if (rc == JSON_PATH_OK) {
            if (sub.len != 1 || sub.matches[0].value->type == JSON_OBJECT ||
                sub.matches[0].value->type == JSON_ARRAY) {
                rc = JSON_PATH_ERR_INVALID_PATH;
            } else {
                *v = sub.matches[0].value;
                *owned = 0;
            }
        }
        jsonPathFree(&sub);
        return rc;
    }
    if (l->next.type == TOK_DOUBLE_QUOTE) return scanDoubleQuotedString(l, v);
    if (l->next.type == TOK_SINGLE_QUOTE) {
        sds json = scanSingleQuotedStringAsJson(l);
        rc = parseJsonText(json, sdslen(json), v);
        sdsfree(json);
        return rc;
    }
    if (l->next.type == TOK_ALPHA && *l->next.str == 'n') {
        if ((rc = scanIdentifier(l, &text, &len)) != JSON_PATH_OK) return rc;
        if (len != 4 || memcmp(text, "null", 4) != 0) return JSON_PATH_ERR_INVALID_IDENTIFIER;
    } else if (l->next.type == TOK_ALPHA && (*l->next.str == 't' || *l->next.str == 'f')) {
        if ((rc = scanIdentifier(l, &text, &len)) != JSON_PATH_OK) return rc;
        if (!(len == 4 && memcmp(text, "true", 4) == 0) && !(len == 5 && memcmp(text, "false", 5) == 0)) {
            return JSON_PATH_ERR_INVALID_IDENTIFIER;
        }
    } else {
        if ((rc = scanNumberInFilterExpr(l, &text, &len)) != JSON_PATH_OK) return rc;
    }
    return parseJsonText(text, len, v);
}

static jsonPathCode parseComparisonValue(selector *s, jsonValue **v, int *owned) {
    jsonPathCode rc = JSON_PATH_ERR_RECURSION_LIMIT;
    *v = NULL;
    *owned = 0;
    if (!enterRecursion(s)) rc = parseComparisonValueInner(s, v, owned);
    leaveRecursion(s);
    return rc;
}

static void freeComparisonValue(jsonValue *v, int owned) {
    if (owned) jsonFree(v);
}

static jsonPathCode parseFactorInner(selector *s, idxVec *result) {
    pathLexer *l = &s->lex;
    jsonPathCode rc;
    jsonValue *cv = NULL;
    int owned = 0, op = TOK_UNKNOWN;
    sds name = NULL;

    skipSpaces(l);
    if (l->next.type == TOK_LPAREN) {
        nextToken(l, 1);
        if ((rc = parseFilterExpr(s, result)) != JSON_PATH_OK) return rc;
        if (!matchToken(l, TOK_RPAREN, 1)) return JSON_PATH_ERR_INVALID_PATH;
        return JSON_PATH_OK;
    }

    if (matchToken(l, TOK_AT, 0)) {
        if (l->next.type != TOK_DOT && l->next.type != TOK_LBRACKET) {
            /* @ op value */
            if ((rc = parseComparisonOp(s, &op)) != JSON_PATH_OK) return rc;
            if ((rc = parseComparisonValue(s, &cv, &owned)) != JSON_PATH_OK) goto done;
            rc = processComparisonExpr(s, 1, "", 0, op, cv, result);
            goto done;
        }
        if ((rc = parseMemberName(s, &name)) != JSON_PATH_OK) goto done;
        skipSpaces(l);
        int t = l->next.type;
        if (t == TOK_LT || t == TOK_LE || t == TOK_GT || t == TOK_GE || t == TOK_EQ || t == TOK_NE) {
            /* @.name op value */
            if ((rc = parseComparisonOp(s, &op)) != JSON_PATH_OK) goto done;
            if ((rc = parseComparisonValue(s, &cv, &owned)) != JSON_PATH_OK) goto done;
            rc = processComparisonExpr(s, 0, name, sdslen(name), op, cv, result);
        } else if (t == TOK_LBRACKET) {
            nextToken(l, 1);
            if (l->next.type == TOK_QUESTION_MARK) {
                /* @.name[?(@ op value)] or @.name[?(value op @)] */
                nextToken(l, 1);
                if (!matchToken(l, TOK_LPAREN, 0)) {
                    rc = JSON_PATH_ERR_INVALID_PATH;
                    goto done;
                }
                if (l->next.type == TOK_AT) {
                    nextToken(l, 1);
                    if ((rc = parseComparisonOp(s, &op)) != JSON_PATH_OK) goto done;
                    if ((rc = parseComparisonValue(s, &cv, &owned)) != JSON_PATH_OK) goto done;
                } else {
                    if ((rc = parseComparisonValue(s, &cv, &owned)) != JSON_PATH_OK) goto done;
                    if ((rc = parseComparisonOp(s, &op)) != JSON_PATH_OK) goto done;
                    op = swapComparisonOpSide(op);
                    if (!matchToken(l, TOK_AT, 0)) {
                        rc = JSON_PATH_ERR_INVALID_PATH;
                        goto done;
                    }
                }
                rc = processArrayContains(s, name, sdslen(name), op, cv, result);
            } else {
                /* @.name[index] op value */
                int64_t index;
                if ((rc = parseIndex(s, &index)) != JSON_PATH_OK) goto done;
                if (!matchToken(l, TOK_RBRACKET, 0)) {
                    rc = JSON_PATH_ERR_INVALID_PATH;
                    goto done;
                }
                if ((rc = parseComparisonOp(s, &op)) != JSON_PATH_OK) goto done;
                if ((rc = parseComparisonValue(s, &cv, &owned)) != JSON_PATH_OK) goto done;
                rc = processComparisonExprAtIndex(s, index, name, sdslen(name), op, cv, result);
            }
        } else {
            /* @.name */
            rc = processAttributeFilter(s, name, sdslen(name), result);
        }
        goto done;
    }

    /* value op @ [MemberName [index]] */
    if ((rc = parseComparisonValue(s, &cv, &owned)) != JSON_PATH_OK) goto done;
    skipSpaces(l);
    if ((rc = parseComparisonOp(s, &op)) != JSON_PATH_OK) goto done;
    op = swapComparisonOpSide(op);
    if (!matchToken(l, TOK_AT, 0)) {
        rc = JSON_PATH_ERR_INVALID_PATH;
        goto done;
    }
    skipSpaces(l);
    if (l->next.type == TOK_RPAREN || l->next.type == TOK_AND || l->next.type == TOK_OR) {
        rc = processComparisonExpr(s, 1, "", 0, op, cv, result);
        goto done;
    }
    if ((rc = parseMemberName(s, &name)) != JSON_PATH_OK) goto done;
    if (l->next.type == TOK_LBRACKET) {
        nextToken(l, 1);
        int64_t index;
        if ((rc = parseIndex(s, &index)) != JSON_PATH_OK) goto done;
        if (!matchToken(l, TOK_RBRACKET, 0)) {
            rc = JSON_PATH_ERR_INVALID_PATH;
            goto done;
        }
        rc = processComparisonExprAtIndex(s, index, name, sdslen(name), op, cv, result);
    } else {
        rc = processComparisonExpr(s, 0, name, sdslen(name), op, cv, result);
    }

done:
    sdsfree(name);
    freeComparisonValue(cv, owned);
    return rc;
}

static jsonPathCode parseFactor(selector *s, idxVec *result) {
    jsonPathCode rc = JSON_PATH_ERR_RECURSION_LIMIT;
    if (!enterRecursion(s)) rc = parseFactorInner(s, result);
    leaveRecursion(s);
    return rc;
}

/* Term ::= Factor { {SPACE} "&&" {SPACE} Factor } */
static jsonPathCode parseTermInner(selector *s, idxVec *result) {
    jsonPathCode rc = parseFactor(s, result);
    if (rc != JSON_PATH_OK) return rc;
    while (matchToken(&s->lex, TOK_AND, 1)) {
        idxVec v1 = *result, v2 = {0};
        memset(result, 0, sizeof(*result));
        rc = parseFactor(s, &v2);
        if (rc == JSON_PATH_OK) idxIntersect(s, &v1, &v2, result);
        idxFree(&v1);
        idxFree(&v2);
        if (rc != JSON_PATH_OK) return rc;
    }
    return JSON_PATH_OK;
}

static jsonPathCode parseTerm(selector *s, idxVec *result) {
    jsonPathCode rc = JSON_PATH_ERR_RECURSION_LIMIT;
    if (!enterRecursion(s)) rc = parseTermInner(s, result);
    leaveRecursion(s);
    return rc;
}

/* FilterExpr ::= {SPACE} Term { {SPACE} "||" {SPACE} Term {SPACE} } */
static jsonPathCode parseFilterExprInner(selector *s, idxVec *result) {
    skipSpaces(&s->lex);
    jsonPathCode rc = parseTerm(s, result);
    if (rc != JSON_PATH_OK) return rc;
    while (matchToken(&s->lex, TOK_OR, 1)) {
        idxVec v = {0};
        rc = parseTerm(s, &v);
        if (rc == JSON_PATH_OK) idxUnion(s, &v, result);
        idxFree(&v);
        if (rc != JSON_PATH_OK) return rc;
    }
    skipSpaces(&s->lex);
    return JSON_PATH_OK;
}

static jsonPathCode parseFilterExpr(selector *s, idxVec *result) {
    jsonPathCode rc = JSON_PATH_ERR_RECURSION_LIMIT;
    if (!enterRecursion(s)) rc = parseFilterExprInner(s, result);
    leaveRecursion(s);
    return rc;
}

/* Filter ::= "?" "(" FilterExpr ")" */
static jsonPathCode parseFilter(selector *s) {
    nextToken(&s->lex, 0);
    if (!matchToken(&s->lex, TOK_LPAREN, 0)) return JSON_PATH_ERR_INVALID_PATH;
    idxVec result = {0};
    jsonPathCode rc = parseFilterExpr(s, &result);
    if (rc == JSON_PATH_OK && !matchToken(&s->lex, TOK_RPAREN, 0)) rc = JSON_PATH_ERR_INVALID_PATH;
    if (rc == JSON_PATH_OK && !matchToken(&s->lex, TOK_RBRACKET, 1)) rc = JSON_PATH_ERR_INVALID_PATH;
    if (rc == JSON_PATH_OK) rc = processFilterResult(s, &result);
    idxFree(&result);
    return rc;
}

/* WildcardFilter ::= "[" "?" "(" FilterExpr ")" "]", after "*" or "*." */
static jsonPathCode parseWildcardFilter(selector *s) {
    pathLexer *l = &s->lex;
    if (s->node->type != JSON_ARRAY) return JSON_PATH_ERR_INVALID_PATH;
    if (!matchToken(l, TOK_LBRACKET, 0)) return JSON_PATH_ERR_INVALID_PATH;
    if (!matchToken(l, TOK_QUESTION_MARK, 0)) return JSON_PATH_ERR_INVALID_PATH;
    if (!matchToken(l, TOK_LPAREN, 1)) return JSON_PATH_ERR_INVALID_PATH;
    idxVec result = {0};
    jsonPathCode rc = parseFilterExpr(s, &result);
    if (rc == JSON_PATH_OK && !matchToken(l, TOK_RPAREN, 1)) rc = JSON_PATH_ERR_INVALID_PATH;
    if (rc == JSON_PATH_OK && !matchToken(l, TOK_RBRACKET, 0)) rc = JSON_PATH_ERR_INVALID_PATH;
    if (rc == JSON_PATH_OK) rc = processFilterResult(s, &result);
    idxFree(&result);
    return rc;
}

/* WildcardInBrackets ::= "*" {SPACE} "]" [ "[" {SPACE} "?" "(" FilterExpr ")" {SPACE} "]" ] */
static jsonPathCode parseWildcardInBrackets(selector *s) {
    pathLexer *l = &s->lex;
    if (!matchToken(l, TOK_WILDCARD, 1)) return JSON_PATH_ERR_INVALID_PATH;
    if (!matchToken(l, TOK_RBRACKET, 1)) return JSON_PATH_ERR_INVALID_PATH;
    if (!(l->next.type == TOK_LBRACKET && peekToken(l) == TOK_QUESTION_MARK)) return processWildcard(s);

    if (!matchToken(l, TOK_LBRACKET, 1)) return JSON_PATH_ERR_INVALID_PATH;
    skipSpaces(l);
    if (!matchToken(l, TOK_QUESTION_MARK, 0)) return JSON_PATH_ERR_INVALID_PATH;
    if (!matchToken(l, TOK_LPAREN, 0)) return JSON_PATH_ERR_INVALID_PATH;
    idxVec result = {0};
    jsonPathCode rc = parseFilterExpr(s, &result);
    if (rc == JSON_PATH_OK && !matchToken(l, TOK_RPAREN, 1)) rc = JSON_PATH_ERR_INVALID_PATH;
    if (rc == JSON_PATH_OK && !matchToken(l, TOK_RBRACKET, 1)) rc = JSON_PATH_ERR_INVALID_PATH;
    if (rc == JSON_PATH_OK) rc = processFilterResult(s, &result);
    idxFree(&result);
    return rc;
}

static jsonPathCode processUnionOfMembers(selector *s, sds *names, size_t n) {
    if (n == 1) return traverseToObjectMember(s, names[0], sdslen(names[0]));
    if (s->node->type != JSON_OBJECT) {
        if (s->mode != JSON_PATH_READ) return JSON_PATH_ERR_INSERT_NON_OBJECT;
        s->node = NULL;
        return JSON_PATH_OK;
    }
    for (size_t k = 0; k < n; k++) {
        size_t pos;
        if (jsonObjectFind(s->node, names[k], sdslen(names[k]), &pos) != NULL) {
            jsonPathCode rc = evalObjectMember(s, pos);
            if (jsonPathIsSyntaxError(rc)) return rc;
        }
    }
    s->node = NULL;
    return JSON_PATH_OK;
}

/* NameInBrackets ::= QuotedMemberName { {SPACE} "," {SPACE} QuotedMemberName } */
static jsonPathCode parseNameInBrackets(selector *s) {
    sds *names = NULL;
    size_t n = 0, cap = 0;
    jsonPathCode rc;
    do {
        if (n) skipSpaces(&s->lex);
        sds name;
        if ((rc = parseQuotedMemberName(s, &name)) != JSON_PATH_OK) goto done;
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            names = zrealloc(names, sizeof(sds) * cap);
        }
        names[n++] = name;
    } while (matchToken(&s->lex, TOK_COMMA, 1));

    if (!matchToken(&s->lex, TOK_RBRACKET, 1)) {
        rc = JSON_PATH_ERR_INVALID_PATH;
        goto done;
    }
    rc = processUnionOfMembers(s, names, n);

done:
    for (size_t k = 0; k < n; k++) sdsfree(names[k]);
    zfree(names);
    return rc;
}

/* Out-of-range indexes are skipped, but the bound is computed in 32-bit
 * unsigned arithmetic, so on an empty array index 0 is tried and fails. Any
 * error from a branch ends the union. */
static jsonPathCode processUnion(selector *s, idxVec *indices) {
    uint32_t last = (uint32_t)s->node->array.len - 1;
    for (size_t k = 0; k < indices->len; k++) {
        int64_t i = indices->v[k];
        if (i < 0) i += (int64_t)s->node->array.len;
        if (i < 0 || i > (int64_t)last) continue;
        jsonPathCode rc = evalArrayMember(s, i);
        if (rc != JSON_PATH_OK) return rc;
    }
    s->node = NULL;
    return JSON_PATH_OK;
}

/* UnionOfIndexes ::= Integer ({SPACE} "," {SPACE} Integer)+ */
static jsonPathCode parseUnionOfIndexes(selector *s, int64_t start) {
    pathLexer *l = &s->lex;
    if (s->node->type != JSON_ARRAY) return JSON_PATH_ERR_NOT_ARRAY;
    idxVec indices = {0};
    jsonPathCode rc = JSON_PATH_OK;
    int comma = 0;
    idxPush(&indices, start);

    skipSpaces(l);
    while (l->next.type != TOK_RBRACKET) {
        if (l->next.type == TOK_COMMA) {
            if (comma) {
                rc = JSON_PATH_ERR_INVALID_PATH;
                goto done;
            }
            comma = 1;
            nextToken(l, 1);
        } else {
            if (!comma) {
                rc = JSON_PATH_ERR_INVALID_PATH;
                goto done;
            }
            comma = 0;
            skipSpaces(l);
            int64_t index;
            if ((rc = parseIndex(s, &index)) != JSON_PATH_OK) goto done;
            idxPush(&indices, index);
        }
        skipSpaces(l);
    }
    if (comma || !matchToken(l, TOK_RBRACKET, 1)) {
        rc = JSON_PATH_ERR_INVALID_PATH;
        goto done;
    }
    rc = processUnion(s, &indices);

done:
    idxFree(&indices);
    return rc;
}

/* start and end are clamped to the array, then walked with a 32-bit int loop
 * counter, so a step beyond int range wraps as it does in the module. */
static jsonPathCode processSlice(selector *s, int64_t start, int64_t end, int64_t step) {
    if (s->node->type != JSON_ARRAY) return JSON_PATH_ERR_NOT_ARRAY;
    if (!matchToken(&s->lex, TOK_RBRACKET, 1)) return JSON_PATH_ERR_INVALID_PATH;
    int64_t size = (int64_t)s->node->array.len;
    if (start < 0) start += size;
    if (end < 0) end += size;
    if (step == 0) return JSON_PATH_ERR_STEP_ZERO;

    if (start < 0) {
        start = 0;
    } else if (start > size) {
        start = size;
    }
    if (end < 0) {
        end = 0;
    } else if (end > size) {
        end = size;
    }

    jsonPathCode rc;
    int i;
    if (step > 0) {
        for (i = (int)start; i < end; i = (int)(uint32_t)((uint64_t)(int64_t)i + (uint64_t)step)) {
            rc = evalArrayMember(s, i);
            if (jsonPathIsSyntaxError(rc)) return rc;
        }
    } else {
        for (i = (int)start; i > end; i = (int)(uint32_t)((uint64_t)(int64_t)i + (uint64_t)step)) {
            rc = evalArrayMember(s, i);
            if (jsonPathIsSyntaxError(rc)) return rc;
        }
    }
    s->node = NULL;
    return JSON_PATH_OK;
}

static jsonPathCode parseStep(selector *s, int64_t start, int64_t end) {
    skipSpaces(&s->lex);
    if (s->lex.next.type == TOK_RBRACKET) return processSlice(s, start, end, 1);
    int64_t step;
    jsonPathCode rc = parseIndex(s, &step);
    if (rc != JSON_PATH_OK) return rc;
    return processSlice(s, start, end, step);
}

/* EndAndStep ::= End [{SPACE} ":" {SPACE} [Step]] ] */
static jsonPathCode parseEndAndStep(selector *s, int64_t start) {
    int64_t end;
    jsonPathCode rc = parseIndex(s, &end);
    if (rc != JSON_PATH_OK) return rc;
    skipSpaces(&s->lex);
    if (s->lex.next.type == TOK_COLON) {
        nextToken(&s->lex, 0);
        return parseStep(s, start, end);
    }
    return processSlice(s, start, end, 1);
}

/* SliceStartsWithColon ::= {SPACE} ":" {SPACE} [ ":" {SPACE} [Step] | EndAndStep ] ] */
static jsonPathCode parseSliceStartsWithColon(selector *s) {
    if (s->node->type != JSON_ARRAY) return JSON_PATH_ERR_NOT_ARRAY;
    int64_t size = (int64_t)s->node->array.len;
    nextToken(&s->lex, 1);
    switch (s->lex.next.type) {
    case TOK_RBRACKET: return processSlice(s, 0, size, 1);
    case TOK_COLON: nextToken(&s->lex, 1); return parseStep(s, 0, size);
    default: return parseEndAndStep(s, 0);
    }
}

/* SliceStartsWithInteger ::= Start {SPACE} ":" {SPACE} [ ":" {SPACE} [Step] | EndAndStep */
static jsonPathCode parseSliceStartsWithInteger(selector *s, int64_t start) {
    if (s->node->type != JSON_ARRAY) return JSON_PATH_ERR_NOT_ARRAY;
    int64_t size = (int64_t)s->node->array.len;
    nextToken(&s->lex, 1);
    skipSpaces(&s->lex);
    switch (s->lex.next.type) {
    case TOK_RBRACKET: return processSlice(s, start, size, 1);
    case TOK_COLON: nextToken(&s->lex, 0); return parseStep(s, start, size);
    default: return parseEndAndStep(s, start);
    }
}

static jsonPathCode processSubscript(selector *s, int64_t idx) {
    if (!matchToken(&s->lex, TOK_RBRACKET, 1)) return JSON_PATH_ERR_INVALID_PATH;
    if (s->node->type != JSON_ARRAY) return JSON_PATH_ERR_NOT_ARRAY;
    return traverseToArrayIndex(s, idx);
}

/* SliceOrUnionOrIndex ::= SliceStartsWithInteger | UnionOfIndexes | Index */
static jsonPathCode parseSliceOrUnionOrIndex(selector *s) {
    int64_t start;
    jsonPathCode rc = parseIndex(s, &start);
    if (rc != JSON_PATH_OK) return rc;
    skipSpaces(&s->lex);
    switch (s->lex.next.type) {
    case TOK_COLON: return parseSliceStartsWithInteger(s, start);
    case TOK_COMMA: return parseUnionOfIndexes(s, start);
    default: return processSubscript(s, start);
    }
}

/* IndexExpr ::= Filter | SliceStartsWithColon | SliceOrUnionOrIndex */
static jsonPathCode parseIndexExpr(selector *s) {
    switch (s->lex.next.type) {
    case TOK_END: return JSON_PATH_ERR_EMPTY_EXPR_TOKEN;
    case TOK_QUESTION_MARK: return parseFilter(s);
    case TOK_COLON: return parseSliceStartsWithColon(s);
    case TOK_COMMA: return JSON_PATH_ERR_INVALID_PATH;
    default: return parseSliceOrUnionOrIndex(s);
    }
}

/* BracketPathElement ::= "[" {SPACE} ( WildcardInBrackets | ((NameInBrackets | IndexExpr) ) {SPACE} "]") */
static jsonPathCode parseBracketPathElement(selector *s) {
    if (!matchToken(&s->lex, TOK_LBRACKET, 1)) return JSON_PATH_ERR_INVALID_PATH;
    jsonPathCode rc;
    int t = s->lex.next.type;
    if (t == TOK_WILDCARD) {
        rc = parseWildcardInBrackets(s);
    } else if (t == TOK_SINGLE_QUOTE || t == TOK_DOUBLE_QUOTE) {
        rc = parseNameInBrackets(s);
    } else {
        rc = parseIndexExpr(s);
    }
    if (rc != JSON_PATH_OK) return rc;
    skipSpaces(&s->lex);
    return JSON_PATH_OK;
}

/* Key ::= "*" [ [ "." ] WildcardFilter ] | UnquotedMemberName */
static jsonPathCode parseKey(selector *s) {
    pathLexer *l = &s->lex;
    if (matchToken(l, TOK_WILDCARD, 0)) {
        if (l->next.type == TOK_DOT) nextToken(l, 0);
        if (l->next.type == TOK_LBRACKET && peekToken(l) == TOK_QUESTION_MARK) return parseWildcardFilter(s);
        return processWildcard(s);
    }
    const char *name;
    size_t len;
    jsonPathCode rc = scanUnquotedMemberName(l, &name, &len);
    if (rc != JSON_PATH_OK) return rc;
    return traverseToObjectMember(s, name, len);
}

/* QualifiedPath ::= QualifiedPathElement RelativePath */
static jsonPathCode parseQualifiedPath(selector *s) {
    jsonPathCode rc = s->lex.next.type == TOK_LBRACKET ? parseBracketPathElement(s) : parseKey(s);
    if (rc != JSON_PATH_OK) return rc;
    return parseRelativePath(s);
}

/* BracketPath ::= BracketPathElement [ RelativePath ] */
static jsonPathCode parseBracketPath(selector *s) {
    jsonPathCode rc = parseBracketPathElement(s);
    if (rc != JSON_PATH_OK) return rc;
    if (s->lex.next.type == TOK_END) return JSON_PATH_OK;
    return parseRelativePath(s);
}

/* RelativePath ::= empty | RecursivePath | DotPath | BracketPath | QualifiedPath */
static jsonPathCode parseRelativePath(selector *s) {
    if (s->node == NULL || matchToken(&s->lex, TOK_END, 0)) return JSON_PATH_OK;
    switch (s->lex.next.type) {
    case TOK_DOTDOT: return parseRecursivePath(s);
    case TOK_DOT: nextToken(&s->lex, 0); return parseQualifiedPath(s);
    case TOK_LBRACKET: return parseBracketPath(s);
    default: return parseQualifiedPath(s);
    }
}

/* SupportedPath ::= ["$" | "."] RelativePath */
static jsonPathCode parseSupportedPath(selector *s) {
    if (s->node == NULL || matchToken(&s->lex, TOK_END, 0)) return JSON_PATH_OK;
    if (matchToken(&s->lex, TOK_DOLLAR, 0)) {
        if (s->node != s->root) return JSON_PATH_ERR_DOLLAR_NON_ROOT;
        s->r->v2 = 1;
    } else {
        matchToken(&s->lex, TOK_DOT, 0);
    }
    return parseRelativePath(s);
}

/* Parse the rest of the path at the current node and select the node the
 * parse ends on, if any. */
static jsonPathCode eval(selector *s) {
    jsonPathCode rc = JSON_PATH_ERR_RECURSION_LIMIT;
    if (!enterRecursion(s)) {
        rc = parseSupportedPath(s);
        if (rc == JSON_PATH_OK && s->node != NULL) {
            jsonPathMatch m = {s->node, s->parent, s->index, s->depth, NULL};
            if (s->node_path) m.pointer = sdsdup(s->node_path);
            addMatch(s->r, &m);
        }
    }
    leaveRecursion(s);
    return rc;
}

static jsonPathCode runSelector(jsonPathResult *r, jsonValue *root, const char *path, jsonPathMode mode, int *recursion) {
    if (strlen(path) > JSON_PATH_MAX_QUERY_SIZE) return JSON_PATH_ERR_QUERY_SIZE_LIMIT;
    jsonPathClear(r);

    selector s;
    memset(&s, 0, sizeof(s));
    s.r = r;
    s.root = root;
    s.node = root;
    s.mode = mode;
    s.recursion = recursion;
    s.node_path = mode == JSON_PATH_READ ? NULL : sdsempty();
    s.lex.p = path;
    s.lex.next.type = TOK_UNKNOWN;
    s.lex.next.str = path;
    s.lex.rd_tokens = &r->rd_tokens;
    nextToken(&s.lex, 0);

    jsonPathCode rc = eval(&s);
    uniqueInserts(r);
    sdsfree(s.node_path);
    return rc;
}

jsonPathCode jsonPathEval(jsonPathResult *r, jsonValue *root, const char *path, jsonPathMode mode) {
    int recursion = 0;
    return runSelector(r, root, path, mode, &recursion);
}

int jsonPathIsRoot(const char *path) {
    return strcmp(path, ".") == 0 || strcmp(path, "$") == 0;
}

int jsonPathIsV2(const char *path) {
    return path[0] == '$';
}

const char *jsonPathErrorMessage(jsonPathCode code) {
    switch (code) {
    case JSON_PATH_OK: return "";
    case JSON_PATH_ERR_INVALID_PATH: return "SYNTAXERR Invalid JSON path";
    case JSON_PATH_ERR_INVALID_WILDCARD: return "ERR Invalid use of wildcard";
    case JSON_PATH_ERR_INVALID_MEMBER_NAME: return "SYNTAXERR Invalid object member name";
    case JSON_PATH_ERR_INVALID_NUMBER: return "SYNTAXERR Invalid number";
    case JSON_PATH_ERR_INVALID_IDENTIFIER: return "SYNTAXERR Invalid identifier";
    case JSON_PATH_ERR_INVALID_DOT_SEQUENCE: return "SYNTAXERR Invalid dot sequence";
    case JSON_PATH_ERR_EMPTY_EXPR_TOKEN: return "SYNTAXERR Expression token cannot be empty";
    case JSON_PATH_ERR_INDEX_NOT_NUMBER: return "SYNTAXERR Array index is not a number";
    case JSON_PATH_ERR_STEP_ZERO: return "SYNTAXERR Step in the slice cannot be zero";
    case JSON_PATH_ERR_NOT_EXIST: return "NONEXISTENT JSON path does not exist";
    case JSON_PATH_ERR_NOT_OBJECT: return "WRONGTYPE JSON element is not an object";
    case JSON_PATH_ERR_NOT_ARRAY: return "WRONGTYPE JSON element is not an array";
    case JSON_PATH_ERR_OUT_OF_BOUNDS: return "OUTOFBOUNDARIES Array index is out of bounds";
    case JSON_PATH_ERR_JSON_PARSE: return "SYNTAXERR Failed to parse JSON string due to syntax error";
    case JSON_PATH_ERR_RECURSION_LIMIT: return "LIMIT Parser recursion depth is exceeded";
    case JSON_PATH_ERR_DESCENT_LIMIT:
        return "LIMIT Total number of recursive descent tokens in the query string exceeds the limit";
    case JSON_PATH_ERR_QUERY_SIZE_LIMIT: return "LIMIT Query string size limit is exceeded";
    case JSON_PATH_ERR_INSERT_NON_OBJECT: return "ERROR Cannot insert a member into a non-object value";
    case JSON_PATH_ERR_DOLLAR_NON_ROOT: return "SYNTAXERR Dollar sign cannot apply to non-root element";
    }
    return "";
}

/* ----------------------------------------------------------------------------
 * JSON Pointer, as RapidJSON's GenericPointer implements it
 * ------------------------------------------------------------------------- */

typedef struct ptrToken {
    const char *name; /* Decoded, into the parse buffer. */
    size_t len;
    uint32_t index; /* PTR_INVALID_INDEX unless the name is a canonical decimal. */
} ptrToken;

typedef struct ptrParsed {
    ptrToken *tokens;
    size_t count;
    char *buf;
} ptrParsed;

/* Returns 0 if ptr is not a valid pointer. URI fragment pointers ("#...") are
 * not supported; the paths built here never start with '#'. */
static int ptrParse(const char *ptr, size_t len, ptrParsed *out) {
    size_t count = 0, i = 0;
    for (size_t k = 0; k < len; k++) {
        if (ptr[k] == '/') count++;
    }
    out->tokens = NULL;
    out->buf = NULL;
    out->count = 0;
    if (len != 0 && ptr[0] != '/') return 0;
    ptrToken *tokens = zmalloc(sizeof(ptrToken) * (count ? count : 1));
    char *name = zmalloc(len + 1);
    out->tokens = tokens;
    out->buf = name;

    while (i < len) {
        i++; /* '/' */
        ptrToken *t = &tokens[out->count++];
        t->name = name;
        int is_number = 1;
        while (i < len && ptr[i] != '/') {
            char c = ptr[i++];
            if (c == '~') {
                if (i < len && ptr[i] == '0') {
                    c = '~';
                } else if (i < len && ptr[i] == '1') {
                    c = '/';
                } else {
                    return 0;
                }
                i++;
            }
            if (c < '0' || c > '9') is_number = 0;
            *name++ = c;
        }
        t->len = (size_t)(name - t->name);
        if (t->len == 0) is_number = 0;
        *name++ = '\0';
        if (is_number && t->len > 1 && t->name[0] == '0') is_number = 0;
        uint32_t n = 0;
        if (is_number) {
            for (size_t j = 0; j < t->len; j++) {
                uint32_t m = n * 10 + (uint32_t)(t->name[j] - '0');
                if (m < n) {
                    is_number = 0;
                    break;
                }
                n = m;
            }
        }
        t->index = is_number ? n : PTR_INVALID_INDEX;
    }
    return 1;
}

static void ptrFree(ptrParsed *p) {
    zfree(p->tokens);
    zfree(p->buf);
}

static jsonValue *ptrStep(jsonValue *v, const ptrToken *t) {
    if (v->type == JSON_OBJECT) return jsonObjectFind(v, t->name, t->len, NULL);
    if (v->type == JSON_ARRAY) {
        if (t->index == PTR_INVALID_INDEX || t->index >= v->array.len) return NULL;
        return v->array.items[t->index];
    }
    return NULL;
}

jsonValue *jsonPointerGet(jsonValue *root, const char *ptr, size_t len) {
    ptrParsed p;
    jsonValue *v = root;
    if (!ptrParse(ptr, len, &p)) v = NULL;
    for (size_t k = 0; v && k < p.count; k++) v = ptrStep(v, &p.tokens[k]);
    ptrFree(&p);
    return v;
}

/* Pointer::Set. Missing members and elements are created as null on the way,
 * and a value of the wrong kind for a token is replaced by an empty object or
 * array. Takes ownership of v even on failure. */
int jsonPointerSet(jsonValue *root, const char *ptr, size_t len, jsonValue *v) {
    ptrParsed p;
    if (!ptrParse(ptr, len, &p)) {
        ptrFree(&p);
        jsonFree(v);
        return 0;
    }
    jsonValue *cur = root;
    for (size_t k = 0; k < p.count; k++) {
        const ptrToken *t = &p.tokens[k];
        if (cur->type == JSON_ARRAY && t->len == 1 && t->name[0] == '-') {
            jsonArrayAppend(cur, jsonCreateNull());
            cur = cur->array.items[cur->array.len - 1];
            continue;
        }
        if (t->index == PTR_INVALID_INDEX) {
            if (cur->type != JSON_OBJECT) jsonReplace(cur, jsonCreateObject());
        } else if (cur->type != JSON_ARRAY && cur->type != JSON_OBJECT) {
            jsonReplace(cur, jsonCreateArray());
        }
        if (cur->type == JSON_ARRAY) {
            while (t->index >= cur->array.len) jsonArrayAppend(cur, jsonCreateNull());
            cur = cur->array.items[t->index];
        } else {
            jsonValue *child = jsonObjectFind(cur, t->name, t->len, NULL);
            if (child == NULL) {
                child = jsonCreateNull();
                jsonObjectSet(cur, t->name, t->len, child);
            }
            cur = child;
        }
    }
    jsonReplace(cur, v);
    ptrFree(&p);
    return 1;
}

int jsonPointerErase(jsonValue *root, const char *ptr, size_t len) {
    ptrParsed p;
    int ok = 0;
    if (!ptrParse(ptr, len, &p) || p.count == 0) goto done;
    jsonValue *v = root;
    for (size_t k = 0; v && k + 1 < p.count; k++) v = ptrStep(v, &p.tokens[k]);
    if (v == NULL) goto done;
    const ptrToken *last = &p.tokens[p.count - 1];
    if (v->type == JSON_OBJECT) {
        ok = jsonObjectDelete(v, last->name, last->len);
    } else if (v->type == JSON_ARRAY && last->index != PTR_INVALID_INDEX && last->index < v->array.len) {
        jsonArrayDeleteRange(v, last->index, 1);
        ok = 1;
    }
done:
    ptrFree(&p);
    return ok;
}

/* ----------------------------------------------------------------------------
 * Applying writes
 * ------------------------------------------------------------------------- */

/* Swap the contents of two values, keeping both addresses. */
static void jsonSwap(jsonValue *a, jsonValue *b) {
    jsonValue tmp = *a;
    *a = *b;
    *b = tmp;
}

jsonPathCode jsonPathCommitInserts(jsonValue *root, jsonPathResult *r, const jsonValue *v) {
    for (size_t k = 0; k < r->ninserts; k++) {
        if (!jsonPointerSet(root, r->inserts[k], sdslen(r->inserts[k]), jsonDup(v))) return JSON_PATH_ERR_INVALID_PATH;
    }
    return JSON_PATH_OK;
}

/* A single update swaps v into place, so v then holds the old value and that is
 * what the inserts receive. With several updates each gets a copy, and one is
 * skipped if an earlier update removed its path. */
jsonPathCode jsonPathCommit(jsonValue *root, jsonPathResult *r, jsonValue *v) {
    if (r->len == 0 && r->ninserts == 0) {
        jsonFree(v);
        return r->error;
    }
    jsonPathDedupe(r);
    if (r->len == 1) {
        jsonSwap(r->matches[0].value, v);
    } else {
        for (size_t k = 0; k < r->len; k++) {
            sds ptr = r->matches[k].pointer;
            jsonValue *target = jsonPointerGet(root, ptr, sdslen(ptr));
            if (target) jsonReplace(target, jsonDup(v));
        }
    }

    jsonPathCode rc = JSON_PATH_OK;
    if (r->ninserts == 1) {
        if (!jsonPointerSet(root, r->inserts[0], sdslen(r->inserts[0]), v)) rc = JSON_PATH_ERR_INVALID_PATH;
        return rc;
    }
    rc = jsonPathCommitInserts(root, r, v);
    jsonFree(v);
    return rc;
}

/* The module's delete order: deepest first, then by pointer tokens descending
 * (numeric index, then length, then bytes), so later elements of an array go
 * before earlier ones. */
static int ptrTokenCompare(const ptrToken *a, const ptrToken *b) {
    if (a->index != b->index) return a->index < b->index ? -1 : 1;
    if (a->len != b->len) return a->len < b->len ? -1 : 1;
    return memcmp(a->name, b->name, a->len);
}

static int deleteOrderCompare(const void *x, const void *y) {
    const ptrParsed *a = x, *b = y;
    if (a->count != b->count) return a->count > b->count ? -1 : 1;
    for (size_t k = 0; k < a->count; k++) {
        int cmp = ptrTokenCompare(&a->tokens[k], &b->tokens[k]);
        if (cmp) return -cmp;
    }
    return 0;
}

size_t jsonPathDeleteMatches(jsonValue *root, jsonPathResult *r) {
    size_t deleted = 0;
    if (r->len == 1) {
        sds ptr = r->matches[0].pointer;
        if (jsonPointerGet(root, ptr, sdslen(ptr))) deleted += jsonPointerErase(root, ptr, sdslen(ptr));
        return deleted;
    }
    ptrParsed *order = zmalloc(sizeof(ptrParsed) * (r->len ? r->len : 1));
    size_t n = 0;
    for (size_t k = 0; k < r->len; k++) {
        if (ptrParse(r->matches[k].pointer, sdslen(r->matches[k].pointer), &order[n])) {
            n++;
        } else {
            ptrFree(&order[n]);
        }
    }
    qsort(order, n, sizeof(ptrParsed), deleteOrderCompare);
    for (size_t k = 0; k < n; k++) {
        /* Equal pointers are adjacent; deleting an array index twice would
         * remove its successor. */
        if (k > 0 && deleteOrderCompare(&order[k - 1], &order[k]) == 0) continue;
        jsonValue *v = root;
        for (size_t t = 0; v && t < order[k].count; t++) v = ptrStep(v, &order[k].tokens[t]);
        if (v == NULL || order[k].count == 0) continue;
        jsonValue *parent = root;
        for (size_t t = 0; t + 1 < order[k].count; t++) parent = ptrStep(parent, &order[k].tokens[t]);
        const ptrToken *last = &order[k].tokens[order[k].count - 1];
        if (parent->type == JSON_OBJECT) {
            deleted += jsonObjectDelete(parent, last->name, last->len);
        } else {
            jsonArrayDeleteRange(parent, last->index, 1);
            deleted++;
        }
    }
    for (size_t k = 0; k < n; k++) ptrFree(&order[k]);
    zfree(order);
    return deleted;
}
