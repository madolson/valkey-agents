proc json_array {n} {
    set items {}
    for {set i 0} {$i < $n} {incr i} {
        lappend items $i
    }
    return "\[[join $items ,]\]"
}

start_server {tags {json needs:debug}} {
    test {JSON key reports the module type name and json encoding} {
        r del doc
        assert_equal OK [r json.set doc . {{"a":[1,2,{"b":null}],"c":"x"}}]
        assert_equal ReJSON-RL [r type doc]
        assert_equal json [r object encoding doc]
        assert_match {*encoding:json*} [r debug object doc fast]
    }

    test {JSON.SET rejects invalid JSON} {
        assert_error {*SYNTAXERR*} {r json.set doc . {{"a":}}}
        assert_equal ReJSON-RL [r type doc]
    }

    test {Commands for other types reply WRONGTYPE on a JSON key} {
        r json.set doc . {[1]}
        assert_error {WRONGTYPE*} {r get doc}
        assert_error {WRONGTYPE*} {r lpush doc x}
    }

    test {DEL and EXISTS on a JSON key} {
        r json.set doc . {{"a":1}}
        assert_equal 1 [r exists doc]
        assert_equal 1 [r del doc]
        assert_equal 0 [r exists doc]
    }

    test {DEBUG DIGEST-VALUE depends on JSON content and member order} {
        r json.set d1 . {{"a":1,"b":[true,false]}}
        r json.set d2 . {{"a":1,"b":[true,false]}}
        r json.set d3 . {{"b":[true,false],"a":1}}
        r json.set d4 . {{"a":1.0,"b":[true,false]}}
        set digests [r debug digest-value d1 d2 d3 d4]
        assert_equal [lindex $digests 0] [lindex $digests 1]
        assert_not_equal [lindex $digests 0] [lindex $digests 2]
        assert_not_equal [lindex $digests 0] [lindex $digests 3]
        assert_not_equal [r debug digest] 0000000000000000000000000000000000000000
    }

    test {COPY duplicates a JSON key} {
        r flushall
        r json.set src . {{"a":[1,2,3],"b":{"c":"d"},"n":1E2}}
        assert_equal 1 [r copy src dst]
        assert_equal ReJSON-RL [r type dst]
        assert_equal json [r object encoding dst]
        set digest [r debug digest-value src]
        assert_equal $digest [r debug digest-value dst]

        # The copy is independent of the source.
        r json.set src . {"other"}
        assert_equal $digest [r debug digest-value dst]
        r del src
        assert_equal $digest [r debug digest-value dst]
    }

    test {RENAME keeps the JSON value} {
        r flushall
        r json.set a . {[1,"two",{"three":3}]}
        set digest [r debug digest-value a]
        r rename a b
        assert_equal 0 [r exists a]
        assert_equal ReJSON-RL [r type b]
        assert_equal $digest [r debug digest-value b]
    }

    test {EXPIRE applies to a JSON key} {
        r flushall
        r json.set doc . {{"a":1}}
        assert_equal 1 [r expire doc 100]
        assert_range [r ttl doc] 90 100
        assert_equal 1 [r pexpire doc 10]
        wait_for_condition 50 20 {
            [r exists doc] == 0
        } else {
            fail "JSON key did not expire"
        }
    }

    test {MEMORY USAGE grows with the JSON document} {
        r flushall
        r json.set small . {1}
        r json.set big . [json_array 1000]
        set small [r memory usage small]
        set big [r memory usage big]
        assert_morethan $small 0
        assert_morethan $big [expr {$small + 1000 * 8}]
    }

    test {SCAN TYPE filters JSON keys by their TYPE name} {
        r flushall
        r json.set j1 . {{}}
        r json.set j2 . {[]}
        r set s1 v
        assert_equal {j1 j2} [lsort [lindex [r scan 0 type ReJSON-RL count 100] 1]]
        assert_equal {j1 j2} [lsort [lindex [r scan 0 type rejson-rl count 100] 1]]
        assert_equal {s1} [lindex [r scan 0 type string count 100] 1]
    }

    test {UNLINK frees large JSON documents lazily and small ones inline} {
        r flushall
        wait_lazyfree_done r
        r config resetstat
        # An array of n elements is n + 1 values; lazy free starts above 64.
        r json.set small . [json_array 63]
        r unlink small
        wait_lazyfree_done r
        assert_equal 0 [s lazyfreed_objects]

        r json.set big . [json_array 64]
        r unlink big
        wait_lazyfree_done r
        assert_equal 1 [s lazyfreed_objects]

        # Values are counted at every depth, not just the top level.
        r json.set nested . "{\"a\":[json_array 100]}"
        r unlink nested
        wait_lazyfree_done r
        assert_equal 2 [s lazyfreed_objects]
    } {} {needs:config-resetstat}

    test {FLUSHALL ASYNC frees JSON keys} {
        r flushall
        for {set i 0} {$i < 100} {incr i} {
            r json.set doc:$i . [json_array 100]
        }
        assert_equal 100 [r dbsize]
        r flushall async
        assert_equal 0 [r dbsize]
        wait_lazyfree_done r
    }

    test {Keyspace event class j and ACL category json exist} {
        set orig [lindex [r config get notify-keyspace-events] 1]
        r config set notify-keyspace-events Kj
        assert_equal jK [lindex [r config get notify-keyspace-events] 1]
        r config set notify-keyspace-events KA
        assert_equal AK [lindex [r config get notify-keyspace-events] 1]
        r config set notify-keyspace-events $orig
        assert_not_equal -1 [lsearch -exact [r acl cat] json]
    }
}

start_server {tags {json}} {
    test {JSON.SET and JSON.GET at the root and at paths} {
        r flushall
        assert_equal OK [r json.set k . {{"a":1,"b":[1,2],"c":{"d":true}}}]
        assert_equal {{"a":1,"b":[1,2],"c":{"d":true}}} [r json.get k]
        assert_equal OK [r json.set k .a {"x"}]
        assert_equal OK [r json.set k {$.c.e} {[]}]
        assert_equal {{"a":"x","b":[1,2],"c":{"d":true,"e":[]}}} [r json.get k]
        assert_equal {[1,2]} [r json.get k .b]
        assert_equal {[[1,2]]} [r json.get k {$.b}]
        assert_equal {[]} [r json.get k {$.missing}]
        assert_error {NONEXISTENT JSON path does not exist} {r json.get k .missing}
        assert_equal {} [r json.get nokey]
        assert_equal ReJSON-RL [r type k]
    }

    test {JSON.SET on a new key needs the root path} {
        r flushall
        assert_error {SYNTAXERR A new Valkey key's path must be root} {r json.set k .a 1}
        assert_equal 0 [r exists k]
        assert_equal OK [r json.set k {$} 1]
        assert_equal 1 [r json.get k]
    }

    test {JSON.SET errors} {
        r flushall
        r set s v
        assert_error {WRONGTYPE Not a JSON document key} {r json.set s . 1}
        assert_error {SYNTAXERR Command syntax error} {r json.set k . 1 YY}
        assert_error {*wrong number of arguments*} {r json.set k . 1 NX XX}
        assert_error {SYNTAXERR Failed to parse JSON string due to syntax error} {r json.set k . {{"a":}}}
        r json.set k . {{"a":1}}
        assert_error {NONEXISTENT JSON path does not exist} {r json.set k .x.y 1}
        assert_error {ERROR Cannot insert a member into a non-object value} {r json.set k {$.a.b} 1}
        assert_error {SYNTAXERR*} {r json.set k {.a[} 1}
    }

    test {JSON.SET through a wildcard that matches nothing is a no-op} {
        r flushall
        r json.set k . {{"a":{}}}
        assert_equal OK [r json.set k {$..nothing.x} 1]
        assert_equal OK [r json.set k {$.a.*} 1]
        assert_equal {{"a":{}}} [r json.get k]
        assert_equal OK [r json.set k {$.*.x} 1]
        assert_equal {{"a":{"x":1}}} [r json.get k]
    }

    test {JSON.SET NX and XX} {
        r flushall
        assert_equal {} [r json.set k . 1 XX]
        assert_equal 0 [r exists k]
        assert_equal OK [r json.set k . {{"a":1}} NX]
        assert_equal {} [r json.set k . 2 NX]
        assert_equal {} [r json.set k .a 2 NX]
        assert_equal {} [r json.set k .b 2 XX]
        assert_equal OK [r json.set k .a 2 xx]
        assert_equal OK [r json.set k .b 3 nx]
        assert_equal {{"a":2,"b":3}} [r json.get k]
        # NX and XX are checked before the value is parsed.
        assert_equal {} [r json.set k .a {not json} NX]
    }

    test {JSON.SET at the root drops the TTL, at a path keeps it} {
        r flushall
        r json.set k . {{"a":1}}
        r expire k 100
        r json.set k .a 2
        assert_range [r ttl k] 90 100
        r json.set k {$} 3
        assert_equal -1 [r ttl k]
    }

    test {JSON.SET enforces json.max-path-limit} {
        r flushall
        r config set json.max-path-limit 3
        assert_equal OK [r json.set k . {[[[1]]]}]
        assert_error {LIMIT Document path nesting limit is exceeded} {r json.set k . {[[[[1]]]]}}
        r json.set k . {{"a":{"b":1}}}
        assert_equal OK [r json.set k .a.b {[]}]
        assert_error {LIMIT Document path nesting limit is exceeded} {r json.set k .a.b {[[]]}}
        r config set json.max-path-limit 128
        assert_equal OK [r json.set k . "[string repeat {[} 128][string repeat {]} 128]"]
        assert_error {LIMIT*} {r json.set k . "[string repeat {[} 129][string repeat {]} 129]"}
    }

    test {JSON.SET enforces json.max-document-size} {
        r flushall
        r config set json.max-document-size 1000
        r json.set k . {{"a":[]}}
        assert_error {LIMIT Document size limit is exceeded} {r json.set k . "\[[string repeat {"abcdefghij",} 200]0\]"}
        assert_error {LIMIT Document size limit is exceeded} {r json.set k .a "\[[string repeat {"abcdefghij",} 200]0\]"}
        assert_error {LIMIT Document size limit is exceeded} {r json.arrappend k .a "\"[string repeat x 2000]\""}
        r json.set k .s {""}
        assert_error {LIMIT Document size limit is exceeded} {r json.strappend k .s "\"[string repeat x 2000]\""}
        assert_equal {{"a":[],"s":""}} [r json.get k]
        r config set json.max-document-size 0
        assert_equal OK [r json.set k .a "\[[string repeat {"abcdefghij",} 200]0\]"]
    }

    test {JSON.GET formatting options} {
        r flushall
        r json.set k . {{"a":1,"b":[1,2],"c":{}}}
        assert_equal "{\n  \"a\": 1,\n  \"b\": \[\n    1,\n    2\n  \],\n  \"c\": {}\n}" \
            [r json.get k INDENT "  " NEWLINE "\n" SPACE " "]
        assert_equal "\[\n\t\[\n\t\t1,\n\t\t2\n\t\]\n\]" [r json.get k indent "\t" newline "\n" {$.b}]
        assert_equal {{"a":1,"b":[1,2],"c":{}}} [r json.get k NOESCAPE]
        assert_error {SYNTAXERR Command syntax error} {r json.get k INDENT}
    }

    test {JSON.GET with several paths} {
        r flushall
        r json.set k . {{"a":1,"b":[1,2]}}
        assert_equal {{".a":1,".b":[1,2]}} [r json.get k .a .b]
        assert_error {NONEXISTENT JSON path does not exist} {r json.get k .a .missing}
        # One JSONPath makes every path JSONPath.
        assert_equal {{".a":[1],"$.b":[[1,2]],".missing":[]}} [r json.get k .a {$.b} .missing]
        assert_equal "{\n\t\".a\":\[\n\t\t1\n\t\],\n\t\"\$.b\":\[\n\t\t\[\n\t\t\t1,\n\t\t\t2\n\t\t\]\n\t\]\n}" \
            [r json.get k INDENT "\t" NEWLINE "\n" .a {$.b}]
    }

    test {JSON.MGET} {
        r flushall
        r json.set a . {{"x":1}}
        r json.set b . {{"x":[2]}}
        r json.set c . {{"y":3}}
        assert_equal {1 {[2]} {} {}} [r json.mget a b c nokey .x]
        assert_equal {{[1]} {[[2]]} {[]} {}} [r json.mget a b c nokey {$.x}]
        assert_error {SYNTAXERR Expression token cannot be empty} {r json.mget a b c nokey {.x[}}
        r set s v
        assert_error {WRONGTYPE Not a JSON document key} {r json.mget a s .x}
    }

    test {JSON.MSET applies each triple on a best-effort basis} {
        r flushall
        r json.set a . {{"x":1}}
        assert_equal OK [r json.mset a .x 2 b . {[]} a .y {"z"}]
        assert_equal {{"x":2,"y":"z"}} [r json.get a]
        assert_equal {[]} [r json.get b]
        # Validated against the keyspace before anything is applied.
        assert_error {SYNTAXERR Command syntax error} {r json.mset a .x 3 new .x 1}
        assert_error {NONEXISTENT JSON path does not exist} {r json.mset a .x 3 a .q.r 1}
        assert_error {SYNTAXERR Failed to parse*} {r json.mset a .x 3 b . {[}}
        assert_equal 2 [r json.get a .x]
        # A triple that no longer applies after an earlier one is skipped.
        assert_equal OK [r json.mset a . 5 a .x 6]
        assert_equal 5 [r json.get a]
        assert_error {*wrong number of arguments*} {r json.mset a .x 1 b}
    }

    test {JSON.DEL and JSON.FORGET} {
        r flushall
        r json.set k . {{"a":1,"b":{"a":2},"c":[1,2,3]}}
        assert_equal 0 [r json.del nokey]
        assert_equal 0 [r json.del k .missing]
        assert_equal 0 [r json.del k {$.missing}]
        assert_equal 0 [r json.del k {}]
        assert_equal 2 [r json.del k {$..a}]
        assert_equal {{"b":{},"c":[1,2,3]}} [r json.get k]
        assert_equal 2 [r json.forget k {$.c[0,2]}]
        assert_equal {{"b":{},"c":[2]}} [r json.get k]
        assert_error {ERROR Cannot insert a member into a non-object value} {r json.del k {.c.x}}
        assert_equal 1 [r json.del k]
        assert_equal 0 [r exists k]
        r json.set k . 1
        assert_equal 1 [r json.forget k {$}]
        assert_equal 0 [r exists k]
    }

    test {JSON.TYPE} {
        r flushall
        r json.set k . {{"n":null,"t":true,"s":"x","i":1,"d":1.0,"u":18446744073709551615,"o":{},"a":[]}}
        assert_equal object [r json.type k]
        assert_equal {null boolean string integer number number object array} [r json.type k {$.*}]
        assert_equal integer [r json.type k .i]
        assert_equal {} [r json.type k .missing]
        assert_equal {} [r json.type k {.i.*}]
        assert_equal {} [r json.type k {$.missing}]
        assert_equal {} [r json.type nokey]
    }

    test {JSON.NUMINCRBY and JSON.NUMMULTBY result types follow the operands} {
        r flushall
        r json.set k . {{"i":10,"d":1.0,"s":"x"}}
        assert_equal 15 [r json.numincrby k .i 5.0]
        assert_equal number [r json.type k .i]
        assert_equal 3 [r json.nummultby k .d 3]
        assert_equal integer [r json.type k .d]
        assert_equal 16 [r json.numincrby k .i 1]
        assert_equal {[17,4,null]} [r json.numincrby k {$.*} 1]
        assert_equal {[2]} [r json.nummultby k {$.d} 0.5]
        assert_equal {[null]} [r json.numincrby k {$.s} 1]
        assert_equal {[]} [r json.numincrby k {$.missing} 1]
        assert_equal {{"i":17,"d":2,"s":"x"}} [r json.get k]
        r json.set k .d 0.1
        assert_equal 0.30000000000000004 [r json.numincrby k .d 0.2]
    }

    test {JSON.NUMINCRBY and JSON.NUMMULTBY errors} {
        r flushall
        r json.set k . {{"i":9223372036854775807,"d":1e308,"s":"x"}}
        assert_error {WRONGTYPE Value is not a number} {r json.numincrby k .i x}
        assert_error {WRONGTYPE Value is not a number} {r json.numincrby k .i {"1"}}
        assert_error {WRONGTYPE JSON element is not a number} {r json.numincrby k .s 1}
        assert_error {NONEXISTENT JSON path does not exist} {r json.numincrby k .missing 1}
        assert_error {NONEXISTENT Document key does not exist} {r json.numincrby nokey . 1}
        # Integer overflow falls back to doubles.
        assert_equal 9.2233720368547758e+18 [r json.numincrby k .i 1]
        assert_equal number [r json.type k .i]
        assert_error {OVERFLOW Addition would overflow} {r json.numincrby k .d 1e308}
        assert_error {OVERFLOW Multiplication would overflow} {r json.nummultby k .d 10}
        assert_equal 1e308 [r json.get k .d]
    }

    test {JSON.NUMMULTBY refuses a NaN result} {
        r flushall
        r json.set k . {[9e308]}
        assert_error {OVERFLOW Multiplication would overflow} {r json.nummultby k {[0]} 0}
        assert_error {OVERFLOW Multiplication would overflow} {r json.nummultby k {$[0]} 0}
        r json.set k . {[-9e308]}
        assert_error {OVERFLOW Addition would overflow} {r json.numincrby k {$[0]} 9e308}
        assert_equal {[-9e308]} [r json.get k]
    }

    test {JSON.STRLEN and JSON.STRAPPEND} {
        r flushall
        r json.set k . {{"a":"ab","b":{"a":"x"},"n":1}}
        assert_equal 2 [r json.strlen k .a]
        assert_equal {2 1} [r json.strlen k {$..a}]
        assert_equal 4 [r json.strappend k .a {"cd"}]
        assert_equal {5 2} [r json.strappend k {$..a} {"!"}]
        # Legacy paths reply with the last string changed.
        assert_equal 3 [r json.strappend k ..a {"?"}]
        assert_equal {{"a":"abcd!?","b":{"a":"x!?"},"n":1}} [r json.get k]
        assert_equal {7 {} {}} [r json.strappend k {$.*} {"."}]
        assert_error {WRONGTYPE JSON element is not a string} {r json.strlen k .n}
        assert_error {WRONGTYPE JSON element is not a string} {r json.strappend k .n {"x"}}
        assert_error {WRONGTYPE Value is not a string} {r json.strappend k .a 1}
        assert_error {SYNTAXERR*} {r json.strappend k .a x}
        assert_equal {} [r json.strlen nokey]
        r json.set s . {"x"}
        assert_equal 3 [r json.strappend s {"yz"}]
        assert_equal {"xyz"} [r json.get s]
    }

    test {JSON.STRAPPEND cuts both strings at a NUL character} {
        r flushall
        r json.set s . {"a\u0000b"}
        assert_equal 3 [r json.strlen s]
        assert_equal 2 [r json.strappend s {"c\u0000d"}]
        assert_equal {"ac"} [r json.get s]
    }

    test {JSON.TOGGLE replies JSON under a legacy path and integers under JSONPath} {
        r flushall
        r json.set k . {{"a":true,"b":{"a":false},"n":1}}
        assert_equal false [r json.toggle k .a]
        assert_equal {1 1} [r json.toggle k {$..a}]
        assert_equal {0 {} {}} [r json.toggle k {$.*}]
        assert_equal {} [r json.toggle k {$.missing}]
        assert_error {WRONGTYPE JSON element is not a bool} {r json.toggle k .n}
        assert_error {NONEXISTENT JSON path does not exist} {r json.toggle k .missing}
        r json.set t . true
        assert_equal false [r json.toggle t]
        assert_equal {{"a":false,"b":{"a":true},"n":1}} [r json.get k]
    }

    test {JSON.CLEAR resets each type} {
        r flushall
        r json.set k . {{"o":{"a":1},"a":[1],"t":true,"f":false,"s":"x","e":"","i":5,"z":0,"d":1.5,"u":18446744073709551615,"m":-0.0,"n":null}}
        assert_equal 7 [r json.clear k {$.*}]
        assert_equal {{"o":{},"a":[],"t":false,"f":false,"s":"","e":"","i":0,"z":0,"d":0.0,"u":0.0,"m":-0.0,"n":null}} [r json.get k]
        assert_equal 0 [r json.clear k {$.*}]
        assert_equal 0 [r json.clear k .missing]
        r json.set k . {{"a":{"b":[1]}}}
        assert_equal 3 [r json.clear k {$..*}]
        assert_equal 1 [r json.clear k]
        assert_equal {{}} [r json.get k]
        assert_error {NONEXISTENT Document key does not exist} {r json.clear nokey}
    }

    test {JSON.ARRLEN, JSON.ARRAPPEND and JSON.ARRINSERT} {
        r flushall
        r json.set k . {{"a":[1],"b":{"a":[]},"n":1}}
        assert_equal 1 [r json.arrlen k .a]
        assert_equal {1 0} [r json.arrlen k {$..a}]
        assert_equal {1 {} {}} [r json.arrlen k {$.*}]
        assert_equal 3 [r json.arrappend k .a 2 {"x"}]
        assert_equal {4 1} [r json.arrappend k {$..a} null]
        assert_equal 6 [r json.arrinsert k .a 0 -1 -2]
        assert_equal 7 [r json.arrinsert k .a -1 {"before last"}]
        assert_equal 8 [r json.arrinsert k .a 7 {"end"}]
        assert_equal {{"a":[-1,-2,1,2,"x","before last",null,"end"],"b":{"a":[null]},"n":1}} [r json.get k]
        assert_error {OUTOFBOUNDARIES Array index is out of bounds} {r json.arrinsert k .a 9 1}
        assert_error {OUTOFBOUNDARIES Array index is out of bounds} {r json.arrinsert k .a -9 1}
        assert_error {WRONGTYPE Value is not an integer} {r json.arrinsert k .a x 1}
        assert_error {WRONGTYPE JSON element is not an array} {r json.arrappend k .n 1}
        assert_error {SYNTAXERR*} {r json.arrappend k .a {[}}
        assert_error {NONEXISTENT Document key does not exist} {r json.arrappend nokey . 1}
        assert_equal {} [r json.arrlen nokey]
    }

    test {JSON.ARRINSERT keeps arrays changed before an out of bounds one} {
        r flushall
        r json.set k . {{"a":[1,[2,3,4]]}}
        # Deepest first: the inner array is changed, then the outer one fails.
        assert_error {OUTOFBOUNDARIES*} {r json.arrinsert k {$..*} 3 0}
        assert_equal {{"a":[1,[2,3,4,0]]}} [r json.get k]
    }

    test {JSON.ARRPOP} {
        r flushall
        r json.set k . {{"a":[1,2,3,4,5],"b":[],"n":1}}
        assert_equal 5 [r json.arrpop k .a]
        assert_equal 1 [r json.arrpop k .a 0]
        assert_equal 4 [r json.arrpop k .a 100]
        assert_equal 2 [r json.arrpop k .a -100]
        assert_equal {3 {} {}} [r json.arrpop k {$.*}]
        assert_equal {{"a":[],"b":[],"n":1}} [r json.get k]
        assert_equal {} [r json.arrpop k .b]
        r json.set k .a {[{"x":1}]}
        assert_equal {{"x":1}} [r json.arrpop k .a]
        assert_error {WRONGTYPE Value is not an integer} {r json.arrpop k .a x}
        assert_error {WRONGTYPE JSON element is not an array} {r json.arrpop k .n}
        r json.set a . {[1,[2,3]]}
        assert_equal {[2,3]} [r json.arrpop a]
    }

    test {JSON.ARRTRIM} {
        r flushall
        r json.set k . {{"a":[0,1,2,3,4,5],"b":[],"n":1}}
        assert_equal 3 [r json.arrtrim k .a 1 3]
        assert_equal {[1,2,3]} [r json.get k .a]
        assert_equal 3 [r json.arrtrim k .a -5 100]
        assert_equal 0 [r json.arrtrim k .a 0 -1]
        assert_equal {0 0 {}} [r json.arrtrim k {$.*} 0 1]
        r json.set k .a {[0,1,2]}
        assert_equal 0 [r json.arrtrim k .a 2 1]
        assert_error {WRONGTYPE Value is not an integer} {r json.arrtrim k .a 0 x}
    }

    test {JSON.ARRINDEX} {
        r flushall
        r json.set k . {{"a":[1,1.0,"1",{"x":[1,{}]},true,null],"b":{"a":[]},"n":1}}
        assert_equal 0 [r json.arrindex k .a 1.0]
        assert_equal 1 [r json.arrindex k .a 1E0 1]
        assert_equal 2 [r json.arrindex k .a {"1"}]
        assert_equal 3 [r json.arrindex k .a {{"x":[1.0,{}]}}]
        assert_equal 5 [r json.arrindex k .a null]
        assert_equal -1 [r json.arrindex k .a true 0 4]
        assert_equal 4 [r json.arrindex k .a true -10 -1]
        assert_equal -1 [r json.arrindex k .a true 0 -2]
        assert_equal {-1 -1} [r json.arrindex k {$..a} false]
        assert_equal {0 {} {}} [r json.arrindex k {$.*} 1]
        assert_error {WRONGTYPE JSON element is not an array} {r json.arrindex k .n 1}
        assert_error {SYNTAXERR*} {r json.arrindex k .a {[}}
        assert_error {WRONGTYPE Value is not an integer} {r json.arrindex k .a 1 x}
        assert_error {NONEXISTENT Document key does not exist} {r json.arrindex nokey . 1}
    }

    test {JSON.OBJLEN and JSON.OBJKEYS} {
        r flushall
        r json.set k . {{"a":{"x":1,"y":2},"b":{},"n":1}}
        assert_equal 3 [r json.objlen k]
        assert_equal 2 [r json.objlen k .a]
        assert_equal {2 0 {}} [r json.objlen k {$.*}]
        assert_equal {a b n} [r json.objkeys k]
        assert_equal {x y} [r json.objkeys k .a]
        assert_equal {{x y} {} {}} [r json.objkeys k {$.*}]
        assert_equal {} [r json.objkeys k .b]
        assert_equal {} [r json.objkeys k .missing]
        assert_equal {} [r json.objkeys nokey]
        assert_error {WRONGTYPE JSON element is not an object} {r json.objkeys k .n}
        assert_error {WRONGTYPE JSON element is not an object} {r json.objlen k .n}
        assert_error {NONEXISTENT JSON path does not exist} {r json.objlen k .missing}
        assert_equal {} [r json.objlen nokey]
    }
}

# RDB length encoding, see rdbSaveLen().
proc json_rdb_len {n} {
    if {$n < 64} {
        return [binary format c $n]
    } elseif {$n < 16384} {
        return [binary format cc [expr {0x40 | ($n >> 8)}] [expr {$n & 0xff}]]
    }
    return [binary format cI 0x80 $n]
}

# DUMP payload footer with a zero CRC, for use with checksum validation off.
proc json_dump_footer {} {
    return "[binary format s 82][string repeat \x00 8]"
}

proc json_native_payload {text} {
    return "[binary format c 24][json_rdb_len [string length $text]]$text[json_dump_footer]"
}

# A valkey-json module value: the ReJSON-RL type id with the encoding version
# in the low 10 bits, then one string and the module value EOF marker.
proc json_module_payload {text encver} {
    set id [expr {0x45e25238df912c00 | $encver}]
    set body "[json_rdb_len 5][json_rdb_len [string length $text]]$text[json_rdb_len 0]"
    return "[binary format c 7][binary format cW 0x81 $id]$body[json_dump_footer]"
}

set ::json_persist_docs [list \
    num:1E2 {1E2} \
    num:1e2 {1e2} \
    num:-0.0 {-0.0} \
    num:-0 {-0} \
    num:0.3 {0.30000000000000004} \
    num:u64max {18446744073709551615} \
    num:i64min {-9223372036854775808} \
    num:big {100000000000000000000000000000000000000000} \
    num:9e308 {9e308} \
    int:small {42} \
    int:neg {-7} \
    int:32bit {2147483648} \
    str:int {"12345"} \
    str:empty {""} \
    str:escapes {"\"q\" \\ \/ \b\f\n\r\t \u0000 \u001f"} \
    str:unicode-escaped {"\u00e9 \u65e5\u672c \ud83d\ude00 \uFFFF"} \
    str:unicode-raw "\"\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x98\x80\"" \
    arr:numbers {[1E2,1e2,-0.0,1.0,0.1,2e22,1e-7,123.456e1,18446744073709551616]} \
    obj:nested {{"a":{"b":[1,{"c":[true,false,null]},"x"]},"d":{}}} \
    obj:order "{[join [lmap i {9 3 7 1 40 0 33 12 5 2 38 11 20 4 6 8 10 13 14 15 16 17 18 19 21 22 23 24 25 26 27 28 29 30 31 32 34 35 36 37} {format {"k%d":%d} $i $i}] ,]}" \
    deep:arrays "[string repeat {[} 128][string repeat {]} 128]" \
    deep:objects "[string repeat "\{\"a\":" 127]0[string repeat "\}" 127]" \
    arr:large [json_array 5000] \
]

proc json_persist_setup {} {
    foreach {key doc} $::json_persist_docs {
        r json.set $key . $doc
    }
}

# Compact serialization of every test key plus the dataset digest.
proc json_persist_state {} {
    set state {}
    foreach {key doc} $::json_persist_docs {
        lappend state $key [r json.get $key] [r debug digest-value $key]
    }
    lappend state [r debug digest]
    return $state
}

start_server {tags {json needs:debug}} {
    test {JSON keys keep exact number and string text across DEBUG RELOAD} {
        r flushall
        json_persist_setup
        set before [json_persist_state]
        assert_equal {1E2} [r json.get num:1E2]
        assert_equal {-0.0} [r json.get num:-0.0]
        assert_equal {0.30000000000000004} [r json.get num:0.3]
        r debug reload
        assert_equal $before [json_persist_state]
        assert_equal ReJSON-RL [r type obj:order]
        assert_equal json [r object encoding obj:order]
    }

    test {JSON key formats that differ only in whitespace reload to the compact text} {
        r flushall
        r json.set ws . " {\n  \"a\" : \[ 1 , 2 \] ,\t\"b\" : { } \r\n} "
        r debug reload
        assert_equal {{"a":[1,2],"b":{}}} [r json.get ws]
    }

    test {DEBUG OBJECT reports a serialized length for JSON keys} {
        r flushall
        r json.set doc . {{"a":"b"}}
        assert_match {*encoding:json serializedlength:10 *} [r debug object doc]
    }

    test {DUMP and RESTORE a JSON key} {
        r flushall
        json_persist_setup
        foreach {key doc} $::json_persist_docs {
            set payload [r dump $key]
            assert_equal 24 [scan [string index $payload 0] %c]
            r restore $key:copy 0 $payload
            assert_equal [r json.get $key] [r json.get $key:copy]
            assert_equal [r debug digest-value $key] [r debug digest-value $key:copy]
        }
        assert_error {BUSYKEY*} {r restore num:1E2 0 [r dump num:1e2]}
        r restore num:1E2 0 [r dump num:1e2] replace
        assert_equal 1e2 [r json.get num:1E2]
    }

    test {RESTORE loads a valkey-json module value natively} {
        r flushall
        r debug set-skip-checksum-validation 1
        r restore doc 0 [json_module_payload {{"a":[1E2,-0.0,"x"]}} 3]
        r debug set-skip-checksum-validation 0
        assert_equal ReJSON-RL [r type doc]
        assert_equal json [r object encoding doc]
        assert_equal {{"a":[1E2,-0.0,"x"]}} [r json.get doc]
    } {} {needs:debug}

    test {RESTORE refuses valkey-json module values at encoding version 0} {
        r flushall
        r debug set-skip-checksum-validation 1
        # Version 0 walks the document node by node: 0x08 is an integer.
        set payload "[binary format c 7][binary format cW 0x81 0x45e25238df912c00][json_rdb_len 2][json_rdb_len 8][json_rdb_len 0][json_rdb_len 0][json_dump_footer]"
        catch {r restore doc 0 $payload} err
        r debug set-skip-checksum-validation 0
        assert_match {*Bad data format*} $err
        assert_equal 0 [r exists doc]
        verify_log_message 0 "*ReJSON-RL module data at encoding version 0*" 0
    } {} {needs:debug}

    test {RESTORE rejects corrupt JSON payloads} {
        r flushall
        r debug set-skip-checksum-validation 1
        foreach payload [list \
                             [json_native_payload {{"a":}}] \
                             [json_native_payload {}] \
                             [json_module_payload {[1,2} 3] \
                             [json_module_payload {1} 4]] {
            catch {r restore doc 0 $payload} err
            assert_match {*Bad data format*} $err
        }
        # Module value without its EOF marker.
        set payload "[binary format c 7][binary format cW 0x81 0x45e25238df912c03][json_rdb_len 5][json_rdb_len 1]1[json_rdb_len 5][json_dump_footer]"
        catch {r restore doc 0 $payload} err
        assert_match {*Bad data format*} $err
        r debug set-skip-checksum-validation 0
        assert_equal 0 [r dbsize]
    } {} {needs:debug}

    test {RESTORE accepts JSON nested deeper than the write limit, up to 10000} {
        r flushall
        r debug set-skip-checksum-validation 1
        set deep "[string repeat {[} 10000][string repeat {]} 10000]"
        r restore deep 0 [json_native_payload $deep]
        catch {r restore deeper 0 [json_native_payload "\[$deep\]"]} err
        r debug set-skip-checksum-validation 0
        assert_match {*Bad data format*} $err
        assert_equal $deep [r json.get deep]
        r debug reload
        assert_equal $deep [r json.get deep]
        assert_equal 0 [r exists deeper]
    } {} {needs:debug}
}
