#include <ruby.h>
#include <ruby/st.h>
#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h>
# include <unistd.h>
#endif

static st_data_t expect_size = 32;
struct checker {
    st_table *tbl;
    st_index_t nr;
    VALUE test;
};

static void
force_unpack_check(struct checker *c, st_data_t key, st_data_t val)
{
    if (c->nr == 0) {
        st_data_t i;

        if (c->tbl->bins != NULL) rb_bug("should be packed");

        /* force unpacking during iteration: */
        for (i = 1; i < expect_size; i++)
            st_add_direct(c->tbl, i, i);

        if (c->tbl->bins == NULL) rb_bug("should be unpacked");
    }

    if (key != c->nr) {
        rb_bug("unexpected key: %"PRIuVALUE" (expected %"PRIuVALUE")", (VALUE)key, (VALUE)c->nr);
    }
    if (val != c->nr) {
        rb_bug("unexpected val: %"PRIuVALUE" (expected %"PRIuVALUE")", (VALUE)val, (VALUE)c->nr);
    }

    c->nr++;
}

static int
unp_fec_i(st_data_t key, st_data_t val, st_data_t args, int error)
{
    struct checker *c = (struct checker *)args;

    if (error) {
        if (c->test == ID2SYM(rb_intern("delete2")))
            return ST_STOP;

        rb_bug("unexpected error");
    }

    force_unpack_check(c, key, val);

    if (c->test == ID2SYM(rb_intern("check"))) {
        return ST_CHECK;
    }
    if (c->test == ID2SYM(rb_intern("delete1"))) {
        if (c->nr == 1) return ST_DELETE;
        return ST_CHECK;
    }
    if (c->test == ID2SYM(rb_intern("delete2"))) {
        if (c->nr == 1) {
            st_data_t k = 0;
            st_data_t v;

            if (!st_delete(c->tbl, &k, &v)) {
                rb_bug("failed to delete");
            }
            if (v != 0) {
                rb_bug("unexpected value deleted: %"PRIuVALUE" (expected 0)", (VALUE)v);
            }
        }
        return ST_CHECK;
    }

    rb_raise(rb_eArgError, "unexpected arg: %+"PRIsVALUE, c->test);
}

static VALUE
unp_fec(VALUE self, VALUE test)
{
    st_table *tbl = st_init_numtable();
    struct checker c;

    c.tbl = tbl;
    c.nr = 0;
    c.test = test;

    st_add_direct(tbl, 0, 0);

    if (tbl->bins != NULL) rb_bug("should still be packed");

    st_foreach_check(tbl, unp_fec_i, (st_data_t)&c, -1);

    if (c.test == ID2SYM(rb_intern("delete2"))) {
        if (c.nr != 1) {
            rb_bug("mismatched iteration: %"PRIuVALUE" (expected 1)", (VALUE)c.nr);
        }
    }
    else if (c.nr != expect_size) {
        rb_bug("mismatched iteration: %"PRIuVALUE" (expected %"PRIuVALUE")",
                (VALUE)c.nr, (VALUE)expect_size);
    }

    if (tbl->bins == NULL) rb_bug("should be unpacked");

    st_free_table(tbl);

    return Qnil;
}

static int
unp_fe_i(st_data_t key, st_data_t val, st_data_t args)
{
    struct checker *c = (struct checker *)args;

    force_unpack_check(c, key, val);
    if (c->test == ID2SYM(rb_intern("unpacked"))) {
        return ST_CONTINUE;
    }
    else if (c->test == ID2SYM(rb_intern("unpack_delete"))) {
        if (c->nr == 1) {
            st_data_t k = 0;
            st_data_t v;

            if (!st_delete(c->tbl, &k, &v)) {
                rb_bug("failed to delete");
            }
            if (v != 0) {
                rb_bug("unexpected value deleted: %"PRIuVALUE" (expected 0)", (VALUE)v);
            }
            return ST_CONTINUE;
        }
        rb_bug("should never get here");
    }

    rb_raise(rb_eArgError, "unexpected arg: %+"PRIsVALUE, c->test);
}

static VALUE
unp_fe(VALUE self, VALUE test)
{
    st_table *tbl = st_init_numtable();
    struct checker c;

    c.tbl = tbl;
    c.nr = 0;
    c.test = test;

    st_add_direct(tbl, 0, 0);

    if (tbl->bins != NULL) rb_bug("should still be packed");

    st_foreach(tbl, unp_fe_i, (st_data_t)&c);

    if (c.test == ID2SYM(rb_intern("unpack_delete"))) {
        if (c.nr != 1) {
            rb_bug("mismatched iteration: %"PRIuVALUE" (expected 1)", (VALUE)c.nr);
        }
    }
    else if (c.nr != expect_size) {
        rb_bug("mismatched iteration: %"PRIuVALUE" (expected %"PRIuVALUE"o)",
                (VALUE)c.nr, (VALUE)expect_size);
    }

    if (tbl->bins == NULL) rb_bug("should be unpacked");

    st_free_table(tbl);

    return Qnil;
}

#if defined(HAVE_SYS_MMAN_H) && (defined(MAP_ANON) || defined(MAP_ANONYMOUS))
# if !defined(MAP_ANON) && defined(MAP_ANONYMOUS)
#  define MAP_ANON MAP_ANONYMOUS
# endif

struct mmapped_key {
    unsigned char *ptr;
    size_t len;
    size_t map_len;
    int unmapped;
};

struct mmapped_foreach_state {
    struct mmapped_key **keys;
    st_index_t key_count;
    st_index_t seen;
};

static int
mmapped_key_cmp(st_data_t lhs_, st_data_t rhs_)
{
    const struct mmapped_key *lhs = (const struct mmapped_key *)lhs_;
    const struct mmapped_key *rhs = (const struct mmapped_key *)rhs_;

    if (lhs->len != rhs->len) return 1;
    return memcmp(lhs->ptr, rhs->ptr, lhs->len);
}

static st_index_t
mmapped_key_hash(st_data_t key_)
{
    const struct mmapped_key *key = (const struct mmapped_key *)key_;
    st_index_t hash = 0;
    size_t i;

    for (i = 0; i < key->len; i++) {
        hash = hash * 997 + key->ptr[i];
    }
    return hash + (hash >> 5);
}

static const struct st_hash_type mmapped_key_hash_type = {
    mmapped_key_cmp,
    mmapped_key_hash,
};

static struct mmapped_key *
mmapped_key_new(size_t map_len, unsigned char fill)
{
    struct mmapped_key *key = ALLOC(struct mmapped_key);

    key->len = 16;
    key->map_len = map_len;
    key->unmapped = 0;
    key->ptr = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
    if (key->ptr == MAP_FAILED) rb_sys_fail("mmap");
    memset(key->ptr, fill, key->len);
    return key;
}

static void
mmapped_key_free(struct mmapped_key *key)
{
    if (key == NULL) return;
    if (!key->unmapped) {
        if (munmap(key->ptr, key->map_len) != 0) rb_sys_fail("munmap");
    }
    xfree(key);
}

static int
mmapped_foreach_i(st_data_t key_, st_data_t val, st_data_t arg)
{
    struct mmapped_foreach_state *state = (struct mmapped_foreach_state *)arg;
    struct mmapped_key *key = (struct mmapped_key *)key_;
    (void)val;
    (void)key;

    if (state->seen == 0) {
        struct mmapped_key *future = state->keys[1];
        if (munmap(future->ptr, future->map_len) != 0) rb_sys_fail("munmap");
        future->unmapped = 1;
    }

    state->seen++;
    return ST_CONTINUE;
}

static VALUE
foreach_keeps_stored_hash(VALUE self)
{
    const st_index_t key_count = 17;
    st_table *tbl = st_init_table_with_size(&mmapped_key_hash_type, key_count);
    struct mmapped_key **keys = ALLOC_N(struct mmapped_key *, key_count);
    struct mmapped_foreach_state state;
    long page_size_raw;
    size_t page_size;
    st_index_t i;

    (void)self;

    page_size_raw = sysconf(_SC_PAGESIZE);
    page_size = page_size_raw > 0 ? (size_t)page_size_raw : 4096;

    for (i = 0; i < key_count; i++) {
        keys[i] = mmapped_key_new(page_size, (unsigned char)('a' + i));
        st_insert(tbl, (st_data_t)keys[i], (st_data_t)i);
    }

# ifdef ST_USE_SWISS_BINS
    if (tbl->bins == NULL || tbl->ctrl != NULL) {
        rb_bug("unexpected foreach/hash test table shape");
    }
# endif

    state.keys = keys;
    state.key_count = key_count;
    state.seen = 0;
    st_foreach(tbl, mmapped_foreach_i, (st_data_t)&state);

    st_free_table(tbl);
    for (i = 0; i < key_count; i++) {
        mmapped_key_free(keys[i]);
    }
    xfree(keys);

    return SIZET2NUM(state.seen);
}
#else
static VALUE
foreach_keeps_stored_hash(VALUE self)
{
    (void)self;
    return ID2SYM(rb_intern("unsupported"));
}
#endif

void
Init_foreach(void)
{
    VALUE bug = rb_define_module("Bug");
    rb_define_singleton_method(bug, "unp_st_foreach_check", unp_fec, 1);
    rb_define_singleton_method(bug, "unp_st_foreach", unp_fe, 1);
    rb_define_singleton_method(bug, "st_foreach_keeps_stored_hash", foreach_keeps_stored_hash, 0);
}
