#include "torture.h"

#define CSYNC_TEST 1
#include "csync_statedb.c"

#define TESTDB "/tmp/check_csync1/test.db"
#define TESTDBTMP "/tmp/check_csync1/test.db.ctmp"

static void setup(void **state)
{
    CSYNC *csync;
    int rc;

    rc = system("rm -rf /tmp/check_csync1");
    assert_int_equal(rc, 0);
    rc = system("rm -rf /tmp/check_csync2");
    assert_int_equal(rc, 0);
    rc = system("mkdir -p /tmp/check_csync1");
    assert_int_equal(rc, 0);
    rc = system("mkdir -p /tmp/check_csync2");
    assert_int_equal(rc, 0);
    rc = system("mkdir -p /tmp/check_csync");
    assert_int_equal(rc, 0);
    rc = csync_create(&csync, "/tmp/check_csync1", "/tmp/check_csync2");
    assert_int_equal(rc, 0);
    rc = csync_set_config_dir(csync, "/tmp/check_csync/");
    assert_int_equal(rc, 0);
    rc = csync_init(csync);
    assert_int_equal(rc, 0);

    *state = csync;
}

static void setup_db(void **state)
{
    CSYNC *csync;
    char *stmt = NULL;
    int rc;
    c_strlist_t *result = NULL;

    setup(state);
    csync = *state;

    rc = csync_statedb_create_tables(csync);
    assert_int_equal(rc, 0);

    result = csync_statedb_query(csync,
        "CREATE TABLE IF NOT EXISTS metadata ("
        "phash INTEGER(8),"
        "pathlen INTEGER,"
        "path VARCHAR(4096),"
        "inode INTEGER,"
        "uid INTEGER,"
        "gid INTEGER,"
        "mode INTEGER,"
        "modtime INTEGER(8),"
        "type INTEGER,"
        "md5 VARCHAR(32),"
        "PRIMARY KEY(phash)"
        ");"
        );

    assert_non_null(result);
    c_strlist_destroy(result);


    stmt = sqlite3_mprintf("INSERT INTO metadata"
                           "(phash, pathlen, path, inode, uid, gid, mode, modtime, type, md5) VALUES"
                           "(%lu, %d, '%q', %d, %d, %d, %d, %lu, %d, %lu);",
                           42,
                           42,
                           "It's a rainy day",
                           23,
                           42,
                           42,
                           42,
                           42,
                           2,
                           43);

    rc = csync_statedb_insert(csync, stmt);
    sqlite3_free(stmt);
}

static void teardown(void **state) {
    CSYNC *csync = *state;
    int rc;

    rc = csync_destroy(csync);
    assert_int_equal(rc, 0);
    rc = system("rm -rf /tmp/check_csync");
    assert_int_equal(rc, 0);
    rc = system("rm -rf /tmp/check_csync1");
    assert_int_equal(rc, 0);
    rc = system("rm -rf /tmp/check_csync2");
    assert_int_equal(rc, 0);

    *state = NULL;
}


static void check_csync_statedb_query_statement(void **state)
{
    CSYNC *csync = *state;
    c_strlist_t *result;

    result = csync_statedb_query(csync, "");
    assert_null(result);

    result = csync_statedb_query(csync, "SELECT;");
    assert_null(result);
}

static void check_csync_statedb_create_error(void **state)
{
    CSYNC *csync = *state;
    c_strlist_t *result;

    result = csync_statedb_query(csync, "CREATE TABLE test(phash INTEGER, text VARCHAR(10));");
    assert_non_null(result);
    c_strlist_destroy(result);

    result = csync_statedb_query(csync, "CREATE TABLE test(phash INTEGER, text VARCHAR(10));");
    assert_null(result);
}

static void check_csync_statedb_insert_statement(void **state)
{
    CSYNC *csync = *state;
    c_strlist_t *result;
    int rc;

    result = csync_statedb_query(csync, "CREATE TABLE test(phash INTEGER, text VARCHAR(10));");
    assert_non_null(result);
    c_strlist_destroy(result);

    rc = csync_statedb_insert(csync, "INSERT;");
    assert_int_equal(rc, 0);
    rc = csync_statedb_insert(csync, "INSERT");
    assert_int_equal(rc, 0);
    rc = csync_statedb_insert(csync, "");
    assert_int_equal(rc, 0);
}

static void check_csync_statedb_query_create_and_insert_table(void **state)
{
    CSYNC *csync = *state;
    c_strlist_t *result;
    int rc;

    result = csync_statedb_query(csync, "CREATE TABLE test(phash INTEGER, text VARCHAR(10));");
    c_strlist_destroy(result);
    rc = csync_statedb_insert(csync, "INSERT INTO test (phash, text) VALUES (42, 'hello');");
    assert_true(rc > 0);

    result = csync_statedb_query(csync, "SELECT * FROM test;");
    assert_non_null(result);
    assert_int_equal(result->count, 2);

    assert_string_equal(result->vector[0], "42");
    assert_string_equal(result->vector[1], "hello");
    c_strlist_destroy(result);
}

static void check_csync_statedb_is_empty(void **state)
{
    CSYNC *csync = *state;
    c_strlist_t *result;
    int rc;

    /* we have an empty db */
    assert_true(_csync_statedb_is_empty(csync));

    /* add a table and an entry */
    result = csync_statedb_query(csync, "CREATE TABLE metadata(phash INTEGER, text VARCHAR(10));");
    c_strlist_destroy(result);
    rc = csync_statedb_insert(csync, "INSERT INTO metadata (phash, text) VALUES (42, 'hello');");
    assert_true(rc > 0);

    assert_false(_csync_statedb_is_empty(csync));
}

static void check_csync_statedb_create_tables(void **state)
{
    CSYNC *csync = *state;
    char *stmt = NULL;
    int rc;

    rc = csync_statedb_create_tables(csync);
    assert_int_equal(rc, 0);

    stmt = sqlite3_mprintf("INSERT INTO metadata_temp"
           "(phash, pathlen, path, inode, uid, gid, mode, modtime, type, md5) VALUES"
                           "(%lu, %d, '%q', %d, %d, %d, %d, %ld, %d, '%q');",
           (ulong)42,
           42,
           "It's a rainy day",
           42,
           42,
           42,
           42,
           (long)42,
            2, "xsyxcmfkdsjaf");

    rc = csync_statedb_insert(csync, stmt);
    assert_true(rc > 0);

    sqlite3_free(stmt);
}

static void check_csync_statedb_drop_tables(void **state)
{
    CSYNC *csync = *state;
    int rc;

    rc = csync_statedb_drop_tables(csync);
    assert_int_equal(rc, 0);
    rc = csync_statedb_create_tables(csync);
    assert_int_equal(rc, 0);
    rc = csync_statedb_drop_tables(csync);
    assert_int_equal(rc, 0);
}

static void check_csync_statedb_insert_metadata(void **state)
{
    CSYNC *csync = *state;
    csync_file_stat_t *st;
    int i, rc;

    rc = csync_statedb_create_tables(csync);
    assert_int_equal(rc, 0);

    for (i = 0; i < 100; i++) {
        st = c_malloc(sizeof(csync_file_stat_t));
        st->phash = i;

        rc = c_rbtree_insert(csync->local.tree, (void *) st);
        assert_int_equal(rc, 0);
    }

    rc = csync_statedb_insert_metadata(csync);
    assert_int_equal(rc, 0);
}

static void check_csync_statedb_write(void **state)
{
    CSYNC *csync = *state;
    csync_file_stat_t *st;
    int i, rc;

    for (i = 0; i < 100; i++) {
        st = c_malloc(sizeof(csync_file_stat_t));
        st->phash = i;

        rc = c_rbtree_insert(csync->local.tree, (void *) st);
        assert_int_equal(rc, 0);
    }

    rc = csync_statedb_write(csync);
    assert_int_equal(rc, 0);
}

static void check_csync_statedb_get_stat_by_hash(void **state)
{
    CSYNC *csync = *state;
    csync_file_stat_t *tmp;

    tmp = csync_statedb_get_stat_by_hash(csync, (uint64_t) 42);
    assert_non_null(tmp);

    assert_int_equal(tmp->phash, 42);
    assert_int_equal(tmp->inode, 23);

    free(tmp);
}

static void check_csync_statedb_get_stat_by_hash_not_found(void **state)
{
    CSYNC *csync = *state;
    csync_file_stat_t *tmp;

    tmp = csync_statedb_get_stat_by_hash(csync, (uint64_t) 666);
    assert_null(tmp);
}

static void check_csync_statedb_get_stat_by_inode(void **state)
{
    CSYNC *csync = *state;
    csync_file_stat_t *tmp;

    tmp = csync_statedb_get_stat_by_inode(csync, (ino_t) 23);
    assert_non_null(tmp);

    assert_int_equal(tmp->phash, 42);
    assert_int_equal(tmp->inode, 23);

    free(tmp);
}

static void check_csync_statedb_get_stat_by_inode_not_found(void **state)
{
    CSYNC *csync = *state;
    csync_file_stat_t *tmp;

    tmp = csync_statedb_get_stat_by_inode(csync, (ino_t) 666);
    assert_null(tmp);
}

int torture_run_tests(void)
{
    const UnitTest tests[] = {
        unit_test_setup_teardown(check_csync_statedb_query_statement, setup, teardown),
        unit_test_setup_teardown(check_csync_statedb_create_error, setup, teardown),
        unit_test_setup_teardown(check_csync_statedb_insert_statement, setup, teardown),
        unit_test_setup_teardown(check_csync_statedb_query_create_and_insert_table, setup, teardown),
        unit_test_setup_teardown(check_csync_statedb_is_empty, setup, teardown),
        unit_test_setup_teardown(check_csync_statedb_create_tables, setup, teardown),
        unit_test_setup_teardown(check_csync_statedb_drop_tables, setup, teardown),
        unit_test_setup_teardown(check_csync_statedb_insert_metadata, setup, teardown),
        unit_test_setup_teardown(check_csync_statedb_write, setup, teardown),
        unit_test_setup_teardown(check_csync_statedb_get_stat_by_hash, setup_db, teardown),
        unit_test_setup_teardown(check_csync_statedb_get_stat_by_hash_not_found, setup_db, teardown),
        unit_test_setup_teardown(check_csync_statedb_get_stat_by_inode, setup_db, teardown),
        unit_test_setup_teardown(check_csync_statedb_get_stat_by_inode_not_found, setup_db, teardown),
    };

    return run_tests(tests);
}

