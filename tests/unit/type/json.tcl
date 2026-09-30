# JSON keys are created through DEBUG JSON-SET until the JSON commands exist.

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
        assert_equal OK [r debug json-set doc {{"a":[1,2,{"b":null}],"c":"x"}}]
        assert_equal ReJSON-RL [r type doc]
        assert_equal json [r object encoding doc]
        assert_match {*encoding:json*} [r debug object doc fast]
    }

    test {DEBUG JSON-SET rejects invalid JSON} {
        assert_error {*SYNTAXERR*} {r debug json-set doc {{"a":}}}
        assert_equal ReJSON-RL [r type doc]
    }

    test {Commands for other types reply WRONGTYPE on a JSON key} {
        r debug json-set doc {[1]}
        assert_error {WRONGTYPE*} {r get doc}
        assert_error {WRONGTYPE*} {r lpush doc x}
    }

    test {DEL and EXISTS on a JSON key} {
        r debug json-set doc {{"a":1}}
        assert_equal 1 [r exists doc]
        assert_equal 1 [r del doc]
        assert_equal 0 [r exists doc]
    }

    test {DEBUG DIGEST-VALUE depends on JSON content and member order} {
        r debug json-set d1 {{"a":1,"b":[true,false]}}
        r debug json-set d2 {{"a":1,"b":[true,false]}}
        r debug json-set d3 {{"b":[true,false],"a":1}}
        r debug json-set d4 {{"a":1.0,"b":[true,false]}}
        set digests [r debug digest-value d1 d2 d3 d4]
        assert_equal [lindex $digests 0] [lindex $digests 1]
        assert_not_equal [lindex $digests 0] [lindex $digests 2]
        assert_not_equal [lindex $digests 0] [lindex $digests 3]
        assert_not_equal [r debug digest] 0000000000000000000000000000000000000000
    }

    test {COPY duplicates a JSON key} {
        r flushall
        r debug json-set src {{"a":[1,2,3],"b":{"c":"d"},"n":1E2}}
        assert_equal 1 [r copy src dst]
        assert_equal ReJSON-RL [r type dst]
        assert_equal json [r object encoding dst]
        set digest [r debug digest-value src]
        assert_equal $digest [r debug digest-value dst]

        # The copy is independent of the source.
        r debug json-set src {"other"}
        assert_equal $digest [r debug digest-value dst]
        r del src
        assert_equal $digest [r debug digest-value dst]
    }

    test {RENAME keeps the JSON value} {
        r flushall
        r debug json-set a {[1,"two",{"three":3}]}
        set digest [r debug digest-value a]
        r rename a b
        assert_equal 0 [r exists a]
        assert_equal ReJSON-RL [r type b]
        assert_equal $digest [r debug digest-value b]
    }

    test {EXPIRE applies to a JSON key} {
        r flushall
        r debug json-set doc {{"a":1}}
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
        r debug json-set small {1}
        r debug json-set big [json_array 1000]
        set small [r memory usage small]
        set big [r memory usage big]
        assert_morethan $small 0
        assert_morethan $big [expr {$small + 1000 * 8}]
    }

    test {SCAN TYPE filters JSON keys by their TYPE name} {
        r flushall
        r debug json-set j1 {{}}
        r debug json-set j2 {[]}
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
        r debug json-set small [json_array 63]
        r unlink small
        wait_lazyfree_done r
        assert_equal 0 [s lazyfreed_objects]

        r debug json-set big [json_array 64]
        r unlink big
        wait_lazyfree_done r
        assert_equal 1 [s lazyfreed_objects]

        # Values are counted at every depth, not just the top level.
        r debug json-set nested "{\"a\":[json_array 100]}"
        r unlink nested
        wait_lazyfree_done r
        assert_equal 2 [s lazyfreed_objects]
    } {} {needs:config-resetstat}

    test {FLUSHALL ASYNC frees JSON keys} {
        r flushall
        for {set i 0} {$i < 100} {incr i} {
            r debug json-set doc:$i [json_array 100]
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
        assert_equal {} [r acl cat json]
        assert_not_equal -1 [lsearch -exact [r acl cat] json]
    }
}
