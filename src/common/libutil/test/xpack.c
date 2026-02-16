#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <string.h>
#include <errno.h>
#include <jansson.h>

#include "src/common/libtap/tap.h"
#include "fluid.h"
#include "xpack.h"

/* Test basic pack with fluid */
static void test_pack_basic (void)
{
    json_t *obj;
    json_error_t error;
    fluid_t id = 6787342413402046;
    const char *str;
    fluid_t decoded_id;

    obj = xpack_ex (&error, 0, "{s:J}", "id", id);
    ok (obj != NULL,
        "xpack_ex with single fluid works");

    char *s = json_dumps (obj, 0);
    diag ("%s", s);
    free (s);

    if (obj) {
        str = json_string_value (json_object_get (obj, "id"));
        ok (str != NULL,
            "fluid was encoded as string");
        ok (fluid_parse (str, &decoded_id) == 0,
            "encoded fluid is valid f58");
        ok (decoded_id == id,
            "encoded fluid matches original");
        json_decref (obj);
    }
}

/* Test basic unpack with fluid */
static void test_unpack_basic (void)
{
    json_t *obj;
    json_error_t error;
    fluid_t id = 67890;
    fluid_t decoded_id;
    char buf[64];

    fluid_encode (buf, sizeof (buf), id, FLUID_STRING_F58);
    obj = json_pack ("{s:s}", "id", buf);
    ok (obj != NULL,
        "created test object");

    ok (xunpack_ex (obj, &error, 0, "{s:J}", "id", &decoded_id) == 0,
        "xunpack_ex with single fluid works");
    ok (decoded_id == id,
        "decoded fluid matches original");

    json_decref (obj);
}

/* Test pack with multiple arguments including fluid */
static void test_pack_multiple (void)
{
    json_t *obj;
    json_error_t error;
    fluid_t id = 11111;
    const char *name = "testjob";
    int priority = 5;
    const char *str;
    fluid_t decoded_id;

    obj = xpack_ex (&error,
                 0,
                 "{s:J s:s s:i}",
                 "id", id,
                 "name", name,
                 "priority", priority);
    ok (obj != NULL,
        "xpack_ex with multiple args works");

    char *s = json_dumps (obj, 0);
    diag ("%s", s);
    free (s);

    if (obj) {
        str = json_string_value (json_object_get (obj, "id"));
        ok (str != NULL && fluid_parse (str, &decoded_id) == 0,
            "fluid field is valid");
        ok (decoded_id == id,
            "fluid matches");
        ok (strcmp (json_string_value (json_object_get (obj, "name")),
                    name) == 0,
            "name field matches");
        ok (json_integer_value (json_object_get (obj, "priority"))
            == priority,
            "priority field matches");
        json_decref (obj);
    }
}

/* Test unpack with multiple arguments including fluid */
static void test_unpack_multiple (void)
{
    json_t *obj;
    json_error_t error;
    fluid_t id = 22222;
    fluid_t decoded_id;
    const char *decoded_name;
    int decoded_priority;
    char buf[64];

    fluid_encode (buf, sizeof (buf), id, FLUID_STRING_F58);
    obj = json_pack ("{s:s s:s s:i}",
                     "id", buf,
                     "name", "testjob",
                     "priority", 5);
    ok (obj != NULL,
        "created test object");

    ok (xunpack_ex (obj,
                 &error,
                 0,
                 "{s:J s:s s:i}",
                 "id", &decoded_id,
                 "name", &decoded_name,
                 "priority", &decoded_priority) == 0,
        "xunpack_ex with multiple args works");
    ok (decoded_id == id,
        "fluid matches");
    ok (strcmp (decoded_name, "testjob") == 0,
        "name matches");
    ok (decoded_priority == 5,
        "priority matches");

    json_decref (obj);
}

/* Test pack with nested objects (J not nested) */
static void test_pack_nested (void)
{
    json_t *obj;
    json_error_t error;
    fluid_t id = 33333;
    const char *str;
    fluid_t decoded_id;

    obj = xpack_ex (&error,
                 0,
                 "{s:J s:{s:i s:s}}",
                 "id", id,
                 "metadata",
                   "count", 10,
                   "owner", "alice");
    ok (obj != NULL,
        "xpack_ex with nested object works");

    char *s = json_dumps (obj, 0);
    diag ("%s", s);
    free (s);

    if (obj) {
        str = json_string_value (json_object_get (obj, "id"));
        ok (str != NULL && fluid_parse (str, &decoded_id) == 0,
            "fluid field is valid");
        ok (decoded_id == id,
            "fluid matches");

        json_t *meta = json_object_get (obj, "metadata");
        ok (meta != NULL && json_is_object (meta),
            "metadata is an object");
        ok (json_integer_value (json_object_get (meta, "count")) == 10,
            "nested count matches");
        json_decref (obj);
    }
}

/* Test unpack with nested objects (J not nested) */
static void test_unpack_nested (void)
{
    json_t *obj;
    json_error_t error;
    fluid_t id = 44444;
    fluid_t decoded_id;
    int count;
    const char *owner;
    char buf[64];

    fluid_encode (buf, sizeof (buf), id, FLUID_STRING_F58);
    obj = json_pack ("{s:s s:{s:i s:s}}",
                     "id", buf,
                     "metadata",
                       "count", 10,
                       "owner", "alice");
    ok (obj != NULL,
        "created test object with nested metadata");

    ok (xunpack_ex (obj,
                 &error,
                 0,
                 "{s:J s:{s:i s:s}}",
                 "id", &decoded_id,
                 "metadata",
                   "count", &count,
                   "owner", &owner) == 0,
        "xunpack_ex with nested object works");
    ok (decoded_id == id,
        "fluid matches");
    ok (count == 10,
        "nested count matches");
    ok (strcmp (owner, "alice") == 0,
        "nested owner matches");

    json_decref (obj);
}

/* Test optional fluid (s?J) - key present */
static void test_unpack_optional_present (void)
{
    json_t *obj;
    json_error_t error;
    fluid_t id = 55555;
    fluid_t decoded_id = 0;
    char buf[64];

    fluid_encode (buf, sizeof (buf), id, FLUID_STRING_F58);
    obj = json_pack ("{s:s}", "id", buf);
    ok (obj != NULL,
        "created test object");

    ok (xunpack_ex (obj, &error, 0, "{s?J}", "id", &decoded_id) == 0,
        "xunpack_ex with optional fluid (present) works");
    ok (decoded_id == id,
        "optional fluid matches when present");

    json_decref (obj);
}

/* Test optional fluid (s?J) - key absent */
static void test_unpack_optional_absent (void)
{
    json_t *obj;
    json_error_t error;
    fluid_t decoded_id = 99999;

    obj = json_pack ("{}");
    ok (obj != NULL,
        "created empty test object");

    ok (xunpack_ex (obj, &error, 0, "{s?J}", "id", &decoded_id) == 0,
        "xunpack_ex with optional fluid (absent) works");
    diag ("error.text = %s", error.text);
    ok (decoded_id == 99999,
        "optional fluid unchanged when absent");

    json_decref (obj);
}

/* Test fast path (no custom types) */
static void test_fastpath (void)
{
    json_t *obj;
    json_error_t error;
    const char *name;
    int value;

    obj = xpack_ex (&error, 0, "{s:s s:i}", "name", "test", "value", 42);
    ok (obj != NULL,
        "xpack_ex without custom types works (fast path)");

    if (obj) {
        ok (xunpack_ex (obj,
                     &error,
                     0,
                     "{s:s s:i}",
                     "name", &name,
                     "value", &value) == 0,
            "xunpack_ex without custom types works (fast path)");
        ok (strcmp (name, "test") == 0,
            "name matches");
        ok (value == 42,
            "value matches");
        json_decref (obj);
    }
}

/* Test error cases */
static void test_errors (void)
{
    json_t *obj;
    json_error_t error;
    fluid_t id;

    obj = xpack_ex (&error, 0, NULL);
    ok (obj == NULL && errno == EINVAL,
        "xpack_ex with NULL format returns EINVAL");

    obj = json_pack ("{s:i}", "id", 12345);
    ok (xunpack_ex (obj, &error, 0, "{s:J}", "id", &id) < 0,
        "xunpack_ex fails when value is not a string");
    json_decref (obj);

    obj = json_pack ("{s:s}", "id", "not-a-valid-fluid");
    ok (xunpack_ex (obj, &error, 0, "{s:J}", "id", &id) < 0,
        "xunpack_ex fails with invalid fluid string");
    json_decref (obj);

    obj = json_pack ("{}");
    ok (xunpack_ex (obj, &error, 0, "{s:J}", "id", &id) < 0,
        "xunpack_ex fails when required key is missing");
    json_decref (obj);

    ok (xunpack_ex (NULL, &error, 0, "{s:J}", "id", &id) < 0
        && errno == EINVAL,
        "xunpack_ex with NULL root returns EINVAL");

    obj = json_pack ("{s:s}", "id", "123");
    ok (xunpack_ex (obj, &error, 0, NULL) < 0 && errno == EINVAL,
        "xunpack_ex with NULL format returns EINVAL");
    json_decref (obj);
}

/* Test with arrays (J not in array) */
static void test_pack_with_array (void)
{
    json_t *obj;
    json_error_t error;
    fluid_t id = 66666;
    const char *str;
    fluid_t decoded_id;

    obj = xpack_ex (&error,
                 0,
                 "{s:J s:[i,i,i]}",
                 "id", id,
                 "values", 1, 2, 3);
    ok (obj != NULL,
        "xpack_ex with array works");

    char *s = json_dumps (obj, 0);
    diag ("%s", s);
    free (s);

    if (obj) {
        str = json_string_value (json_object_get (obj, "id"));
        ok (str != NULL && fluid_parse (str, &decoded_id) == 0,
            "fluid field is valid");
        ok (decoded_id == id,
            "fluid matches");

        json_t *arr = json_object_get (obj, "values");
        ok (arr != NULL && json_is_array (arr),
            "values is an array");
        ok (json_array_size (arr) == 3,
            "array has 3 elements");
        json_decref (obj);
    }
}

/* Test pack/unpack round trip */
static void test_roundtrip (void)
{
    json_t *obj;
    json_error_t error;
    fluid_t id1 = 77777;
    fluid_t id2 = 88888;
    fluid_t decoded_id1, decoded_id2;
    const char *decoded_name;

    obj = xpack_ex (&error,
                 0,
                 "{s:J s:J s:s}",
                 "id1", id1,
                 "id2", id2,
                 "name", "roundtrip");
    ok (obj != NULL,
        "xpack_ex for roundtrip works");

    char *s = json_dumps (obj, 0);
    diag ("%s", s);
    free (s);

    if (obj) {
        ok (xunpack_ex (obj,
                     &error,
                     0,
                     "{s:J s:J s:s}",
                     "id1", &decoded_id1,
                     "id2", &decoded_id2,
                     "name", &decoded_name) == 0,
            "xunpack_ex for roundtrip works");
        ok (decoded_id1 == id1,
            "first fluid roundtrip successful");
        ok (decoded_id2 == id2,
            "second fluid roundtrip successful");
        ok (strcmp (decoded_name, "roundtrip") == 0,
            "name roundtrip successful");
        json_decref (obj);
    }
}

/* Test with fluid > 2^54 (JavaScript safe integer limit) */
static void test_large_fluid (void)
{
    json_t *obj;
    json_error_t error;
    fluid_t large_id = 18014398509481985ULL;  /* 2^54 + 1 */
    fluid_t decoded_id;
    const char *str;
    char *s;

    /* Verify we're actually testing a large value */
    ok (large_id > (1ULL << 54),
        "test value is larger than 2^54");

    obj = xpack_ex (&error, 0, "{s:J}", "id", large_id);
    ok (obj != NULL,
        "xpack_ex with fluid > 2^54 works");

    if (obj) {
        s = json_dumps (obj, 0);
        diag ("%s", s);
        free (s);

        /* Verify it's encoded as string, not integer */
        str = json_string_value (json_object_get (obj, "id"));
        ok (str != NULL,
            "large fluid encoded as string");
        ok (!json_is_integer (json_object_get (obj, "id")),
            "large fluid not stored as JSON integer");

        /* Verify we can decode it back */
        ok (xunpack_ex (obj, &error, 0, "{s:J}", "id", &decoded_id) == 0,
            "xunpack_ex with fluid > 2^54 works");
        ok (decoded_id == large_id,
            "large fluid roundtrip successful (expected %llu, got %llu)",
            (unsigned long long)large_id,
            (unsigned long long)decoded_id);

        json_decref (obj);
    }
}

int main (int argc, char *argv[])
{
    plan (NO_PLAN);

    test_pack_basic ();
    test_unpack_basic ();
    test_pack_multiple ();
    test_unpack_multiple ();
    test_pack_nested ();
    test_unpack_nested ();
    test_unpack_optional_present ();
    test_unpack_optional_absent ();
    test_fastpath ();
    test_errors ();
    test_pack_with_array ();
    test_roundtrip ();
    test_large_fluid ();

    done_testing ();
    return 0;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
