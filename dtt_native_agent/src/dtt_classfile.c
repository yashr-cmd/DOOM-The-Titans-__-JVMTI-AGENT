#include "../include/dtt_classfile.h"
#include <stdlib.h>
#include <string.h>

/* ============================== byte helpers ============================== */

static unsigned short rd_u2(const unsigned char *p) {
    return (unsigned short)(((unsigned int)p[0] << 8) | (unsigned int)p[1]);
}
static unsigned int rd_u4(const unsigned char *p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

typedef struct { unsigned char *buf; jint len; jint cap; } outbuf_t;

static int ob_init(outbuf_t *ob) {
    ob->cap = 8192;
    ob->len = 0;
    ob->buf = (unsigned char *)malloc((size_t)ob->cap);
    return ob->buf != NULL;
}
static void ob_free(outbuf_t *ob) {
    free(ob->buf);
    ob->buf = NULL; ob->len = 0; ob->cap = 0;
}
static int ob_ensure(outbuf_t *ob, jint extra) {
    if (ob->len + extra <= ob->cap) return 1;
    jint ncap = ob->cap * 2;
    while (ncap < ob->len + extra) ncap *= 2;
    unsigned char *nb = (unsigned char *)realloc(ob->buf, (size_t)ncap);
    if (!nb) return 0;
    ob->buf = nb; ob->cap = ncap;
    return 1;
}
static int ob_put(outbuf_t *ob, const unsigned char *data, jint n) {
    if (n <= 0) return 1;
    if (!ob_ensure(ob, n)) return 0;
    memcpy(ob->buf + ob->len, data, (size_t)n);
    ob->len += n;
    return 1;
}
static int ob_u1(outbuf_t *ob, unsigned char v) { return ob_put(ob, &v, 1); }
static int ob_u2(outbuf_t *ob, unsigned short v) {
    unsigned char b[2]; b[0] = (unsigned char)(v >> 8); b[1] = (unsigned char)v;
    return ob_put(ob, b, 2);
}
static int ob_u4(outbuf_t *ob, unsigned int v) {
    unsigned char b[4];
    b[0] = (unsigned char)(v >> 24); b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);  b[3] = (unsigned char)v;
    return ob_put(ob, b, 4);
}

/* ============================== constant pool ============================== */

typedef struct { unsigned char tag; jint info_off; jint info_len; } cp_entry_t;
typedef struct { cp_entry_t *entries; int count; jint pool_start; jint pool_end; } cp_table_t;

static int cp_parse(const unsigned char *data, jint len, jint *cursor, cp_table_t *cp) {
    unsigned short cp_count;
    int i;

    if (*cursor + 2 > len) return 0;
    cp_count = rd_u2(data + *cursor);
    cp->pool_start = *cursor;
    *cursor += 2;
    cp->count = cp_count;
    cp->entries = (cp_entry_t *)calloc((size_t)cp_count, sizeof(cp_entry_t));
    if (!cp->entries) return 0;

    i = 1;
    while (i < cp_count) {
        unsigned char tag;
        jint info_off, info_len;

        if (*cursor + 1 > len) { free(cp->entries); return 0; }
        tag = data[*cursor];
        info_off = *cursor + 1;

        switch (tag) {
            case 1: { /* Utf8 */
                unsigned short slen;
                if (info_off + 2 > len) { free(cp->entries); return 0; }
                slen = rd_u2(data + info_off);
                info_len = 2 + slen;
                break;
            }
            case 7: case 8: case 16: case 19: case 20: info_len = 2; break; /* Class,String,MethodType,Module,Package */
            case 15: info_len = 3; break;                                   /* MethodHandle */
            case 3: case 4: info_len = 4; break;                            /* Integer,Float */
            case 9: case 10: case 11: case 12: case 17: case 18: info_len = 4; break; /* xref,NameAndType,Dynamic,InvokeDynamic */
            case 5: case 6: info_len = 8; break;                            /* Long,Double */
            default:
                free(cp->entries);
                return 0; /* unknown tag: bail rather than guess */
        }

        if (info_off + info_len > len) { free(cp->entries); return 0; }
        cp->entries[i].tag = tag;
        cp->entries[i].info_off = info_off;
        cp->entries[i].info_len = info_len;
        *cursor = info_off + info_len;
        i += (tag == 5 || tag == 6) ? 2 : 1;
    }
    cp->pool_end = *cursor;
    return 1;
}

static int cp_utf8_eq(const unsigned char *data, const cp_table_t *cp, int idx, const char *s) {
    unsigned short slen; size_t s_len;
    if (idx <= 0 || idx >= cp->count || cp->entries[idx].tag != 1) return 0;
    slen = rd_u2(data + cp->entries[idx].info_off);
    s_len = strlen(s);
    if (slen != s_len) return 0;
    return memcmp(data + cp->entries[idx].info_off + 2, s, s_len) == 0;
}
static int cp_find_utf8(const unsigned char *data, const cp_table_t *cp, const char *s) {
    int i;
    for (i = 1; i < cp->count; i++) if (cp->entries[i].tag == 1 && cp_utf8_eq(data, cp, i, s)) return i;
    return -1;
}
static int cp_find_class(const unsigned char *data, const cp_table_t *cp, int name_utf8_idx) {
    int i;
    for (i = 1; i < cp->count; i++)
        if (cp->entries[i].tag == 7 && rd_u2(data + cp->entries[i].info_off) == (unsigned short)name_utf8_idx) return i;
    return -1;
}
static int cp_find_nat(const unsigned char *data, const cp_table_t *cp, int name_idx, int desc_idx) {
    int i;
    for (i = 1; i < cp->count; i++)
        if (cp->entries[i].tag == 12 &&
            rd_u2(data + cp->entries[i].info_off) == (unsigned short)name_idx &&
            rd_u2(data + cp->entries[i].info_off + 2) == (unsigned short)desc_idx) return i;
    return -1;
}
static int cp_find_methodref(const unsigned char *data, const cp_table_t *cp, int class_idx, int nt_idx) {
    int i;
    for (i = 1; i < cp->count; i++)
        if (cp->entries[i].tag == 10 &&
            rd_u2(data + cp->entries[i].info_off) == (unsigned short)class_idx &&
            rd_u2(data + cp->entries[i].info_off + 2) == (unsigned short)nt_idx) return i;
    return -1;
}

/* incrementally-extended pool: appends new Utf8/Class/NameAndType/Methodref
 * entries after the original pool. Resolves against original pool first so
 * we never duplicate an entry that's already present. */
#define CPX_MAX 16
typedef struct {
    const char *utf8_strs[CPX_MAX]; int utf8_idx[CPX_MAX]; int utf8_count;
    struct { unsigned char tag; unsigned short f1; unsigned short f2; int resolved; } items[CPX_MAX];
    int item_count;
    int next_index;
} cp_extend_t;

static void cpx_init(cp_extend_t *ex, int original_count) {
    ex->utf8_count = 0; ex->item_count = 0; ex->next_index = original_count;
}
static int cpx_add_utf8(cp_extend_t *ex, const unsigned char *data, const cp_table_t *cp, const char *s) {
    int existing = cp_find_utf8(data, cp, s);
    int i, idx;
    if (existing > 0) return existing;
    for (i = 0; i < ex->utf8_count; i++) if (strcmp(ex->utf8_strs[i], s) == 0) return ex->utf8_idx[i];
    idx = ex->next_index++;
    ex->utf8_strs[ex->utf8_count] = s;
    ex->utf8_idx[ex->utf8_count] = idx;
    ex->utf8_count++;
    return idx;
}
static int cpx_add_class_by_name(cp_extend_t *ex, const unsigned char *data, const cp_table_t *cp, const char *internal_name) {
    int name_idx = cpx_add_utf8(ex, data, cp, internal_name);
    int existing = cp_find_class(data, cp, name_idx);
    int i, idx;
    if (existing > 0) return existing;
    for (i = 0; i < ex->item_count; i++)
        if (ex->items[i].tag == 7 && ex->items[i].f1 == (unsigned short)name_idx) return ex->items[i].resolved;
    idx = ex->next_index++;
    ex->items[ex->item_count].tag = 7;
    ex->items[ex->item_count].f1 = (unsigned short)name_idx;
    ex->items[ex->item_count].resolved = idx;
    ex->item_count++;
    return idx;
}
static int cpx_add_nat(cp_extend_t *ex, const unsigned char *data, const cp_table_t *cp, const char *name, const char *desc) {
    int name_idx = cpx_add_utf8(ex, data, cp, name);
    int desc_idx = cpx_add_utf8(ex, data, cp, desc);
    int existing = cp_find_nat(data, cp, name_idx, desc_idx);
    int i, idx;
    if (existing > 0) return existing;
    for (i = 0; i < ex->item_count; i++)
        if (ex->items[i].tag == 12 && ex->items[i].f1 == (unsigned short)name_idx && ex->items[i].f2 == (unsigned short)desc_idx)
            return ex->items[i].resolved;
    idx = ex->next_index++;
    ex->items[ex->item_count].tag = 12;
    ex->items[ex->item_count].f1 = (unsigned short)name_idx;
    ex->items[ex->item_count].f2 = (unsigned short)desc_idx;
    ex->items[ex->item_count].resolved = idx;
    ex->item_count++;
    return idx;
}
static int cpx_add_methodref(cp_extend_t *ex, const unsigned char *data, const cp_table_t *cp,
                              const char *owner, const char *name, const char *desc) {
    int class_idx = cpx_add_class_by_name(ex, data, cp, owner);
    int nat_idx = cpx_add_nat(ex, data, cp, name, desc);
    int existing = cp_find_methodref(data, cp, class_idx, nat_idx);
    int i, idx;
    if (existing > 0) return existing;
    for (i = 0; i < ex->item_count; i++)
        if (ex->items[i].tag == 10 && ex->items[i].f1 == (unsigned short)class_idx && ex->items[i].f2 == (unsigned short)nat_idx)
            return ex->items[i].resolved;
    idx = ex->next_index++;
    ex->items[ex->item_count].tag = 10;
    ex->items[ex->item_count].f1 = (unsigned short)class_idx;
    ex->items[ex->item_count].f2 = (unsigned short)nat_idx;
    ex->items[ex->item_count].resolved = idx;
    ex->item_count++;
    return idx;
}
static int cpx_emit(cp_extend_t *ex, outbuf_t *ob) {
    int i;
    for (i = 0; i < ex->utf8_count; i++) {
        size_t slen = strlen(ex->utf8_strs[i]);
        if (!ob_u1(ob, 1)) return 0;
        if (!ob_u2(ob, (unsigned short)slen)) return 0;
        if (!ob_put(ob, (const unsigned char *)ex->utf8_strs[i], (jint)slen)) return 0;
    }
    for (i = 0; i < ex->item_count; i++) {
        if (!ob_u1(ob, ex->items[i].tag)) return 0;
        if (!ob_u2(ob, ex->items[i].f1)) return 0;
        if (ex->items[i].tag != 7) { /* Class only has 1 field; NameAndType/Methodref have 2 */
            if (!ob_u2(ob, ex->items[i].f2)) return 0;
        }
    }
    return 1;
}
static int cpx_total_new_entries(const cp_extend_t *ex) { return ex->utf8_count + ex->item_count; }

/* ============================== descriptor -> verification_type_info ============================== */

/* Returns bytes consumed from desc, or 0 on parse failure. Writes one
 * verification_type_info entry (variable length) for the parameter/local
 * starting at *desc into ob. class_this_idx is used only by the caller for
 * the implicit 'this' slot, not from here. */
static int emit_vti_for_field_type(outbuf_t *ob, const char *desc, cp_extend_t *ex,
                                    const unsigned char *data, const cp_table_t *cp) {
    switch (desc[0]) {
        case 'B': case 'C': case 'S': case 'Z': case 'I':
            return ob_u1(ob, 1) ? 1 : 0; /* Integer */
        case 'F':
            return ob_u1(ob, 2) ? 1 : 0; /* Float */
        case 'D':
            return ob_u1(ob, 3) ? 1 : 0; /* Double */
        case 'J':
            return ob_u1(ob, 4) ? 1 : 0; /* Long */
        case 'L': {
            const char *semi = strchr(desc, ';');
            char name[256];
            size_t nlen;
            int class_idx;
            if (!semi) return 0;
            nlen = (size_t)(semi - (desc + 1));
            if (nlen >= sizeof(name)) return 0;
            memcpy(name, desc + 1, nlen);
            name[nlen] = '\0';
            class_idx = cpx_add_class_by_name(ex, data, cp, name);
            if (!ob_u1(ob, 7)) return 0;
            if (!ob_u2(ob, (unsigned short)class_idx)) return 0;
            return (int)(semi - desc) + 1;
        }
        case '[': {
            const char *p = desc;
            int class_idx;
            while (*p == '[') p++;
            if (*p == 'L') { const char *semi = strchr(p, ';'); if (!semi) return 0; p = semi; }
            /* p now points at the last char of the element type descriptor */
            {
                size_t full_len = (size_t)(p - desc) + 1;
                char arr_name[256];
                if (full_len >= sizeof(arr_name)) return 0;
                memcpy(arr_name, desc, full_len);
                arr_name[full_len] = '\0';
                class_idx = cpx_add_class_by_name(ex, data, cp, arr_name);
                if (!ob_u1(ob, 7)) return 0;
                if (!ob_u2(ob, (unsigned short)class_idx)) return 0;
                return (int)full_len;
            }
        }
        default:
            return 0;
    }
}

/* Builds the FULL_FRAME locals list (this + params) for a method, writing
 * verification_type_info entries directly to ob. Returns number of locals
 * written, or -1 on failure. */
static int build_full_frame_locals(outbuf_t *ob, const char *method_desc, int is_static,
                                    int this_class_idx, cp_extend_t *ex,
                                    const unsigned char *data, const cp_table_t *cp) {
    int count = 0;
    const char *p = method_desc;
    if (*p != '(') return -1;
    p++;

    if (!is_static) {
        if (!ob_u1(ob, 7)) return -1;
        if (!ob_u2(ob, (unsigned short)this_class_idx)) return -1;
        count++;
    }
    while (*p != ')') {
        int consumed = emit_vti_for_field_type(ob, p, ex, data, cp);
        if (consumed <= 0) return -1;
        p += consumed;
        count++;
    }
    return count;
}

/* ============================== StackMapTable frame helpers ============================== */

static int vti_entry_len(const unsigned char *p) {
    unsigned char tag = p[0];
    if (tag == 7 || tag == 8) return 3;
    return 1;
}

/* Absolute bytecode offset of the first StackMapTable frame (sum from offset 0). */
static int stackmap_first_frame_absolute_offset(const unsigned char *src, jint src_len) {
    unsigned char frame_type;
    if (src_len < 1) return -1;
    frame_type = src[0];

    if (frame_type <= 63) return (int)frame_type;
    if (frame_type >= 64 && frame_type <= 127) return (int)frame_type - 64;
    if (frame_type == 247 || (frame_type >= 248 && frame_type <= 254) || frame_type == 255) {
        if (src_len < 3) return -1;
        return (int)rd_u2(src + 1);
    }
    return -1;
}

/* Returns the byte length of one stack_map_frame entry, or -1 on failure. */
static int stackmap_frame_byte_len(const unsigned char *src, jint src_len) {
    unsigned char frame_type;
    int i, off, n_append, n_locals, n_stack;

    if (src_len < 1) return -1;
    frame_type = src[0];

    if (frame_type <= 63) return 1;
    if (frame_type >= 64 && frame_type <= 127) {
        int vlen = vti_entry_len(src + 1);
        if (src_len < 1 + vlen) return -1;
        return 1 + vlen;
    }
    if (frame_type == 247) {
        int vlen;
        if (src_len < 3) return -1;
        vlen = vti_entry_len(src + 3);
        if (src_len < 3 + vlen) return -1;
        return 3 + vlen;
    }
    if (frame_type >= 248 && frame_type <= 251) return 3;
    if (frame_type >= 252 && frame_type <= 254) {
        if (src_len < 3) return -1;
        n_append = frame_type - 251;
        off = 3;
        for (i = 0; i < n_append; i++) {
            int vlen;
            if (src_len < off + 1) return -1;
            vlen = vti_entry_len(src + off);
            if (src_len < off + vlen) return -1;
            off += vlen;
        }
        return off;
    }
    if (frame_type == 255) {
        if (src_len < 5) return -1;
        n_locals = (int)rd_u2(src + 3);
        off = 5;
        for (i = 0; i < n_locals; i++) {
            int vlen;
            if (src_len < off + 1) return -1;
            vlen = vti_entry_len(src + off);
            if (src_len < off + vlen) return -1;
            off += vlen;
        }
        if (src_len < off + 2) return -1;
        n_stack = (int)rd_u2(src + off);
        off += 2;
        for (i = 0; i < n_stack; i++) {
            int vlen;
            if (src_len < off + 1) return -1;
            vlen = vti_entry_len(src + off);
            if (src_len < off + vlen) return -1;
            off += vlen;
        }
        return off;
    }
    return -1;
}

/* ============================== generic skip helpers ============================== */

static int skip_attribute(const unsigned char *data, jint len, jint *cursor) {
    unsigned int alen;
    if (*cursor + 6 > len) return 0;
    alen = rd_u4(data + *cursor + 2);
    *cursor += 6 + (jint)alen;
    return *cursor <= len;
}
static int skip_attributes(const unsigned char *data, jint len, jint *cursor) {
    unsigned short count; int i;
    if (*cursor + 2 > len) return 0;
    count = rd_u2(data + *cursor);
    *cursor += 2;
    for (i = 0; i < count; i++) if (!skip_attribute(data, len, cursor)) return 0;
    return 1;
}
static int skip_field_or_method(const unsigned char *data, jint len, jint *cursor) {
    if (*cursor + 8 > len) return 0;
    *cursor += 6; /* access_flags, name_index, descriptor_index */
    return skip_attributes(data, len, cursor);
}

/* ============================== main entry point ============================== */

dtt_cf_result_t dtt_cf_inject_guard(const unsigned char *data, jint len,
                                     const char *method_name, const char *method_desc,
                                     const char *guard_owner, const char *guard_name,
                                     const char *guard_desc, dtt_guard_style_t style) {
    dtt_cf_result_t fail = { NULL, 0 };
    jint cursor = 0;
    cp_table_t cp;
    unsigned short access_flags, this_class_idx, super_class_idx, interfaces_count;
    unsigned short fields_count, methods_count;
    jint fields_start, fields_end;
    jint methods_start;
    int method_idx_target = -1;
    jint method_start = -1, method_end = -1;
    int i;
    unsigned short m_access, m_name_idx, m_desc_idx, m_attr_count;
    jint code_attr_off = -1, code_attr_len = -1; /* covers the whole attribute incl. 6-byte header */
    outbuf_t out;
    cp_extend_t ex;

    if (data == NULL || len < 10) return fail;

    /* header: magic(4) minor(2) major(2) */
    cursor = 8;
    if (!cp_parse(data, len, &cursor, &cp)) return fail;

    if (cursor + 8 > len) { free(cp.entries); return fail; }
    access_flags = rd_u2(data + cursor); (void)access_flags;
    this_class_idx = rd_u2(data + cursor + 2);
    super_class_idx = rd_u2(data + cursor + 4); (void)super_class_idx;
    interfaces_count = rd_u2(data + cursor + 6);
    cursor += 8 + (jint)interfaces_count * 2;
    if (cursor > len) { free(cp.entries); return fail; }

    if (cursor + 2 > len) { free(cp.entries); return fail; }
    fields_count = rd_u2(data + cursor);
    fields_start = cursor;
    cursor += 2;
    for (i = 0; i < fields_count; i++) if (!skip_field_or_method(data, len, &cursor)) { free(cp.entries); return fail; }
    fields_end = cursor;

    if (cursor + 2 > len) { free(cp.entries); return fail; }
    methods_count = rd_u2(data + cursor);
    methods_start = cursor;
    cursor += 2;

    for (i = 0; i < methods_count; i++) {
        jint m_start = cursor;
        int is_target;
        if (cursor + 8 > len) { free(cp.entries); return fail; }
        m_access = rd_u2(data + cursor);
        m_name_idx = rd_u2(data + cursor + 2);
        m_desc_idx = rd_u2(data + cursor + 4);
        is_target = cp_utf8_eq(data, &cp, m_name_idx, method_name) && cp_utf8_eq(data, &cp, m_desc_idx, method_desc);

        {
            jint acursor = cursor + 6;
            if (acursor + 2 > len) { free(cp.entries); return fail; }
            m_attr_count = rd_u2(data + acursor);
            acursor += 2;
            {
                int a;
                for (a = 0; a < m_attr_count; a++) {
                    jint attr_start = acursor;
                    unsigned short name_idx;
                    unsigned int alen;
                    if (acursor + 6 > len) { free(cp.entries); return fail; }
                    name_idx = rd_u2(data + acursor);
                    alen = rd_u4(data + acursor + 2);
                    if (is_target && code_attr_off < 0 && cp_utf8_eq(data, &cp, name_idx, "Code")) {
                        code_attr_off = attr_start;
                        code_attr_len = 6 + (jint)alen;
                    }
                    acursor += 6 + (jint)alen;
                    if (acursor > len) { free(cp.entries); return fail; }
                }
            }
            cursor = acursor;
        }

        if (is_target) { method_idx_target = i; method_start = m_start; method_end = cursor; }
        if (cursor > len) { free(cp.entries); return fail; }
    }

    if (method_idx_target < 0 || code_attr_off < 0) { free(cp.entries); return fail; } /* method or Code attr not found */

    /* ---- parse the Code attribute body ---- */
    {
        jint p = code_attr_off + 6; /* skip name_index(2)+length(4) */
        unsigned short max_stack, max_locals;
        unsigned int code_length;
        jint code_off;
        unsigned short exc_table_count;
        jint exc_table_off;
        unsigned short code_attrs_count;
        jint code_attrs_off;
        int already_patched = 0;

        if (p + 8 > len) { free(cp.entries); return fail; }
        max_stack = rd_u2(data + p);
        max_locals = rd_u2(data + p + 2);
        code_length = rd_u4(data + p + 4);
        code_off = p + 8;
        if (code_off + (jint)code_length > len) { free(cp.entries); return fail; }

        {
            jint q = code_off + (jint)code_length;
            if (q + 2 > len) { free(cp.entries); return fail; }
            exc_table_count = rd_u2(data + q);
            exc_table_off = q + 2;
            q = exc_table_off + (jint)exc_table_count * 8;
            if (q + 2 > len) { free(cp.entries); return fail; }
            code_attrs_count = rd_u2(data + q);
            code_attrs_off = q + 2;

            /* idempotency: look for our marker attribute among Code's sub-attributes */
            {
                jint r = code_attrs_off;
                int a;
                for (a = 0; a < code_attrs_count; a++) {
                    unsigned short nidx; unsigned int al;
                    if (r + 6 > len) { free(cp.entries); return fail; }
                    nidx = rd_u2(data + r);
                    al = rd_u4(data + r + 2);
                    if (cp_utf8_eq(data, &cp, nidx, "DTTGuarded")) already_patched = 1;
                    r += 6 + (jint)al;
                }
            }
            if (already_patched) { free(cp.entries); return fail; } /* nothing to do, don't double-inject */

            /* Also detect a prior guard prologue prepended to the bytecode.
             * ClassFileLoadHook can run again on already-transformed bytes when
             * the DTTGuarded marker's Utf8 index isn't visible to our parser. */
            if (code_length >= 5 &&
                data[code_off] == 0x2A && data[code_off + 1] == 0xB8) {
                free(cp.entries);
                return fail;
            }
        }

        /* locate (if present) the StackMapTable sub-attribute of Code */
        {
            jint smt_off = -1, smt_len = -1; /* smt_len = length field value only (not incl. 6-byte hdr) */
            jint r = code_attrs_off;
            int a;
            for (a = 0; a < code_attrs_count; a++) {
                unsigned short nidx; unsigned int al;
                if (r + 6 > len) { free(cp.entries); return fail; }
                nidx = rd_u2(data + r);
                al = rd_u4(data + r + 2);
                if (cp_utf8_eq(data, &cp, nidx, "StackMapTable")) { smt_off = r; smt_len = (jint)al; }
                r += 6 + (jint)al;
            }

            /* ---- build the guard prologue bytecode + compute N ---- */
            {
                int is_static = (m_access & 0x0008) != 0;
                unsigned char prologue[16];
                int n = 0;
                jint guard_mref;

                cpx_init(&ex, cp.count);

                if (style == DTT_GUARD_ENTITY_DATA_SET) {
                    guard_mref = cpx_add_methodref(&ex, data, &cp, guard_owner, guard_name, guard_desc);
                    prologue[n++] = 0x2A; /* ALOAD_0 */
                    prologue[n++] = 0x2B; /* ALOAD_1 */
                    prologue[n++] = 0x2C; /* ALOAD_2 */
                    prologue[n++] = 0xB8; prologue[n++] = (unsigned char)(guard_mref >> 8); prologue[n++] = (unsigned char)guard_mref; /* INVOKESTATIC */
                    prologue[n++] = 0x4D; /* ASTORE_2 */
                } else {
                    guard_mref = cpx_add_methodref(&ex, data, &cp, guard_owner, guard_name, guard_desc);
                    prologue[n++] = 0x2A; /* ALOAD_0 */
                    prologue[n++] = 0xB8; prologue[n++] = (unsigned char)(guard_mref >> 8); prologue[n++] = (unsigned char)guard_mref; /* INVOKESTATIC */
                    {
                        int ifeq_pos = n;
                        short delta;
                        prologue[n++] = 0x99; /* IFEQ */
                        prologue[n++] = 0; prologue[n++] = 0; /* placeholder */
                        if (style == DTT_GUARD_CANCEL_BOOLEAN) {
                            prologue[n++] = 0x03; /* ICONST_0 */
                            prologue[n++] = 0xAC; /* IRETURN */
                        } else {
                            prologue[n++] = 0xB1; /* RETURN */
                        }
                        delta = (short)(n - ifeq_pos);
                        prologue[ifeq_pos + 1] = (unsigned char)((delta >> 8) & 0xFF);
                        prologue[ifeq_pos + 2] = (unsigned char)(delta & 0xFF);
                    }
                }

                if ((jint)code_length + n >= 65536) { free(cp.entries); return fail; }

                /* ---- assemble the new Code attribute body ---- */
                if (!ob_init(&out)) { free(cp.entries); return fail; }

                if (!ob_u2(&out, max_stack < 4 ? 4 : max_stack)) goto bail; /* guard needs a little headroom */
                if (!ob_u2(&out, max_locals)) goto bail;
                if (!ob_u4(&out, (unsigned int)code_length + (unsigned int)n)) goto bail;
                if (!ob_put(&out, prologue, n)) goto bail;
                if (!ob_put(&out, data + code_off, (jint)code_length)) goto bail;

                if (!ob_u2(&out, exc_table_count)) goto bail;
                {
                    int e;
                    for (e = 0; e < exc_table_count; e++) {
                        const unsigned char *ent = data + exc_table_off + e * 8;
                        unsigned short start_pc = rd_u2(ent), end_pc = rd_u2(ent + 2);
                        unsigned short handler_pc = rd_u2(ent + 4), catch_type = rd_u2(ent + 6);
                        if (!ob_u2(&out, (unsigned short)(start_pc + n))) goto bail;
                        if (!ob_u2(&out, (unsigned short)(end_pc + n))) goto bail;
                        if (!ob_u2(&out, (unsigned short)(handler_pc + n))) goto bail;
                        if (!ob_u2(&out, catch_type)) goto bail;
                    }
                }

                /* code_attrs_count stays the same UNLESS we synthesize a brand new
                 * StackMapTable (when none existed before) or add our marker - both
                 * of which we always add, so +1 (marker) [+1 if smt was absent]. */
                {
                    unsigned short new_attrs_count = code_attrs_count + 1 /* DTTGuarded marker */
                                                      + (unsigned short)(smt_off < 0 ? 1 : 0);
                    jint attrs_count_pos = out.len;
                    if (!ob_u2(&out, new_attrs_count)) goto bail;
                    (void)attrs_count_pos;
                }

                {
                    jint r = code_attrs_off;
                    int a;
                    for (a = 0; a < code_attrs_count; a++) {
                        unsigned short nidx; unsigned int al;
                        nidx = rd_u2(data + r);
                        al = rd_u4(data + r + 2);

                        if (cp_utf8_eq(data, &cp, nidx, "LineNumberTable")) {
                            unsigned short entry_count = rd_u2(data + r + 6);
                            int e;
                            if (!ob_u2(&out, nidx)) goto bail;
                            if (!ob_u4(&out, al)) goto bail;
                            if (!ob_u2(&out, entry_count)) goto bail;
                            for (e = 0; e < entry_count; e++) {
                                const unsigned char *ent = data + r + 8 + e * 4;
                                unsigned short start_pc = rd_u2(ent), line = rd_u2(ent + 2);
                                if (!ob_u2(&out, (unsigned short)(start_pc + n))) goto bail;
                                if (!ob_u2(&out, line)) goto bail;
                            }
                        } else if (cp_utf8_eq(data, &cp, nidx, "LocalVariableTable") ||
                                   cp_utf8_eq(data, &cp, nidx, "LocalVariableTypeTable")) {
                            unsigned short entry_count = rd_u2(data + r + 6);
                            int e;
                            if (!ob_u2(&out, nidx)) goto bail;
                            if (!ob_u4(&out, al)) goto bail;
                            if (!ob_u2(&out, entry_count)) goto bail;
                            for (e = 0; e < entry_count; e++) {
                                const unsigned char *ent = data + r + 8 + e * 10;
                                unsigned short start_pc = rd_u2(ent), plen = rd_u2(ent + 2);
                                unsigned short nn = rd_u2(ent + 4), dd = rd_u2(ent + 6), slot = rd_u2(ent + 8);
                                if (!ob_u2(&out, (unsigned short)(start_pc + n))) goto bail;
                                if (!ob_u2(&out, plen)) goto bail;
                                if (!ob_u2(&out, nn)) goto bail;
                                if (!ob_u2(&out, dd)) goto bail;
                                if (!ob_u2(&out, slot)) goto bail;
                            }
                        } else if (smt_off >= 0 && r == smt_off) {
                            /* rebuild StackMapTable: prepend our own FULL_FRAME entry
                             * at offset_delta=n, then the (possibly re-encoded) rest. */
                            jint body_start_pos = out.len; /* remember to patch attribute length after */
                            unsigned short old_count = rd_u2(data + smt_off + 6);
                            jint src = smt_off + 8;
                            jint src_end = smt_off + 6 + smt_len;

                            if (!ob_u2(&out, nidx)) goto bail;
                            if (!ob_u4(&out, 0)) goto bail; /* placeholder length, patched below */
                            if (!ob_u2(&out, (unsigned short)(old_count + 1))) goto bail;

                            /* our leading frame */
                            if (!ob_u1(&out, 255)) goto bail; /* FULL_FRAME */
                            if (!ob_u2(&out, (unsigned short)n)) goto bail; /* first entry: real_offset == offset_delta */
                            {
                                jint locals_count_pos = out.len;
                                int nlocals;
                                if (!ob_u2(&out, 0)) goto bail; /* placeholder */
                                nlocals = build_full_frame_locals(&out, method_desc, is_static,
                                                                   this_class_idx, &ex, data, &cp);
                                if (nlocals < 0) { ob_free(&out); free(cp.entries); return fail; }
                                out.buf[locals_count_pos] = (unsigned char)(nlocals >> 8);
                                out.buf[locals_count_pos + 1] = (unsigned char)nlocals;
                            }
                            if (!ob_u2(&out, 0)) goto bail; /* number_of_stack_items = 0 */

                            if (old_count > 0) {
                                int o0 = stackmap_first_frame_absolute_offset(
                                    data + src, (jint)(src_end - src));
                                int consumed = stackmap_frame_byte_len(
                                    data + src, (jint)(src_end - src));
                                if (o0 < 0 || consumed < 0) {
                                    ob_free(&out);
                                    free(cp.entries);
                                    return fail;
                                }
                                /* Our leading FULL_FRAME at offset n already describes
                                 * the frame at the start of the original code. If the
                                 * original table's first frame was also at offset 0,
                                 * skip it to avoid two conflicting frames at the same
                                 * bytecode index. Otherwise keep the original first
                                 * frame's offset_delta unchanged: after prepending n
                                 * bytes, its absolute offset becomes o0+n, so the
                                 * delta relative to our frame at n is still o0. */
                                if (o0 != 0) {
                                    if (!ob_put(&out, data + src, consumed)) goto bail;
                                }
                                src += consumed;
                                if (!ob_put(&out, data + src, (jint)(src_end - src))) goto bail;
                            }

                            {
                                unsigned int new_len = (unsigned int)(out.len - (body_start_pos + 6));
                                jint lp = body_start_pos + 2;
                                out.buf[lp]     = (unsigned char)(new_len >> 24);
                                out.buf[lp + 1] = (unsigned char)(new_len >> 16);
                                out.buf[lp + 2] = (unsigned char)(new_len >> 8);
                                out.buf[lp + 3] = (unsigned char)new_len;
                            }
                        } else {
                            /* unrecognized/irrelevant Code sub-attribute (e.g. Deprecated,
                             * RuntimeVisibleTypeAnnotations on locals): copy verbatim. Any
                             * bytecode-offset-bearing content we don't specifically handle
                             * here we intentionally don't claim support for - see header. */
                            if (!ob_put(&out, data + r, 6 + (jint)al)) goto bail;
                        }
                        r += 6 + (jint)al;
                    }
                }

                if (smt_off < 0) {
                    /* method had no StackMapTable at all - synthesize a minimal one
                     * with just our leading frame. */
                    int smt_name_idx = cpx_add_utf8(&ex, data, &cp, "StackMapTable");
                    jint body_start_pos = out.len;
                    if (!ob_u2(&out, (unsigned short)smt_name_idx)) goto bail;
                    if (!ob_u4(&out, 0)) goto bail; /* placeholder */
                    if (!ob_u2(&out, 1)) goto bail; /* number_of_entries */
                    if (!ob_u1(&out, 255)) goto bail;
                    if (!ob_u2(&out, (unsigned short)n)) goto bail;
                    {
                        jint locals_count_pos = out.len;
                        int nlocals;
                        if (!ob_u2(&out, 0)) goto bail;
                        nlocals = build_full_frame_locals(&out, method_desc, is_static,
                                                           this_class_idx, &ex, data, &cp);
                        if (nlocals < 0) { ob_free(&out); free(cp.entries); return fail; }
                        out.buf[locals_count_pos] = (unsigned char)(nlocals >> 8);
                        out.buf[locals_count_pos + 1] = (unsigned char)nlocals;
                    }
                    if (!ob_u2(&out, 0)) goto bail;
                    {
                        unsigned int new_len = (unsigned int)(out.len - (body_start_pos + 6));
                        jint lp = body_start_pos + 2;
                        out.buf[lp] = (unsigned char)(new_len >> 24);
                        out.buf[lp + 1] = (unsigned char)(new_len >> 16);
                        out.buf[lp + 2] = (unsigned char)(new_len >> 8);
                        out.buf[lp + 3] = (unsigned char)new_len;
                    }
                }

                /* our idempotency marker: a zero-length attribute the JVM ignores
                 * (JVMS 4.7.1: unrecognized attributes are silently skipped). */
                {
                    int marker_idx = cpx_add_utf8(&ex, data, &cp, "DTTGuarded");
                    if (!ob_u2(&out, (unsigned short)marker_idx)) goto bail;
                    if (!ob_u4(&out, 0)) goto bail;
                }

                /* ---- now stitch the whole class file together ---- */
                {
                    outbuf_t final_out;
                    int new_cp_count = cp.count + cpx_total_new_entries(&ex);
                    if (new_cp_count > 65535) goto bail;
                    if (!ob_init(&final_out)) goto bail;

                    if (!ob_put(&final_out, data, 8)) goto bail2;                       /* magic/minor/major */
                    if (!ob_u2(&final_out, (unsigned short)new_cp_count)) goto bail2;    /* new constant_pool_count */
                    if (!ob_put(&final_out, data + cp.pool_start + 2, cp.pool_end - (cp.pool_start + 2))) goto bail2; /* original pool entries */
                    if (!cpx_emit(&ex, &final_out)) goto bail2;                          /* appended pool entries */
                    if (!ob_put(&final_out, data + cp.pool_end, fields_start - cp.pool_end)) goto bail2; /* access_flags..interfaces */
                    if (!ob_put(&final_out, data + fields_start, fields_end - fields_start)) goto bail2; /* fields verbatim */
                    if (!ob_put(&final_out, data + fields_end, methods_start - fields_end)) goto bail2;  /* (nothing, contiguous) */
                    if (!ob_u2(&final_out, methods_count)) goto bail2;

                    {
                        jint mp = methods_start + 2;
                        int mi;
                        for (mi = 0; mi < methods_count; mi++) {
                            jint this_m_start = mp, this_m_end;
                            jint acursor2; unsigned short ac2;
                            acursor2 = mp + 6;
                            ac2 = rd_u2(data + acursor2);
                            acursor2 += 2;
                            {
                                int a;
                                for (a = 0; a < ac2; a++) {
                                    unsigned int al2 = rd_u4(data + acursor2 + 2);
                                    acursor2 += 6 + (jint)al2;
                                }
                            }
                            this_m_end = acursor2;

                            if (mi == method_idx_target) {
                                /* rewrite: access/name/desc verbatim, then this method's
                                 * OTHER attributes verbatim, but swap in our new Code body
                                 * for the Code attribute specifically. */
                                if (!ob_put(&final_out, data + this_m_start, 6)) goto bail2; /* access,name,desc */
                                {
                                    unsigned short new_m_attr_count = m_attr_count; /* Code count unchanged, only its body grew */
                                    if (!ob_u2(&final_out, new_m_attr_count)) goto bail2;
                                }
                                {
                                    jint r2 = this_m_start + 8;
                                    int a;
                                    for (a = 0; a < m_attr_count; a++) {
                                        unsigned short nidx2 = rd_u2(data + r2);
                                        unsigned int al2 = rd_u4(data + r2 + 2);
                                        if (r2 == code_attr_off) {
                                            unsigned int new_code_attr_len = (unsigned int)out.len;
                                            if (!ob_u2(&final_out, nidx2)) goto bail2;
                                            if (!ob_u4(&final_out, new_code_attr_len)) goto bail2;
                                            if (!ob_put(&final_out, out.buf, out.len)) goto bail2;
                                        } else {
                                            if (!ob_put(&final_out, data + r2, 6 + (jint)al2)) goto bail2;
                                        }
                                        r2 += 6 + (jint)al2;
                                    }
                                }
                            } else {
                                if (!ob_put(&final_out, data + this_m_start, this_m_end - this_m_start)) goto bail2;
                            }
                            mp = this_m_end;
                        }
                    }

                    /* remaining class-level attributes, verbatim, to end of file */
                    if (!ob_put(&final_out, data + cursor, len - cursor)) goto bail2;

                    ob_free(&out);
                    free(cp.entries);
                    {
                        dtt_cf_result_t ok;
                        ok.data = final_out.buf;
                        ok.len = final_out.len;
                        return ok;
                    }
bail2:
                    ob_free(&final_out);
                    goto bail;
                }
            }
        }
bail:
        ob_free(&out);
        free(cp.entries);
        return fail;
    }
}

void dtt_cf_result_free(dtt_cf_result_t *result) {
    if (result && result->data) {
        free(result->data);
        result->data = NULL;
        result->len = 0;
    }
}
