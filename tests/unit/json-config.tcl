# Configs and INFO fields keep the valkey-json module's names, so that config
# files and scrapers written for the module keep working.

proc json_info_field {field} {
    return [getInfoProperty [r info json] $field]
}

start_server {tags {json external:skip} overrides {json.max-document-size 1mb json.max-path-limit 64 json.debug-mode yes}} {
    test {JSON module configs load from the config file} {
        assert_equal [r config get json.max-document-size] {json.max-document-size 1048576}
        assert_equal [r config get json.max-path-limit] {json.max-path-limit 64}
        assert_equal [r config get json.debug-mode] {json.debug-mode yes}
    }

    test {CONFIG GET json.* matches the module, json.debug-mode is hidden} {
        assert_equal [lsort [r config get json.*]] [lsort {json.max-document-size 1048576 json.max-path-limit 64}]
    }

    test {JSON config CONFIG SET limits} {
        r config set json.max-path-limit 10000
        assert_equal [r config get json.max-path-limit] {json.max-path-limit 10000}
        r config set json.max-path-limit 0
        assert_error {*argument must be between 0 and 10000 inclusive*} {r config set json.max-path-limit 10001}
        assert_error {*argument must be between 0 and 10000 inclusive*} {r config set json.max-path-limit -1}
        assert_error {*argument couldn't be parsed into an integer*} {r config set json.max-path-limit abc}
        r config set json.max-document-size 2gb
        assert_equal [r config get json.max-document-size] {json.max-document-size 2147483648}
        assert_error {*argument must be a memory value*} {r config set json.max-document-size -1}
        assert_error {*can't set immutable config*} {r config set json.debug-mode no}
    }

    test {JSON configs survive CONFIG REWRITE} {
        r config set json.max-path-limit 99
        r config set json.max-document-size 5mb
        r config rewrite
        restart_server 0 true false
        assert_equal [r config get json.max-path-limit] {json.max-path-limit 99}
        assert_equal [r config get json.max-document-size] {json.max-document-size 5242880}
        assert_equal [r config get json.debug-mode] {json.debug-mode yes}
    }
}

start_server {tags {json external:skip} config "default.conf" args {--json.max-path-limit 64}} {
    test {JSON module config from the command line} {
        assert_equal [r config get json.max-path-limit] {json.max-path-limit 64}
        assert_equal [r config get json.max-document-size] {json.max-document-size 0}
        assert_equal [r config get json.debug-mode] {json.debug-mode no}
    }
}

test {Unknown dotted config without its module still aborts startup} {
    catch {exec $::VALKEY_SERVER_BIN --json.no-such-config 1} err
    assert_match {*Module Configuration detected without loadmodule directive or no ApplyConfig call: aborting*} $err
    catch {exec $::VALKEY_SERVER_BIN --nosuchmodule.capacity 100} err
    assert_match {*Module Configuration detected without loadmodule directive or no ApplyConfig call: aborting*} $err
} {} {external:skip}

start_server {tags {json needs:debug}} {
    test {INFO json_core_metrics is where the module put it} {
        foreach section {json json_core_metrics modules everything} {
            assert_match "*# json_core_metrics\r\njson_total_memory_bytes:*\r\njson_num_documents:*" [r info $section]
        }
        foreach section {{} default all server} {
            assert_no_match {*json_*} [r info {*}$section]
        }
    }

    test {INFO json counts documents and their memory} {
        r flushall
        assert_equal [json_info_field json_num_documents] 0
        assert_equal [json_info_field json_total_memory_bytes] 0

        r json.set a . {{"x":[1,2,3],"y":"hello"}}
        assert_equal [json_info_field json_num_documents] 1
        set one [json_info_field json_total_memory_bytes]
        assert_morethan $one 0

        r copy a b
        assert_equal [json_info_field json_num_documents] 2
        assert_equal [json_info_field json_total_memory_bytes] [expr {2 * $one}]

        r debug reload
        assert_equal [json_info_field json_num_documents] 2
        assert_equal [json_info_field json_total_memory_bytes] [expr {2 * $one}]

        r json.set a . {[]}
        assert_equal [json_info_field json_num_documents] 2
        assert_lessthan [json_info_field json_total_memory_bytes] [expr {2 * $one}]

        r del a b
        assert_equal [json_info_field json_num_documents] 0
        assert_equal [json_info_field json_total_memory_bytes] 0
    }

    test {INFO json after lazy free} {
        r config set lazyfree-lazy-user-del yes
        set items {}
        for {set i 0} {$i < 1000} {incr i} {
            lappend items "\"$i\""
        }
        r json.set big . "\[[join $items ,]\]"
        assert_equal [json_info_field json_num_documents] 1
        r unlink big
        wait_for_condition 50 100 {
            [json_info_field json_num_documents] == 0
        } else {
            fail "lazy free of the JSON document did not complete"
        }
        assert_equal [json_info_field json_total_memory_bytes] 0
        r config set lazyfree-lazy-user-del no
    }
}
