/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

#include <cfloat>
#include <cstdio>
#include <cstring>

extern "C" {
#include "json.h"
#include "sds.h"
#include "zmalloc.h"
}

/* Every test checks that all memory it allocated is released. */
class JsonTest : public ::testing::Test {
  protected:
    size_t mem_before = 0;

    void SetUp() override {
        mem_before = zmalloc_used_memory();
    }

    void TearDown() override {
        EXPECT_EQ(zmalloc_used_memory(), mem_before) << "Memory leak detected";
    }
};

static jsonValue *parseLen(const char *in, size_t len, int *err) {
    size_t depth;
    return jsonParse(in, len, JSON_DEFAULT_MAX_DEPTH, err, &depth);
}

static jsonValue *parse(const char *in) {
    int err;
    return parseLen(in, strlen(in), &err);
}

/* Parse and serialize compactly. Returns NULL if the input is rejected. */
static sds roundTripLen(const char *in, size_t len) {
    int err;
    jsonValue *v = parseLen(in, len, &err);
    if (v == NULL) return NULL;
    sds out = jsonSerialize(sdsempty(), v, NULL, 0);
    jsonFree(v);
    return out;
}

static sds roundTrip(const char *in) {
    return roundTripLen(in, strlen(in));
}

static void expectRoundTrip(const char *in, const char *expected) {
    sds out = roundTrip(in);
    ASSERT_NE(out, nullptr) << "rejected: " << in;
    EXPECT_STREQ(out, expected) << "input: " << in;
    sdsfree(out);
}

static void expectSyntaxError(const char *in, size_t len) {
    int err = JSON_OK;
    jsonValue *v = parseLen(in, len, &err);
    EXPECT_EQ(v, nullptr) << "accepted: " << in;
    EXPECT_EQ(err, JSON_ERR_SYNTAX) << "input: " << in;
    jsonFree(v);
}

static sds serializeFormat(const char *in, const char *indent, const char *newline, const char *space, size_t level) {
    jsonFormat fmt;
    fmt.indent = indent;
    fmt.indent_len = indent ? strlen(indent) : 0;
    fmt.newline = newline;
    fmt.newline_len = newline ? strlen(newline) : 0;
    fmt.space = space;
    fmt.space_len = space ? strlen(space) : 0;
    jsonValue *v = parse(in);
    if (v == NULL) return NULL;
    sds out = jsonSerialize(sdsempty(), v, &fmt, level);
    jsonFree(v);
    return out;
}

TEST_F(JsonTest, NumberTextRoundTrips) {
    const char *same[] = {
        "0", "1", "-1", "42", "1E2", "1e2", "1e+2", "1E-2", "-0.0", "0.0", "1.0", "1.50",
        "0.30000000000000004", "12345678901234567890123", "18446744073709551615",
        "18446744073709551616", "9223372036854775807", "9223372036854775808",
        "-9223372036854775808", "-9223372036854775809", "9e308", "-9e308", "1.7976931348623159e308",
        "0.0e309", "1e00308", "1e-400", "5e-324", "12345678901234567890.5", "0.12345678901234567e309",
        "9007199254740993.1e293", "1e-99999999999", "123.456e1", "0.12345678901234567e325",
        "1.0000000000000000000000001e325", "0.0000000000000000000001e330"};
    for (size_t i = 0; i < sizeof(same) / sizeof(same[0]); i++) expectRoundTrip(same[i], same[i]);

    /* -0 is an integer, and RapidJSON stores it as 0. */
    expectRoundTrip("-0", "0");
    expectRoundTrip("[-0,-0.0]", "[0,-0.0]");
    expectRoundTrip(" \t\r\n3 \n", "3");

    /* A 401 digit integer is accepted and kept verbatim. */
    char big[402];
    big[0] = '1';
    memset(big + 1, '0', 400);
    big[401] = '\0';
    expectRoundTrip(big, big);
}

TEST_F(JsonTest, NumberClassification) {
    jsonValue *v = parse("9223372036854775807");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->type, JSON_INTEGER);
    EXPECT_EQ(v->integer, INT64_MAX);
    jsonFree(v);

    v = parse("-9223372036854775808");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->type, JSON_INTEGER);
    EXPECT_EQ(v->integer, INT64_MIN);
    jsonFree(v);

    const char *numbers[] = {"9223372036854775808", "-9223372036854775809", "1.0", "1e2", "-0.0"};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        v = parse(numbers[i]);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(v->type, JSON_NUMBER) << numbers[i];
        jsonFree(v);
    }

    v = parse("9e308");
    ASSERT_NE(v, nullptr);
    /* Rounds to infinity, as strtod() does. */
    EXPECT_GT(jsonGetDouble(v), DBL_MAX);
    jsonFree(v);

    v = parse("0.1");
    ASSERT_NE(v, nullptr);
    EXPECT_DOUBLE_EQ(jsonGetDouble(v), 0.1);
    jsonFree(v);
}

TEST_F(JsonTest, NumberRejections) {
    /* The first group is RapidJSON's exponent guard, not a syntax problem. */
    const char *bad[] = {
        "1e309", "0e309", "-1e309", "1e0000309", "0.0e310", "-0.0e310", "0.12345678901234567e326", "0.123456789012345678e326",
        "01", "-01", "00", ".5", "1.", "+1", "1e", "1e+", "1e-", "-", "NaN", "Infinity", "-Infinity",
        "0x10", "1_000", "--1", "1 2", "[1.]", "[1,]", "{\"a\":1,}", ""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) expectSyntaxError(bad[i], strlen(bad[i]));
}

TEST_F(JsonTest, FormatDouble) {
    /* Expected values are valkey-json's JSON.RESP replies. */
    struct {
        const char *in;
        const char *out;
    } cases[] = {
        {"1E2", "100.0"},
        {"9e308", "1.797693134862316e308"},
        {"-9e308", "-1.797693134862316e308"},
        {"0.0e309", "0.0"},
        {"-0.0", "-0.0"},
        {"1e-400", "0.0"},
        {"0.1e1", "1.0"},
        {"1.5", "1.5"},
        {"18446744073709551615", "18446744073709552000.0"},
        {"9223372036854775808", "9223372036854776000.0"},
        {"12345678901234567890123", "1.2345678901234568e22"},
        {"123456789012345678901", "123456789012345680000.0"},
        {"1e21", "1e21"},
        {"1e22", "1e22"},
        {"0.000001", "0.000001"},
        {"0.0000001", "1e-7"},
        {"1.7976931348623159e308", "1.797693134862316e308"},
        {"123456789012345678e290", "1.2345678901234567e307"},
        {"0.00000000000000000000000000000000000000000000000000001e340", "1e287"},
        {"9007199254740993.1e292", "9.007199254740992e307"},
        {"0.30000000000000004", "0.30000000000000004"},
        {"5e-324", "5e-324"},
        {"-2.5e-3", "-0.0025"},
        {"123.456e1", "1234.56"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        jsonValue *v = parse(cases[i].in);
        ASSERT_NE(v, nullptr) << cases[i].in;
        char buf[JSON_DOUBLE_BUFSIZE];
        int len = jsonFormatDouble(jsonGetDouble(v), buf);
        buf[len] = '\0';
        EXPECT_STREQ(buf, cases[i].out) << cases[i].in;
        jsonFree(v);
    }
}

TEST_F(JsonTest, StringEscapes) {
    /* Escaped non-ASCII becomes raw UTF-8, '/' is never escaped, control
     * characters use RapidJSON's short escapes or uppercase \u00XX. */
    expectRoundTrip("\"\\u001f/\\/\\u00e9\\uD83D\\uDE00\"", "\"\\u001F//\xc3\xa9\xf0\x9f\x98\x80\"");
    expectRoundTrip("\"\\uDBFF\\uDFFF\"", "\"\xf4\x8f\xbf\xbf\"");
    expectRoundTrip("\"\\b\\f\\n\\r\\t\\\"\\\\\"", "\"\\b\\f\\n\\r\\t\\\"\\\\\"");
    expectRoundTrip("\"\\u0001\\u000b\\u001F\\u007f\\u0080\\uFFFF\"", "\"\\u0001\\u000B\\u001F\x7f\xc2\x80\xef\xbf\xbf\"");
    expectRoundTrip("\"\\u0000\"", "\"\\u0000\"");
    expectRoundTrip("{\"k\\n\":\"\\u0041\"}", "{\"k\\n\":\"A\"}");

    /* Raw bytes >= 0x80, valid UTF-8 or not, pass through untouched. */
    expectRoundTrip("\"\x7f\x80\xff\xc3\xa9\"", "\"\x7f\x80\xff\xc3\xa9\"");

    /* An escape at every offset around the eight byte scan boundary. */
    for (int pos = 0; pos < 20; pos++) {
        char in[64], expected[64];
        memset(in, 'a', sizeof(in));
        in[0] = '"';
        memcpy(in + 1 + pos, "\\n", 2);
        in[1 + 24] = '"';
        in[1 + 24 + 1] = '\0';
        memcpy(expected, in, sizeof(in));
        expectRoundTrip(in, expected);
    }

    const char *bad[] = {"\"\\ud800\"", "\"\\udc00\"", "\"\\udc00x\"", "\"\\ud83d\\u0041\"", "\"\\ud83d\"",
                         "\"\\ud83dx\"", "\"\\u12\"", "\"\\u12g4\"", "\"\\x\"", "\"\\'\"", "\"tab\there\"",
                         "\"line\nbreak\"", "\"unterminated", "\"\\", "\"\\u"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) expectSyntaxError(bad[i], strlen(bad[i]));
}

TEST_F(JsonTest, CatString) {
    sds s = jsonCatString(sdsnew("x"), "a\"b\x01", 4);
    EXPECT_STREQ(s, "x\"a\\\"b\\u0001\"");
    sdsfree(s);
}

TEST_F(JsonTest, NulAndBom) {
    /* A NUL byte ends the input: trailing bytes after a complete value are
     * ignored, a NUL inside a string is an unterminated string. */
    sds out = roundTripLen("1\0garbage", 9);
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, "1");
    sdsfree(out);
    expectSyntaxError("\"a\0b\"", 5);
    expectSyntaxError("[1\0]", 4);

    /* Each BOM byte is skipped on its own, only at the very start. */
    expectRoundTrip("\xef\xbb\xbf"
                    "1",
                    "1");
    expectRoundTrip("\xbb"
                    "1",
                    "1");
    expectRoundTrip("\xef"
                    "1",
                    "1");
    expectRoundTrip("\xef\xbf"
                    "1",
                    "1");
    expectSyntaxError("\xbf\xbf"
                      "1",
                      3);
    expectSyntaxError(" \xef\xbb\xbf"
                      "1",
                      5);
    expectSyntaxError("\x0b"
                      "1",
                      2);
    expectSyntaxError("\x0c"
                      "1",
                      2);
    expectSyntaxError("\xef\xbb\xbf", 3);
}

TEST_F(JsonTest, Literals) {
    expectRoundTrip("[ null , true,false ]", "[null,true,false]");
    const char *bad[] = {"nul", "tru", "fals", "True", "NULL", "nullx", "[1]x", "{}{}", "{\"a\" 1}", "{1:1}", "{'a':1}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) expectSyntaxError(bad[i], strlen(bad[i]));
}

TEST_F(JsonTest, FormattingOptions) {
    /* Expected values are valkey-json's JSON.GET INDENT \t NEWLINE \n SPACE " ". */
    sds out = serializeFormat("{\"a\":3,\"b\":2}", "\t", "\n", " ", 0);
    EXPECT_STREQ(out, "{\n\t\"a\": 3,\n\t\"b\": 2\n}");
    sdsfree(out);

    out = serializeFormat("[1, 2 ,{\"x\" : [ ] , \"y\":{}}]", "\t", "\n", " ", 0);
    EXPECT_STREQ(out, "[\n\t1,\n\t2,\n\t{\n\t\t\"x\": [],\n\t\t\"y\": {}\n\t}\n]");
    sdsfree(out);

    out = serializeFormat("{\"a\":[1,{\"b\":null}],\"c\":{}}", "\t", "\n", " ", 0);
    EXPECT_STREQ(out, "{\n\t\"a\": [\n\t\t1,\n\t\t{\n\t\t\t\"b\": null\n\t\t}\n\t],\n\t\"c\": {}\n}");
    sdsfree(out);

    /* SPACE only follows ':'; arrays never get it. */
    out = serializeFormat("{\"a\":[1,2]}", NULL, NULL, "  ", 0);
    EXPECT_STREQ(out, "{\"a\":  [1,2]}");
    sdsfree(out);

    out = serializeFormat("{\"a\":[1,2]}", "--", NULL, NULL, 0);
    EXPECT_STREQ(out, "{--\"a\":[----1,----2--]}");
    sdsfree(out);

    out = serializeFormat("{\"a\":[1]}", NULL, "\r\n", NULL, 0);
    EXPECT_STREQ(out, "{\r\n\"a\":[\r\n1\r\n]\r\n}");
    sdsfree(out);

    /* A starting level indents everything but the first bracket. */
    out = serializeFormat("[1,[2]]", " ", "\n", NULL, 1);
    EXPECT_STREQ(out, "[\n  1,\n  [\n   2\n  ]\n ]");
    sdsfree(out);

    /* Scalars and empty containers are unaffected, as is everything when the
     * three strings are empty. */
    out = serializeFormat("\"s\\n\"", "\t", "\n", " ", 0);
    EXPECT_STREQ(out, "\"s\\n\"");
    sdsfree(out);
    out = serializeFormat("{\"a\":[],\"b\":{}}", "", "", "", 0);
    EXPECT_STREQ(out, "{\"a\":[],\"b\":{}}");
    sdsfree(out);

    /* Output is appended to what s already holds. */
    jsonValue *v = parse("[1]");
    out = jsonSerialize(sdsnew("prefix"), v, NULL, 0);
    EXPECT_STREQ(out, "prefix[1]");
    sdsfree(out);
    jsonFree(v);
}

static sds nested(int depth, const char *open, const char *close) {
    sds s = sdsempty();
    for (int i = 0; i < depth; i++) s = sdscat(s, open);
    for (int i = 0; i < depth; i++) s = sdscat(s, close);
    return s;
}

TEST_F(JsonTest, NestingLimit) {
    int err;
    size_t depth;
    sds s = nested(128, "[", "]");
    jsonValue *v = jsonParse(s, sdslen(s), JSON_DEFAULT_MAX_DEPTH, &err, &depth);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(err, JSON_OK);
    EXPECT_EQ(depth, 128u);
    EXPECT_EQ(jsonDepth(v), 128u);
    jsonFree(v);
    sdsfree(s);

    s = nested(129, "{\"a\":", "}");
    v = jsonParse(s, sdslen(s), JSON_DEFAULT_MAX_DEPTH, &err, &depth);
    EXPECT_EQ(v, nullptr);
    EXPECT_EQ(err, JSON_ERR_DEPTH);
    EXPECT_STREQ(jsonErrorMessage(err), "LIMIT Document path nesting limit is exceeded");
    sdsfree(s);

    /* The limit is a parameter; scalars have depth 0. */
    v = jsonParse("[[[1]]]", 7, 2, &err, &depth);
    EXPECT_EQ(v, nullptr);
    EXPECT_EQ(err, JSON_ERR_DEPTH);
    v = jsonParse("[[{}],[1]]", 10, 3, &err, &depth);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(depth, 3u);
    jsonFree(v);
    v = jsonParse("7", 1, 0, &err, &depth);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(depth, 0u);
    jsonFree(v);

    /* Errors are reported in input order, as RapidJSON finds them. */
    v = jsonParse("[[x", 3, 2, &err, &depth);
    EXPECT_EQ(err, JSON_ERR_SYNTAX);
    v = jsonParse("[[[x", 4, 2, &err, &depth);
    EXPECT_EQ(err, JSON_ERR_DEPTH);
    EXPECT_STREQ(jsonErrorMessage(JSON_ERR_SYNTAX), "SYNTAXERR Failed to parse JSON string due to syntax error");
}

TEST_F(JsonTest, DuplicateKeys) {
    /* The first position wins, the last value wins. */
    expectRoundTrip("{\"a\":1,\"b\":2,\"a\":3}", "{\"a\":3,\"b\":2}");
    expectRoundTrip("{\"a\":1,\"a\":{\"b\":2},\"c\":3,\"a\":[]}", "{\"a\":[],\"c\":3}");
    expectRoundTrip("{\"a\":{\"x\":1,\"x\":2},\"a\":{\"y\":1}}", "{\"a\":{\"y\":1}}");

    /* Same past the point where the object is indexed. */
    sds in = sdsnew("{");
    sds expected = sdsnew("{");
    for (int i = 0; i < 100; i++) in = sdscatprintf(in, "\"k%d\":%d,", i, i);
    for (int i = 0; i < 100; i += 10) in = sdscatprintf(in, "\"k%d\":\"dup\",", i);
    in = sdscat(in, "\"k0\":\"last\"}");
    for (int i = 0; i < 100; i++) {
        if (i == 0)
            expected = sdscat(expected, "\"k0\":\"last\"");
        else if (i % 10 == 0)
            expected = sdscatprintf(expected, ",\"k%d\":\"dup\"", i);
        else
            expected = sdscatprintf(expected, ",\"k%d\":%d", i, i);
    }
    expected = sdscat(expected, "}");
    expectRoundTrip(in, expected);
    sdsfree(in);
    sdsfree(expected);
}

TEST_F(JsonTest, ObjectOrderAndIndex) {
    jsonValue *obj = jsonCreateObject();
    char name[16];
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof(name), "k%d", 99 - i);
        jsonObjectSet(obj, name, strlen(name), jsonCreateInteger(i));
    }
    EXPECT_EQ(obj->object.len, 100u);
    EXPECT_NE(obj->object.index, nullptr);

    /* Insertion order, not name order. */
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof(name), "k%d", 99 - i);
        EXPECT_STREQ(obj->object.members[i].name, name);
        size_t pos;
        jsonValue *v = jsonObjectFind(obj, name, strlen(name), &pos);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(pos, (size_t)i);
        EXPECT_EQ(v->integer, i);
    }
    EXPECT_EQ(jsonObjectFind(obj, "nope", 4, NULL), nullptr);

    /* Setting an existing member keeps its position. */
    jsonObjectSet(obj, "k50", 3, jsonCreateNull());
    EXPECT_EQ(obj->object.members[49].value->type, JSON_NULL);
    EXPECT_EQ(obj->object.len, 100u);

    /* Delete every third member, then check every lookup still resolves to
     * the right position. */
    for (int i = 0; i < 100; i += 3) {
        snprintf(name, sizeof(name), "k%d", i);
        EXPECT_EQ(jsonObjectDelete(obj, name, strlen(name)), 1);
    }
    EXPECT_EQ(jsonObjectDelete(obj, "k0", 2), 0);
    size_t expect_pos = 0;
    for (int i = 0; i < 100; i++) {
        int k = 99 - i;
        snprintf(name, sizeof(name), "k%d", k);
        size_t pos;
        jsonValue *v = jsonObjectFind(obj, name, strlen(name), &pos);
        if (k % 3 == 0) {
            EXPECT_EQ(v, nullptr) << name;
            continue;
        }
        ASSERT_NE(v, nullptr) << name;
        EXPECT_EQ(pos, expect_pos) << name;
        EXPECT_STREQ(obj->object.members[pos].name, name);
        expect_pos++;
    }
    EXPECT_EQ(obj->object.len, expect_pos);

    /* A re-added member goes to the end. */
    jsonObjectSet(obj, "k3", 2, jsonCreateBool(1));
    EXPECT_STREQ(obj->object.members[obj->object.len - 1].name, "k3");
    size_t pos;
    EXPECT_NE(jsonObjectFind(obj, "k3", 2, &pos), nullptr);
    EXPECT_EQ(pos, obj->object.len - 1);

    /* Names are binary safe. */
    jsonObjectSet(obj, "a\0b", 3, jsonCreateInteger(1));
    EXPECT_EQ(jsonObjectFind(obj, "a", 1, NULL), nullptr);
    EXPECT_NE(jsonObjectFind(obj, "a\0b", 3, NULL), nullptr);

    /* Delete down to empty through the index. */
    while (obj->object.len) jsonObjectDeleteAt(obj, obj->object.len / 2);
    EXPECT_EQ(jsonObjectFind(obj, "k3", 2, NULL), nullptr);
    jsonFree(obj);
}

TEST_F(JsonTest, SmallObjectMutation) {
    jsonValue *obj = parse("{\"a\":1,\"b\":2,\"c\":3}");
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->object.index, nullptr);
    EXPECT_EQ(jsonObjectDelete(obj, "b", 1), 1);
    jsonObjectSet(obj, "d", 1, jsonCreateString("x", 1));
    jsonObjectSet(obj, "a", 1, jsonCreateArray());
    sds out = jsonSerialize(sdsempty(), obj, NULL, 0);
    EXPECT_STREQ(out, "{\"a\":[],\"c\":3,\"d\":\"x\"}");
    sdsfree(out);
    jsonFree(obj);
}

TEST_F(JsonTest, ArrayMutation) {
    jsonValue *arr = jsonCreateArray();
    for (int i = 0; i < 10; i++) jsonArrayAppend(arr, jsonCreateInteger(i));
    jsonArrayInsert(arr, 0, jsonCreateString("first", 5));
    jsonArrayInsert(arr, 5, jsonCreateNull());
    jsonArrayInsert(arr, arr->array.len, jsonCreateBool(0));
    jsonArraySet(arr, 1, jsonCreateNumber("1.0", 3));

    sds out = jsonSerialize(sdsempty(), arr, NULL, 0);
    EXPECT_STREQ(out, "[\"first\",1.0,1,2,3,null,4,5,6,7,8,9,false]");
    sdsfree(out);

    jsonValue *v = jsonArrayDetach(arr, 0);
    EXPECT_EQ(v->type, JSON_STRING);
    EXPECT_STREQ(v->string, "first");
    jsonFree(v);
    jsonArrayDeleteRange(arr, 3, 5);
    jsonArrayDeleteRange(arr, 0, 0);
    EXPECT_EQ(jsonChildCount(arr), 7u);
    EXPECT_EQ(jsonChildAt(arr, 3)->integer, 7);

    out = jsonSerialize(sdsempty(), arr, NULL, 0);
    EXPECT_STREQ(out, "[1.0,1,2,7,8,9,false]");
    sdsfree(out);

    jsonArrayDeleteRange(arr, 0, arr->array.len);
    out = jsonSerialize(sdsempty(), arr, NULL, 0);
    EXPECT_STREQ(out, "[]");
    sdsfree(out);
    jsonFree(arr);
}

TEST_F(JsonTest, DupReplaceAndMemory) {
    const char *doc = "{\"a\":[1,-0.0,\"s\",{\"b\":null,\"c\":[true,false]}],\"d\":{},\"e\":1E2}";
    jsonValue *v = parse(doc);
    ASSERT_NE(v, nullptr);
    jsonValue *d = jsonDup(v);
    sds a = jsonSerialize(sdsempty(), v, NULL, 0);
    sds b = jsonSerialize(sdsempty(), d, NULL, 0);
    EXPECT_STREQ(a, b);
    EXPECT_EQ(jsonMemoryUsage(v), jsonMemoryUsage(d));
    EXPECT_GT(jsonMemoryUsage(v), strlen(doc));
    sdsfree(a);
    sdsfree(b);

    /* The dup is independent of the original. */
    jsonObjectDelete(v, "a", 1);
    EXPECT_NE(jsonObjectFind(d, "a", 1, NULL), nullptr);

    /* Replace keeps the destination's address. */
    jsonValue *e = jsonObjectFind(d, "e", 1, NULL);
    jsonReplace(e, jsonCreateString("new", 3));
    EXPECT_EQ(jsonObjectFind(d, "e", 1, NULL), e);
    EXPECT_EQ(e->type, JSON_STRING);
    jsonFree(v);
    jsonFree(d);

    /* Dup of an indexed object keeps its lookups working. */
    sds in = sdsnew("{");
    for (int i = 0; i < 40; i++) in = sdscatprintf(in, "%s\"m%d\":%d", i ? "," : "", i, i);
    in = sdscat(in, "}");
    v = parse(in);
    ASSERT_NE(v, nullptr);
    d = jsonDup(v);
    jsonFree(v);
    jsonValue *m = jsonObjectFind(d, "m39", 3, NULL);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->integer, 39);
    jsonFree(d);
    sdsfree(in);
}

TEST_F(JsonTest, ParseFailureFreesPartialTree) {
    /* Each fails after building nested values, member names and strings. */
    const char *bad[] = {"{\"a\":[1,\"x\",{\"b\":\"y\",\"c\":[1.5,2]}],\"d\":\"z\",\"e\":", "{\"a\":[1,2],\"b\"",
                         "[{\"a\":\"\\u00e9\"},[\"x\",", "{\"a\":{\"b\":{\"c\":1}}}}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) expectSyntaxError(bad[i], strlen(bad[i]));

    int err;
    size_t depth;
    sds s = sdsnew("[\"a\",{\"k\":\"v\",\"n\":");
    for (int i = 0; i < 200; i++) s = sdscat(s, "[\"x\",");
    jsonValue *v = jsonParse(s, sdslen(s), JSON_DEFAULT_MAX_DEPTH, &err, &depth);
    EXPECT_EQ(v, nullptr);
    EXPECT_EQ(err, JSON_ERR_DEPTH);
    sdsfree(s);
}

TEST_F(JsonTest, LargeDocumentRoundTrip) {
    /* Enough output to outgrow any small initial buffer, with escapes that
     * force the serializer past its length estimate. */
    sds in = sdsnew("[");
    for (int i = 0; i < 2000; i++) {
        in = sdscatprintf(in, "%s{\"id\":%d,\"name\":\"n\\\"%d\\u0001\",\"v\":%d.5,\"tags\":[\"a\",\"b\"]}", i ? "," : "",
                          i, i, i);
    }
    in = sdscat(in, "]");
    sds out = roundTrip(in);
    ASSERT_NE(out, nullptr);
    EXPECT_STREQ(out, in);
    sdsfree(out);

    out = serializeFormat(in, "  ", "\n", " ", 0);
    ASSERT_NE(out, nullptr);
    const char *head = "[\n  {\n    \"id\": 0,\n    \"name\": \"n\\\"0\\u0001\",";
    EXPECT_EQ(strncmp(out, head, strlen(head)), 0);
    sds back = roundTrip(out);
    EXPECT_STREQ(back, in);
    sdsfree(back);
    sdsfree(out);
    sdsfree(in);
}
