# Persistence of native JSON keys, and loading of valkey-json module data
# without the module.

source tests/support/aofmanifest.tcl

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

# Writes through every mutating JSON command, including one that fails after
# changing the document.
proc json_mutate {r} {
    $r json.set obj .a {{"n":1}}
    $r json.numincrby obj .a.n 2.5
    $r json.nummultby obj {$..n} 2
    $r json.strappend str:unicode {"!"}
    $r json.toggle obj {$.b[1].c[0]}
    $r json.arrappend obj .b 1 2
    $r json.arrinsert obj .b 0 {"first"}
    $r json.arrpop obj .b
    $r json.arrtrim obj .b 0 2
    $r json.clear obj {.b[2]}
    $r json.del obj {.b[2]}
    $r json.mset int . 43 obj .z null
    $r json.merge obj . {{"z":null,"m":{"k":1}}}
    $r json.set deep . {[[1,2],[3]]}
    catch {$r json.arrinsert deep {$..*} 2 0}
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

    test {JSON writes replicate to a replica} {
        start_server {} {
            r replicaof [srv -1 host] [srv -1 port]
            wait_for_sync r
            json_mutate [srv -1 client]
            wait_for_ofs_sync [srv -1 client] r
            assert_equal [json_state [srv -1 client] $json_docs] [json_state r $json_docs]
            assert_equal {[[1,2,0],[3]]} [r json.get deep]
            r replicaof no one
        }
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

    test {JSON writes after an AOF rewrite replay on restart} {
        json_mutate r
        set before [json_state r $json_docs]
        restart_server 0 true false
        assert_equal $before [json_state r $json_docs]
        r bgrewriteaof
        waitForBgrewriteaof r
        restart_server 0 true false
        assert_equal $before [json_state r $json_docs]
    }
}

set server_path [tmpdir server.json-module-aof]
set aof_dirpath "$server_path/appendonlydir"
set aof_basename appendonly.aof
set aof_manifest_file "$aof_dirpath/$aof_basename$::manifest_suffix"
set defaults {appendonly yes appendfilename appendonly.aof appenddirname appendonlydir aof-use-rdb-preamble no}

# What the valkey-json module's AOF rewrite writes: JSON.SET key . doc.
create_aof $aof_dirpath "$aof_dirpath/$aof_basename.1$::base_aof_suffix$::aof_format_suffix" {
    append_to_aof [formatCommand select 0]
    append_to_aof [formatCommand JSON.SET num . 1E2]
    append_to_aof [formatCommand JSON.SET obj . {{"a":[1,-0.0,"x"],"b":{}}}]
    append_to_aof [formatCommand JSON.SET obj .b.c true]
    append_to_aof [formatCommand JSON.ARRAPPEND obj .a null]
    append_to_aof [formatCommand JSON.SET num . 2]
    append_to_aof [formatCommand JSON.SET deep . {[[[1]]]}]
}
create_aof_manifest $aof_dirpath $aof_manifest_file {
    append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b\n"
}

start_server_aof [list dir $server_path json.max-path-limit 2] {
    test {A valkey-json module AOF replays natively} {
        r select 0
        assert_equal 2 [r json.get num]
        assert_equal {{"a":[1,-0.0,"x",null],"b":{"c":true}}} [r json.get obj]
        assert_equal ReJSON-RL [r type obj]
    }

    test {AOF replay is not bound by json.max-path-limit} {
        assert_equal {[[[1]]]} [r json.get deep]
        assert_error {LIMIT*} {r json.set deep . {[[[1]]]}}
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
