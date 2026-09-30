# The bloom module (src/modules/bloom) is linked into the server unless it was
# built with BUILD_BLOOM=no or BUILD_BLOOM=module. Without it there is nothing
# to test here, and a bf.* directive in a config file would abort startup.
set bloom_static 0
start_server {tags {"bloom"}} {
    foreach m [r module list] {
        if {[dict get $m name] eq "bf" && [dict get $m path] eq "bf"} { set bloom_static 1 }
    }
}

if {$bloom_static} {

start_server {tags {"bloom"} overrides {enable-module-command yes}} {
    test {BF.RESERVE, BF.ADD, BF.MADD, BF.EXISTS, BF.MEXISTS, BF.CARD} {
        r del bf
        assert_equal OK [r bf.reserve bf 0.01 1000]
        assert_equal 1 [r bf.add bf a]
        assert_equal 0 [r bf.add bf a]
        assert_equal {1 1 0} [r bf.madd bf b c a]
        assert_equal 1 [r bf.exists bf b]
        assert_equal 0 [r bf.exists bf nonexistent]
        assert_equal {1 1 1 0} [r bf.mexists bf a b c nonexistent]
        assert_equal 3 [r bf.card bf]
        assert_equal 0 [r bf.card nokey]
        assert_equal bloomfltr [r type bf]
        assert_error {*item exists*} {r bf.reserve bf 0.01 1000}
        r set str x
        assert_error {WRONGTYPE*} {r bf.add str a}
    }

    test {BF.INSERT and BF.INFO} {
        r del ins
        assert_equal {1 1} [r bf.insert ins CAPACITY 50 ERROR 0.05 EXPANSION 4 ITEMS x y]
        assert_equal 50 [r bf.info ins CAPACITY]
        assert_equal 2 [r bf.info ins ITEMS]
        assert_equal 4 [r bf.info ins EXPANSION]
        assert_equal 0.05 [r bf.info ins ERROR]
        assert_error {*not found*} {r bf.insert missing NOCREATE ITEMS x}
    }

    test {Scripts can call bloom commands} {
        r del lk
        assert_equal 1 [r eval {redis.call('bf.add', KEYS[1], 'v'); return redis.call('bf.exists', KEYS[1], 'v')} 1 lk]
        assert_equal 3 [r eval {return 1 + 2} 0]
    }

    test {MODULE UNLOAD of the static bloom module is refused} {
        assert_error {*exports one or more module-side data types*} {r module unload bf}
        assert_equal 1 [r bf.exists bf a]
    }

    test {DEBUG RELOAD keeps bloom filters} {
        set info [r bf.info bf]
        r debug reload
        assert_equal $info [r bf.info bf]
        assert_equal {1 1 1} [r bf.mexists bf a b c]
    } {} {needs:debug}
}

start_server {tags {"bloom external:skip"} overrides {aof-use-rdb-preamble no}} {
    # BF.INFO SIZE includes spare capacity in the module's filter vector, which
    # differs between a BF.LOAD replay and an RDB load, so compare digests.
    test {AOF rewrite round-trips bloom filters} {
        r bf.reserve sc 0.01 100 EXPANSION 2
        for {set i 0} {$i < 300} {incr i} { r bf.add sc item$i }
        set digest [r debug digest-value sc]
        r config set appendonly yes
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal $digest [r debug digest-value sc]
        assert_equal 2 [r bf.info sc FILTERS]
        assert_equal 1 [r bf.exists sc item299]
    }

    test {Bloom filters survive a restart} {
        set digest [r debug digest-value sc]
        r save
        restart_server 0 true false
        assert_equal $digest [r debug digest-value sc]
        assert_equal 1 [r bf.exists sc item0]
    }
}

# Bloom is loaded by default, so if it did not declare forkless support every
# forkless save would fall back to fork.
start_server {tags {"bloom external:skip"} overrides {save "" forkless-infrastructure-enabled yes}} {
    test {Forkless BGSAVE saves bloom filters} {
        r config set bgsave-default-method forkless
        r bf.reserve fk 0.01 100 EXPANSION 2
        for {set i 0} {$i < 300} {incr i} { r bf.add fk item$i }
        set digest [r debug digest-value fk]
        r bgsave
        waitForBgsave r
        assert_equal forkless [s rdb_last_bgsave_type]
        r debug reload nosave
        assert_equal $digest [r debug digest-value fk]
    } {} {needs:debug}
}

# Likewise, without the atomic slot migration option CLUSTER MIGRATESLOTS
# would be refused on every default build.
start_cluster 2 0 {tags {"bloom logreqres:skip external:skip cluster network"}} {
    test {Atomic slot migration moves bloom filters} {
        set src 0
        if {[catch {R 0 bf.reserve mk 0.01 100 EXPANSION 2}]} {
            set src 1
            R 1 bf.reserve mk 0.01 100 EXPANSION 2
        }
        set dst [expr {1 - $src}]
        for {set i 0} {$i < 300} {incr i} { R $src bf.add mk item$i }
        set digest [R $src debug digest-value mk]
        set slot [R $src cluster keyslot mk]

        assert_equal OK [R $src cluster migrateslots slotsrange $slot $slot node [R $dst cluster myid]]
        wait_for_condition 100 100 {
            [R $src cluster countkeysinslot $slot] == 0 &&
            ![catch {R $dst bf.exists mk item299} reply] && $reply == 1
        } else {
            fail "bloom key was not migrated"
        }
        assert_equal $digest [R $dst debug digest-value mk]
    } {} {needs:debug}
}

start_server {tags {"bloom external:skip"} overrides {bf.bloom-capacity 1234 bf.bloom-fp-rate 0.02} args {--bf.bloom-expansion 4}} {
    test {bf.* configs from the config file and command line are applied} {
        assert_equal {bf.bloom-capacity 1234} [r config get bf.bloom-capacity]
        assert_equal {bf.bloom-fp-rate 0.02} [r config get bf.bloom-fp-rate]
        assert_equal {bf.bloom-expansion 4} [r config get bf.bloom-expansion]
        r bf.add k a
        assert_equal 1234 [r bf.info k CAPACITY]
        assert_equal 4 [r bf.info k EXPANSION]
    }

    test {bf.* configs survive CONFIG REWRITE and restart} {
        r config set bf.bloom-capacity 999
        r config rewrite
        set conf [exec cat [srv 0 config_file]]
        assert_match {*bf.bloom-capacity 999*} $conf
        assert_no_match {*loadmodule*} $conf
        restart_server 0 true false
        assert_equal {bf.bloom-capacity 999} [r config get bf.bloom-capacity]
    }
}

test {Unknown bf.* config still aborts startup} {
    catch {exec $::VALKEY_SERVER_BIN --port 0 --bf.bloom-nonexistent 1} err
    assert_match {*Unused Module Configuration: bf.bloom-nonexistent*} $err
} {} {external:skip}

# bloom-module.rdb was written by the bloom module built as a .so from the same
# vendored source and loaded with --loadmodule. sc scaled out to three filters;
# the probe list is every probe0..probe1999 the module reported as a false
# positive for sc, which only reproduces if the bit arrays and seeds load intact.
set server_path [tmpdir "server.bloom-rdb"]
exec cp tests/assets/bloom-module.rdb $server_path
start_server [list tags {"bloom external:skip"} overrides [list dir $server_path dbfilename bloom-module.rdb save "" bf.bloom-capacity 4321]] {
    test {Bloom filters written by the bloom module load into the static module} {
        r select 0
        assert_equal {bf.bloom-capacity 4321} [r config get bf.bloom-capacity]
        assert_equal {Capacity 700 Size 1724 {Number of filters} 3 {Number of items inserted} 494 {Error rate} 0.01 {Expansion rate} 2 {Tightening ratio} 0.5 {Max scaled capacity} 26214300} [r bf.info sc]
        assert_equal {Capacity 1000 Size 2062 {Number of filters} 1 {Number of items inserted} 100 {Error rate} 0.001 {Expansion rate} {}} [r bf.info ns]
        assert_equal {Capacity 50 Size 303 {Number of filters} 1 {Number of items inserted} 3 {Error rate} 0.05 {Expansion rate} 2 {Tightening ratio} 0.5 {Max scaled capacity} 26214350} [r bf.info ins]
        assert_equal 494 [r bf.card sc]

        set items {}
        for {set i 0} {$i < 500} {incr i} { lappend items item$i }
        assert_equal [lrepeat 500 1] [r bf.mexists sc {*}$items]
        assert_equal [lrepeat 100 1] [r bf.mexists ns {*}[lrange $items 0 99]]
        assert_equal {1 1 1} [r bf.mexists ins a b c]

        set probes {probe5 probe12 probe92 probe183 probe232 probe300 probe331 probe524 probe574 probe597
                    probe607 probe613 probe692 probe705 probe792 probe851 probe1002 probe1041 probe1052
                    probe1060 probe1073 probe1078 probe1091 probe1113 probe1273 probe1316 probe1359
                    probe1436 probe1496 probe1532 probe1548 probe1568 probe1714 probe1750 probe1763
                    probe1769 probe1819 probe1830 probe1848 probe1855 probe1978}
        assert_equal [lrepeat [llength $probes] 1] [r bf.mexists sc {*}$probes]
        assert_equal {0 0 0 0} [r bf.mexists sc probe0 probe1 probe2 probe3]
    }
}

}
