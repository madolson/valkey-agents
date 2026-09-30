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
        r debug json-set $key $doc
    }
}

# Compact serialization of every test key plus the dataset digest.
proc json_persist_state {} {
    set state {}
    foreach {key doc} $::json_persist_docs {
        lappend state $key [r debug json-get $key] [r debug digest-value $key]
    }
    lappend state [r debug digest]
    return $state
}

start_server {tags {json needs:debug}} {
    test {JSON keys keep exact number and string text across DEBUG RELOAD} {
        r flushall
        json_persist_setup
        set before [json_persist_state]
        assert_equal {1E2} [r debug json-get num:1E2]
        assert_equal {-0.0} [r debug json-get num:-0.0]
        assert_equal {0.30000000000000004} [r debug json-get num:0.3]
        r debug reload
        assert_equal $before [json_persist_state]
        assert_equal ReJSON-RL [r type obj:order]
        assert_equal json [r object encoding obj:order]
    }

    test {JSON key formats that differ only in whitespace reload to the compact text} {
        r flushall
        r debug json-set ws " {\n  \"a\" : \[ 1 , 2 \] ,\t\"b\" : { } \r\n} "
        r debug reload
        assert_equal {{"a":[1,2],"b":{}}} [r debug json-get ws]
    }

    test {DEBUG OBJECT reports a serialized length for JSON keys} {
        r flushall
        r debug json-set doc {{"a":"b"}}
        assert_match {*encoding:json serializedlength:10 *} [r debug object doc]
    }

    test {DUMP and RESTORE a JSON key} {
        r flushall
        json_persist_setup
        foreach {key doc} $::json_persist_docs {
            set payload [r dump $key]
            assert_equal 24 [scan [string index $payload 0] %c]
            r restore $key:copy 0 $payload
            assert_equal [r debug json-get $key] [r debug json-get $key:copy]
            assert_equal [r debug digest-value $key] [r debug digest-value $key:copy]
        }
        assert_error {BUSYKEY*} {r restore num:1E2 0 [r dump num:1e2]}
        r restore num:1E2 0 [r dump num:1e2] replace
        assert_equal 1e2 [r debug json-get num:1E2]
    }

    test {RESTORE loads a valkey-json module value natively} {
        r flushall
        r debug set-skip-checksum-validation 1
        r restore doc 0 [json_module_payload {{"a":[1E2,-0.0,"x"]}} 3]
        r debug set-skip-checksum-validation 0
        assert_equal ReJSON-RL [r type doc]
        assert_equal json [r object encoding doc]
        assert_equal {{"a":[1E2,-0.0,"x"]}} [r debug json-get doc]
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
        assert_equal $deep [r debug json-get deep]
        r debug reload
        assert_equal $deep [r debug json-get deep]
        assert_equal 0 [r exists deeper]
    } {} {needs:debug}
}
