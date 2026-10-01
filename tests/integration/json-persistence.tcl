# Persistence of native JSON keys, and loading of valkey-json module data
# without the module.

set json_docs [list \
    num:1E2 {1E2} \
    num:-0.0 {-0.0} \
    num:0.3 {0.30000000000000004} \
    num:u64max {18446744073709551615} \
    int {42} \
    str:unicode {"\u00e9 \ud83d\ude00"} \
    obj {{"b":[1,{"c":[true,false,null]},"x"],"a":{}}} \
    deep "[string repeat {[} 128][string repeat {]} 128]" \
]

proc json_setup {r docs} {
    foreach {key doc} $docs {
        $r json.set $key . $doc
    }
}

proc json_state {r docs} {
    set state {}
    foreach {key doc} $docs {
        lappend state $key [$r json.get $key]
    }
    lappend state [$r debug digest]
    return $state
}

# Key and JSON.GET <key> $ output pairs written by utils/json-diff/mkfixture.py.
proc json_fixture_expected {} {
    set fd [open tests/assets/json-module-v3.expected r]
    fconfigure $fd -translation binary
    set pairs {}
    foreach line [split [read $fd] "\n"] {
        if {$line eq {}} continue
        set tab [string first "\t" $line]
        lappend pairs [string range $line 0 [expr {$tab - 1}]] [string range $line [expr {$tab + 1}] end]
    }
    close $fd
    return $pairs
}

proc json_assert_fixture {r} {
    set expected [json_fixture_expected]
    assert_equal [expr {[llength $expected] / 2}] [$r dbsize]
    foreach {key get} $expected {
        assert_equal ReJSON-RL [$r type $key]
        assert_equal json [$r object encoding $key]
        assert_equal $get "\[[$r json.get $key]\]"
    }
}

tags {"json external:skip needs:debug"} {

test {valkey-check-rdb accepts the valkey-json module fixture} {
    catch {exec $::VALKEY_CHECK_RDB_BIN tests/assets/json-module-v3.rdb} result
    assert_match {*\\o/ RDB looks OK! \\o/*} $result
}

set server_path [tmpdir "server.json-module-rdb"]
exec cp -f tests/assets/json-module-v3.rdb $server_path
start_server [list overrides [list dir $server_path dbfilename json-module-v3.rdb]] {
    test {valkey-json module RDB loads natively without the module} {
        r select 0
        assert_no_match {*json*} [r module list]
        json_assert_fixture r
    }

    test {Natively loaded module data survives DEBUG RELOAD} {
        set digest [r debug digest]
        r debug reload
        json_assert_fixture r
        assert_equal $digest [r debug digest]
    }

    test {valkey-check-rdb accepts native JSON keys} {
        r save
        catch {exec $::VALKEY_CHECK_RDB_BIN [file join $server_path json-module-v3.rdb]} result
        assert_match {*\\o/ RDB looks OK! \\o/*} $result
    }

    test {Replica full syncs natively loaded module data} {
        start_server {} {
            r replicaof [srv -1 host] [srv -1 port]
            wait_for_sync r
            r select 0
            json_assert_fixture r
            assert_equal [r -1 debug digest] [r debug digest]
        }
    }
}

start_server {} {
    test {JSON keys survive SAVE and restart} {
        json_setup r $json_docs
        r pexpireat obj 9999999999999
        set before [json_state r $json_docs]
        r save
        restart_server 0 true false
        assert_equal $before [json_state r $json_docs]
        assert_equal 9999999999999 [r pexpiretime obj]
    }

    test {MIGRATE moves JSON keys} {
        start_server {} {
            set before [json_state [srv -1 client] $json_docs]
            r -1 migrate [srv 0 host] [srv 0 port] "" 9 5000 keys {*}[dict keys $json_docs]
            assert_equal 0 [r -1 dbsize]
            assert_equal $before [json_state r $json_docs]
            r migrate [srv -1 host] [srv -1 port] "" 9 5000 keys {*}[dict keys $json_docs]
        }
        assert_equal $before [json_state r $json_docs]
    }

    foreach diskless {no yes} {
        test "JSON keys full sync to a replica (diskless $diskless)" {
            r config set repl-diskless-sync $diskless
            r config set repl-diskless-sync-delay 0
            start_server {} {
                r replicaof [srv -1 host] [srv -1 port]
                wait_for_sync r
                assert_equal [json_state [srv -1 client] $json_docs] [json_state r $json_docs]
                r replicaof no one
            }
        }
    }
}

start_server {overrides {appendonly yes aof-use-rdb-preamble no}} {
    test {AOF rewrite emits JSON.SET key . doc} {
        json_setup r $json_docs
        r pexpireat obj 9999999999999
        r bgrewriteaof
        waitForBgrewriteaof r
        set fd [open [get_base_aof_path r] r]
        fconfigure $fd -translation binary
        set aof [read $fd]
        close $fd
        foreach {key doc} $json_docs {
            set text [r json.get $key]
            set cmd "*4\r\n\$8\r\nJSON.SET\r\n\$[string length $key]\r\n$key\r\n\$1\r\n.\r\n\$[string length $text]\r\n$text\r\n"
            assert {[string first $cmd $aof] >= 0}
        }
        assert_match "*JSON.SET\r\n\$3\r\nobj\r\n*PEXPIREAT\r\n\$3\r\nobj\r\n\$13\r\n9999999999999\r\n*" $aof
    }
}

start_server {overrides {appendonly yes aof-use-rdb-preamble yes}} {
    test {JSON keys survive AOF rewrite with an RDB preamble} {
        json_setup r $json_docs
        set before [json_state r $json_docs]
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal $before [json_state r $json_docs]
        restart_server 0 true false
        assert_equal $before [json_state r $json_docs]
    }
}

}
