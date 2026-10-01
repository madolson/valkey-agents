/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "json.h"
#include "json_path.h"
#include "sds.h"
#include "zmalloc.h"
}

/* Every test checks that all memory it allocated is released. */
class JsonPathTest : public ::testing::Test {
  protected:
    size_t mem_before = 0;

    void SetUp() override {
        mem_before = zmalloc_used_memory();
    }

    void TearDown() override {
        EXPECT_EQ(zmalloc_used_memory(), mem_before) << "Memory leak detected";
    }
};

static jsonValue *parse(const char *in) {
    int err;
    size_t depth;
    jsonValue *v = jsonParse(in, strlen(in), JSON_DEFAULT_MAX_DEPTH, &err, &depth);
    EXPECT_NE(v, nullptr) << "bad test document: " << in;
    return v;
}

static sds serialize(const jsonValue *v) {
    return jsonSerialize(sdsempty(), v, NULL, 0);
}

/* The matches as a JSON array, prefixed with the error message if any. */
static sds matchesToString(const jsonPathResult *r, jsonPathCode rc) {
    sds s = sdsempty();
    if (rc != JSON_PATH_OK) s = sdscatfmt(s, "%s ", jsonPathErrorMessage(rc));
    s = sdscat(s, "[");
    for (size_t i = 0; i < r->len; i++) {
        if (i) s = sdscat(s, ",");
        s = jsonSerialize(s, r->matches[i].value, NULL, 0);
    }
    return sdscat(s, "]");
}

static sds evalToString(const char *doc, const char *path, jsonPathMode mode) {
    jsonValue *d = parse(doc);
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonPathEval(&r, d, path, mode);
    sds s = matchesToString(&r, rc);
    jsonPathFree(&r);
    jsonFree(d);
    return s;
}

static void expectRead(const char *doc, const char *path, const char *expected) {
    sds s = evalToString(doc, path, JSON_PATH_READ);
    EXPECT_STREQ(s, expected) << "doc: " << doc << " path: " << path;
    sdsfree(s);
}

static void expectWrite(const char *doc, const char *path, const char *expected) {
    sds s = evalToString(doc, path, JSON_PATH_WRITE);
    EXPECT_STREQ(s, expected) << "doc: " << doc << " path: " << path;
    sdsfree(s);
}

/* JSON.SET without NX/XX on a non-root path: the document afterwards, or the
 * error. */
static sds setToString(const char *doc, const char *path, const char *value) {
    jsonValue *d = parse(doc);
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonPathEval(&r, d, path, JSON_PATH_WRITE);
    sds s;
    if (rc != JSON_PATH_OK) {
        s = sdsnew(jsonPathErrorMessage(rc));
    } else {
        jsonPathCommit(d, &r, parse(value));
        s = serialize(d);
    }
    jsonPathFree(&r);
    jsonFree(d);
    return s;
}

static void expectSet(const char *doc, const char *path, const char *value, const char *expected) {
    sds s = setToString(doc, path, value);
    EXPECT_STREQ(s, expected) << "doc: " << doc << " path: " << path;
    sdsfree(s);
}

/* JSON.DEL's use of a delete-mode result: "<count> <document>". */
static void expectDel(const char *doc, const char *path, const char *expected) {
    jsonValue *d = parse(doc);
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathCode rc = jsonPathEval(&r, d, path, JSON_PATH_DELETE);
    sds s;
    if (rc != JSON_PATH_OK) {
        s = sdsnew(jsonPathErrorMessage(rc));
    } else {
        size_t n = jsonPathDeleteMatches(d, &r);
        s = sdscatfmt(sdsempty(), "%U ", (unsigned long long)n);
        s = jsonSerialize(s, d, NULL, 0);
    }
    EXPECT_STREQ(s, expected) << "doc: " << doc << " path: " << path;
    sdsfree(s);
    jsonPathFree(&r);
    jsonFree(d);
}

static const char *STORE = "{\"a\":{\"a\":{\"a\":1},\"b\":[1,2,3]},\"arr\":[1,[2,[3]],{\"x\":5}],\"s\":\"str\"}";

TEST_F(JsonPathTest, RootPaths) {
    expectRead("{\"a\":1}", "$", "[{\"a\":1}]");
    expectRead("{\"a\":1}", ".", "[{\"a\":1}]");
    expectRead("{\"a\":1}", "", "[{\"a\":1}]");
    EXPECT_TRUE(jsonPathIsRoot("$"));
    EXPECT_TRUE(jsonPathIsRoot("."));
    EXPECT_FALSE(jsonPathIsRoot(""));
    EXPECT_FALSE(jsonPathIsRoot("$."));
    EXPECT_TRUE(jsonPathIsV2("$.a"));
    EXPECT_FALSE(jsonPathIsV2(".a"));
}

TEST_F(JsonPathTest, LegacyAndV2Flag) {
    jsonValue *d = parse("{\"a\":{\"b\":1}}");
    jsonPathResult r;
    jsonPathInit(&r);
    const char *legacy[] = {".", "a", ".a.b", "a.b", "[\"a\"]", "['a'].b", "..b", "*"};
    const char *v2[] = {"$", "$.a", "$a.b", "$['a']", "$..b"};
    for (size_t i = 0; i < sizeof(legacy) / sizeof(legacy[0]); i++) {
        jsonPathFree(&r);
        jsonPathEval(&r, d, legacy[i], JSON_PATH_READ);
        EXPECT_FALSE(r.v2) << legacy[i];
    }
    for (size_t i = 0; i < sizeof(v2) / sizeof(v2[0]); i++) {
        jsonPathFree(&r);
        jsonPathEval(&r, d, v2[i], JSON_PATH_READ);
        EXPECT_TRUE(r.v2) << v2[i];
    }
    jsonPathFree(&r);
    jsonFree(d);
}

TEST_F(JsonPathTest, MemberSteps) {
    expectRead(STORE, "$.a.a", "[{\"a\":1}]");
    expectRead(STORE, ".a.a.a", "[1]");
    expectRead(STORE, "a.b", "[[1,2,3]]");
    expectRead(STORE, "$a.b", "[[1,2,3]]");
    expectRead(STORE, "$['a'][\"b\"]", "[[1,2,3]]");
    expectRead(STORE, "$[ 'a' ]", "[{\"a\":{\"a\":1},\"b\":[1,2,3]}]");
    expectRead(STORE, "$['a','s','nope','s']", "[{\"a\":{\"a\":1},\"b\":[1,2,3]},\"str\",\"str\"]");
    expectRead(STORE, "$.nope", "[]");
    expectRead(STORE, "$.s.x", "[]");
    /* A member name runs to the next terminator. */
    expectRead("{\"a$b\":1,\"a-b\":2}", "$.a$b", "[1]");
    expectRead("{\"a$b\":1,\"a-b\":2}", "$.a-b", "[2]");
    /* Escapes: double quoted names are JSON, single quoted ones decode only
     * a few sequences. */
    expectRead("{\"a\\\"b\":1,\"a'b\":2,\"a\\\\b\":3}", "$[\"a\\\"b\"]", "[1]");
    expectRead("{\"a\\\"b\":1,\"a'b\":2,\"a\\\\b\":3}", "$['a\\'b']", "[2]");
    expectRead("{\"a\\\"b\":1,\"a'b\":2,\"a\\\\b\":3}", "$['a\\\\b']", "[3]");
    expectRead("{\"\\u00e9\":1}", "$[\"\\u00e9\"]", "[1]");
}

TEST_F(JsonPathTest, ArrayIndexes) {
    expectRead("[1,2,3]", "$[0]", "[1]");
    expectRead("[1,2,3]", "$[-1]", "[3]");
    expectRead("[1,2,3]", "$[+1]", "[2]");
    expectRead("[1,2,3]", "[2]", "[3]");
    expectRead("[1,2,3]", "$[ 1 ]", "[2]");
    expectRead("[1,2,3]", "$[3]", "OUTOFBOUNDARIES Array index is out of bounds []");
    expectRead("[1,2,3]", "$[-4]", "OUTOFBOUNDARIES Array index is out of bounds []");
    expectRead("{\"a\":1}", "$[0]", "WRONGTYPE JSON element is not an array []");
    expectRead("[[1,2],[3,4]]", "$[1][0]", "[3]");
    expectRead("[1,2,3]", "$[0,2]", "[1,3]");
    expectRead("[1,2,3]", "$[2,0,0,-1,7]", "[3,1,1,3]");
    /* On an empty array the union bound wraps, so index 0 is tried. */
    expectRead("[]", "$[0,1]", "OUTOFBOUNDARIES Array index is out of bounds []");
}

TEST_F(JsonPathTest, Slices) {
    const char *a = "[0,1,2,3,4,5]";
    expectRead(a, "$[1:3]", "[1,2]");
    expectRead(a, "$[:2]", "[0,1]");
    expectRead(a, "$[4:]", "[4,5]");
    expectRead(a, "$[:]", "[0,1,2,3,4,5]");
    expectRead(a, "$[::2]", "[0,2,4]");
    expectRead(a, "$[-2:]", "[4,5]");
    expectRead(a, "$[-100:100]", "[0,1,2,3,4,5]");
    expectRead(a, "$[ 1 : 5 : 2 ]", "[1,3]");
    /* Negative steps walk down from start, which defaults to 0, not the end. */
    expectRead(a, "$[::-1]", "[]");
    expectRead(a, "$[4:1:-1]", "[4,3,2]");
    expectRead(a, "$[-1:0:-2]", "[5,3,1]");
    expectRead(a, "$[9:0:-1]", "[5,4,3,2,1]");
    /* The loop counter is an int: this step wraps to 1. */
    expectRead(a, "$[0:3:4294967297]", "[0,1,2]");
    expectRead(a, "$[0:3:0]", "SYNTAXERR Step in the slice cannot be zero []");
    expectRead("{}", "$[0:1]", "WRONGTYPE JSON element is not an array []");
}

TEST_F(JsonPathTest, Wildcards) {
    expectRead(STORE, "$.*", "[{\"a\":{\"a\":1},\"b\":[1,2,3]},[1,[2,[3]],{\"x\":5}],\"str\"]");
    expectRead(STORE, "$.a.b[*]", "[1,2,3]");
    expectRead(STORE, "$.a.*.*", "[1,1,2,3]");
    expectRead(STORE, "$.arr.*.x", "[5]");
    expectRead("{\"a\":1}", "$.a.*", "ERR Invalid use of wildcard []");
    /* A legacy wildcard on a scalar is a syntax error. */
    expectRead("{\"a\":1}", ".a.*", "SYNTAXERR Invalid JSON path []");
    /* Inside another wildcard the error only ends that branch. */
    expectRead("{\"a\":1,\"b\":{\"c\":2}}", "$.*.*", "[2]");
    expectRead("{\"a\":1}", "$.x.*", "[]");
    expectRead("{\"a\":1}", "$.*.", "[1]");
}

TEST_F(JsonPathTest, RecursiveDescent) {
    /* Descendant-or-self in DFS preorder. */
    expectRead(STORE, "$..a", "[{\"a\":{\"a\":1},\"b\":[1,2,3]},{\"a\":1},1]");
    expectRead("{\"a\":{\"b\":1},\"c\":[{\"b\":2}],\"b\":3}", "$..b", "[3,1,2]");
    expectRead("{\"a\":{\"b\":1},\"c\":[{\"b\":2}],\"b\":3}", "..b", "[3,1,2]");
    expectRead("[[1,[2]],[3]]", "$..[0]", "[[1,[2]],1,2,3]");
    expectRead("{\"a\":{\"b\":1}}", "$..", "[{\"a\":{\"b\":1}},{\"b\":1}]");
    expectRead("{\"a\":{\"b\":{\"c\":1}}}", "$..*", "[{\"b\":{\"c\":1}},{\"c\":1},1]");
    /* Duplicates reached along several routes are dropped. */
    expectRead("{\"a\":{\"a\":{\"a\":1}}}", "$..a..a", "[{\"a\":1},1]");
    expectRead("{\"a\":1}", "$...a", "SYNTAXERR Invalid dot sequence []");
    expectRead("{\"a\":1}", "$..a.", "SYNTAXERR Invalid object member name []");
}

TEST_F(JsonPathTest, Filters) {
    const char *a = "[{\"a\":1,\"b\":\"x\"},{\"a\":2,\"b\":\"y\"},{\"a\":3},{\"b\":\"z\"},5,\"s\",null]";
    expectRead(a, "$[?(@.a>1)]", "[{\"a\":2,\"b\":\"y\"},{\"a\":3}]");
    expectRead(a, "$[?(@.a >= 2 && @.b == \"y\")]", "[{\"a\":2,\"b\":\"y\"}]");
    expectRead(a, "$[?(@.b=='x')].a", "[1]");
    /* Union keeps the first term's order, then the new elements. */
    expectRead(a, "$[?(@.a==3 || @.a==1)].a", "[3,1]");
    expectRead(a, "$[?((@.a>1) && (@.a<3 || @.b))].a", "[2]");
    expectRead(a, "$[?(@.b)].b", "[\"x\",\"y\",\"z\"]");
    expectRead(a, "$[?(@==5)]", "[5]");
    expectRead(a, "$[?(@==null)]", "[null]");
    expectRead(a, "$[?(2<@.a)].a", "[3]");
    expectRead(a, "$[*][?(@.a==1)]", "[{\"a\":1,\"b\":\"x\"}]");
    expectRead(a, "$.*[?(@.a==1)]", "[{\"a\":1,\"b\":\"x\"}]");
    /* On an object a filter keeps the object itself. */
    expectRead("{\"a\":1}", "$[?(@.a==1)]", "[{\"a\":1}]");
    /* Values from the document. */
    expectRead("{\"m\":2,\"l\":[1,2,3]}", "$.l[?(@>=$.m)]", "[2,3]");
    expectRead("{\"m\":[2],\"l\":[1,2,3]}", "$.l[?(@>=$.m)]", "SYNTAXERR Invalid JSON path []");
    /* Array contains and array element. */
    const char *t = "[{\"t\":[1,\"a\"]},{\"t\":[\"b\",3]},{\"u\":1}]";
    expectRead(t, "$[?(@.t[?(@==\"b\")])]", "[{\"t\":[\"b\",3]}]");
    expectRead(t, "$[?(@.t[0]==1)]", "[{\"t\":[1,\"a\"]}]");
    expectRead(t, "$[?(@.t[-1]==3)]", "[{\"t\":[\"b\",3]}]");
}

TEST_F(JsonPathTest, FilterNumbers) {
    const char *n = "[1,1.0,-1,1e2,18446744073709551615,9223372036854775807,true,\"1\"]";
    /* Integers compare exactly, anything else as doubles. */
    expectRead(n, "$[?(@==1)]", "[1,1.0]");
    expectRead(n, "$[?(@==100)]", "[1e2]");
    expectRead(n, "$[?(@<18446744073709551615)]", "[1,1.0,-1,1e2,9223372036854775807]");
    expectRead(n, "$[?(@>9223372036854775806)]", "[18446744073709551615,9223372036854775807]");
    expectRead(n, "$[?(@==true)]", "[true]");
    expectRead(n, "$[?(@>false)]", "[true]");
    expectRead(n, "$[?(@==\"1\")]", "[\"1\"]");
}

TEST_F(JsonPathTest, SyntaxErrors) {
    const char *d = "{\"a\":[1,2]}";
    expectRead(d, "$.a[", "SYNTAXERR Expression token cannot be empty []");
    expectRead(d, "$.a[x]", "SYNTAXERR Array index is not a number []");
    expectRead(d, "$.a[0,]", "SYNTAXERR Invalid JSON path []");
    expectRead(d, "$.a[,0]", "SYNTAXERR Invalid JSON path []");
    expectRead(d, "$.a[0", "SYNTAXERR Invalid JSON path []");
    expectRead(d, "$.a.", "SYNTAXERR Invalid object member name []");
    expectRead(d, "$.a[?(@==x)]", "SYNTAXERR Invalid number []");
    expectRead(d, "$.a[?(@==nul)]", "SYNTAXERR Invalid identifier []");
    expectRead(d, "$.a[?(@=1)]", "SYNTAXERR Invalid JSON path []");
    expectRead(d, "$.a[?(@==1.2.3)]", "SYNTAXERR Failed to parse JSON string due to syntax error []");
    expectRead(d, "$.a$", "[]");
    expectRead(d, "$.*$", "[]");
    /* Nothing after a step that matched nothing is parsed. */
    expectRead(d, "$.x[[[", "[]");
    expectRead(d, "$.x.", "[]");
}

TEST_F(JsonPathTest, Limits) {
    /* Every branch of the wildcard lexes the '..' again, and each time counts. */
    sds keys = sdsnew("{");
    for (int i = 0; i < 20; i++) keys = sdscatfmt(keys, "%s\"k%i\":{\"a\":{}}", i ? "," : "", i);
    keys = sdscat(keys, "}");
    expectRead(keys, "$.*.a..b", "[]");
    sdsfree(keys);
    keys = sdsnew("{");
    for (int i = 0; i < 21; i++) keys = sdscatfmt(keys, "%s\"k%i\":{\"a\":{}}", i ? "," : "", i);
    keys = sdscat(keys, "}");
    expectRead(keys, "$.*.a..b", "LIMIT Total number of recursive descent tokens in the query string exceeds the limit []");
    sdsfree(keys);

    sds p = sdsnew("$[?(");
    for (int i = 0; i < 70; i++) p = sdscat(p, "(");
    p = sdscat(p, "@==1");
    for (int i = 0; i < 70; i++) p = sdscat(p, ")");
    p = sdscat(p, ")]");
    expectRead("[1]", p, "LIMIT Parser recursion depth is exceeded []");
    sdsfree(p);

    p = sdsgrowzero(sdsnew("$."), 128 * 1024 + 1);
    memset(p + 2, 'a', sdslen(p) - 2);
    expectRead("{}", p, "LIMIT Query string size limit is exceeded []");
    sdsfree(p);
}

/* The v2 flag and the '..' count carry over between calls, as JSON.GET with
 * several paths relies on. */
TEST_F(JsonPathTest, ReusedResult) {
    jsonValue *d = parse("{\"a\":1}");
    jsonPathResult r;
    jsonPathInit(&r);
    r.v2 = 1;
    EXPECT_EQ(jsonPathEval(&r, d, "a.*", JSON_PATH_READ), JSON_PATH_ERR_INVALID_WILDCARD);
    jsonPathCode rc = JSON_PATH_OK;
    for (int i = 0; i < 21 && rc == JSON_PATH_OK; i++) rc = jsonPathEval(&r, d, "$..a", JSON_PATH_READ);
    EXPECT_EQ(rc, JSON_PATH_ERR_DESCENT_LIMIT);
    jsonPathFree(&r);
    jsonFree(d);
}

TEST_F(JsonPathTest, MatchLocation) {
    jsonValue *d = parse(STORE);
    jsonPathResult r;
    jsonPathInit(&r);
    ASSERT_EQ(jsonPathEval(&r, d, "$.arr[1][1][0]", JSON_PATH_DELETE), JSON_PATH_OK);
    ASSERT_EQ(r.len, 1u);
    EXPECT_EQ(r.matches[0].depth, 4u);
    EXPECT_EQ(r.matches[0].index, 0u);
    EXPECT_EQ(r.matches[0].parent, jsonChildAt(jsonChildAt(jsonChildAt(d, 1), 1), 1));
    EXPECT_STREQ(r.matches[0].pointer, "/arr/1/1/0");
    EXPECT_EQ(r.matches[0].value, jsonPointerGet(d, "/arr/1/1/0", 10));

    ASSERT_EQ(jsonPathEval(&r, d, "$..a", JSON_PATH_READ), JSON_PATH_OK);
    ASSERT_EQ(r.len, 3u);
    EXPECT_EQ(r.matches[0].parent, d);
    EXPECT_EQ(r.matches[0].index, 0u);
    EXPECT_EQ(r.matches[2].depth, 3u);
    EXPECT_EQ(r.matches[2].pointer, nullptr);

    ASSERT_EQ(jsonPathEval(&r, d, "$", JSON_PATH_WRITE), JSON_PATH_OK);
    EXPECT_EQ(r.matches[0].parent, nullptr);
    EXPECT_STREQ(r.matches[0].pointer, "");
    jsonPathFree(&r);

    jsonValue *e = parse("{\"a/b\":{\"~\":1}}");
    ASSERT_EQ(jsonPathEval(&r, e, "$['a/b']['~']", JSON_PATH_WRITE), JSON_PATH_OK);
    EXPECT_STREQ(r.matches[0].pointer, "/a~1b/~0");
    jsonPathFree(&r);
    jsonFree(e);
    jsonFree(d);
}

TEST_F(JsonPathTest, MaxDepth) {
    jsonValue *d = parse("{\"a\":{\"b\":{}}}");
    jsonPathResult r;
    jsonPathInit(&r);
    jsonPathEval(&r, d, "$.a.b", JSON_PATH_WRITE);
    EXPECT_EQ(r.max_depth, 2u);
    jsonPathEval(&r, d, "$.a.b.c", JSON_PATH_WRITE);
    EXPECT_EQ(r.max_depth, 3u);
    jsonPathEval(&r, d, "$..b", JSON_PATH_WRITE);
    EXPECT_EQ(r.max_depth, 2u);
    jsonPathFree(&r);
    jsonFree(d);
}

TEST_F(JsonPathTest, WriteResolution) {
    /* Wrong-typed member step. */
    expectWrite("{\"a\":1}", "$.a.b", "ERROR Cannot insert a member into a non-object value []");
    expectWrite("{\"a\":1}", ".a.b", "ERROR Cannot insert a member into a non-object value []");
    expectWrite("{\"a\":1}", "$['a','b'].c", "[]");
    expectWrite("{\"a\":1}", "$.a['x','y']", "ERROR Cannot insert a member into a non-object value []");
    /* Missing member that is not the last step. */
    expectWrite("{\"a\":1}", "$.x.y", "NONEXISTENT JSON path does not exist []");
    expectWrite("{\"a\":1}", "$.x.*", "NONEXISTENT JSON path does not exist []");
    expectWrite("{\"a\":1}", "$.x[0]", "NONEXISTENT JSON path does not exist []");
    /* Wildcard on an existing scalar, but not under '..'. */
    expectWrite("{\"a\":1}", "$.a.*", "ERR Invalid use of wildcard []");
    expectWrite("{\"a\":1}", "$..a.*", "[]");
    expectWrite("{\"a\":{}}", "$.a.*", "[]");
    expectWrite("{\"a\":[1]}", "$.a[5]", "OUTOFBOUNDARIES Array index is out of bounds []");
    expectWrite("{\"a\":{}}", "$.a[0]", "WRONGTYPE JSON element is not an array []");

    /* The last step creates a member. Trailing junk after it is never read,
     * since the step peeks only one token ahead. */
    expectSet("{\"a\":1}", "$.b", "2", "{\"a\":1,\"b\":2}");
    expectSet("{\"a\":1}", "$.b ", "2", "{\"a\":1,\"b\":2}");
    expectSet("{\"a\":1}", "$.b]", "2", "{\"a\":1,\"b\":2}");
    expectSet("{\"a\":1}", "$.b.", "2", "{\"a\":1,\"b\":2}");
    expectSet("{\"a\":{},\"b\":{}}", "$.*.x", "2", "{\"a\":{\"x\":2},\"b\":{\"x\":2}}");
    expectSet("{\"a\":1}", "$..x", "2", "{\"a\":1}");
    expectSet("{\"a\":[1]}", "$.a[5:9]", "2", "{\"a\":[1]}");
    expectSet("{\"a\":[1]}", "$.a[5,6]", "2", "{\"a\":[1]}");
    expectSet("{\"a\":[1]}", "$.a[?(@>5)]", "2", "{\"a\":[1]}");
    expectSet("{\"a\":1}", "$.*.*", "2", "{\"a\":1}");
}

TEST_F(JsonPathTest, Commit) {
    expectSet("[1,2,3]", "$[0,2]", "0", "[0,2,0]");
    expectSet("{\"a\":1}", "$.a", "{\"b\":[1]}", "{\"a\":{\"b\":[1]}}");
    /* A single update swaps the new value in, and the inserts get the old one. */
    expectSet("{\"a\":{\"x\":1},\"b\":{}}", "$.*.x", "5", "{\"a\":{\"x\":5},\"b\":{\"x\":1}}");
    expectSet("{\"a\":{\"x\":1},\"b\":{\"x\":2},\"c\":{}}", "$.*.x", "5",
              "{\"a\":{\"x\":5},\"b\":{\"x\":5},\"c\":{\"x\":5}}");
    /* Later matches are looked up again after earlier updates. */
    expectSet("{\"a\":{\"a\":1}}", "$..a", "{\"a\":5}", "{\"a\":{\"a\":{\"a\":5}}}");
    expectSet("{\"a\":{\"a\":1}}", "$..a", "3", "{\"a\":3}");
    /* Insert names go into a JSON Pointer unescaped. */
    expectSet("{\"a\":1}", "$.x/y", "1", "{\"a\":1,\"x\":{\"y\":1}}");
    expectSet("{\"x\":5}", "$['x/y']", "1", "{\"x\":{\"y\":1}}");
    expectSet("{\"x\":[1]}", "$['x/2']", "7", "{\"x\":[1,null,7]}");
    expectSet("{\"x\":[1]}", "$['x/-']", "7", "{\"x\":[1,7]}");
    expectSet("{\"a\":1}", "$.x~1y", "1", "{\"a\":1,\"x/y\":1}");
    expectSet("{\"a\":1}", "$.x~y", "1", "{\"a\":1}");
}

TEST_F(JsonPathTest, Delete) {
    expectDel("[1,2,3,4]", "$[0,1,1,3]", "3 [3]");
    expectDel("[1,2,3,4]", "$[*]", "4 []");
    expectDel("[1,2,3,4]", "$[::2]", "2 [2,4]");
    expectDel("{\"a\":{\"a\":1}}", "$..a", "2 {}");
    expectDel("{\"a\":[{\"a\":1},{\"b\":2}],\"b\":[1,{\"a\":3}]}", "$..a", "3 {\"b\":[1,{}]}");
    expectDel("[[1,2],[3,4]]", "$[*][0]", "2 [[2],[4]]");
    expectDel("[[1,2],[3,4]]", "$..[0]", "3 [[4]]");
    expectDel("{\"a\":1}", "$", "0 {\"a\":1}");
    expectDel("{\"a\":1}", "$.a.b", "ERROR Cannot insert a member into a non-object value");
    expectDel("{\"a\":1}", "$.x.b", "0 {\"a\":1}");
}

TEST_F(JsonPathTest, Pointers) {
    jsonValue *d = parse("{\"a\":[1,{\"b~/\":2}],\"0\":3}");
    EXPECT_EQ(jsonPointerGet(d, "", 0), d);
    EXPECT_EQ(jsonPointerGet(d, "/0", 2)->integer, 3);
    EXPECT_EQ(jsonPointerGet(d, "/a/1/b~0~1", 10)->integer, 2);
    EXPECT_EQ(jsonPointerGet(d, "/a/01", 5), nullptr);
    EXPECT_EQ(jsonPointerGet(d, "/a/2", 4), nullptr);
    EXPECT_EQ(jsonPointerGet(d, "a", 1), nullptr);
    EXPECT_EQ(jsonPointerGet(d, "/a~2", 4), nullptr);
    EXPECT_FALSE(jsonPointerSet(d, "/x~", 3, jsonCreateNull()));
    EXPECT_FALSE(jsonPointerErase(d, "", 0));
    EXPECT_TRUE(jsonPointerErase(d, "/a/0", 4));
    EXPECT_FALSE(jsonPointerErase(d, "/a/5", 4));
    EXPECT_TRUE(jsonPointerSet(d, "/a/0/c/1", 8, jsonCreateBool(1)));
    sds s = serialize(d);
    EXPECT_STREQ(s, "{\"a\":[{\"b~/\":2,\"c\":[null,true]}],\"0\":3}");
    sdsfree(s);
    jsonFree(d);
}
