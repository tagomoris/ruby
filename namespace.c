/* indent-tabs-mode: nil */

#include "internal.h"
#include "internal/file.h"
#include "internal/gc.h"
#include "internal/hash.h"
#include "internal/load.h"
#include "internal/namespace.h"
#include "ruby/internal/globals.h"
#include "ruby/util.h"

#include <stdio.h>

VALUE rb_cNamespace;
VALUE rb_cNamespaceEntry;

static char *tmp_dir;

#define NAMESPACE_TMP_PREFIX "_ruby_ns_"

#ifndef MAXPATHLEN
# define MAXPATHLEN 1024
#endif

static int namespace_availability = 0;

int
rb_namespace_available()
{
    const char *env;
    if (namespace_availability) {
        return namespace_availability > 0 ? 1 : 0;
    }
    // TODO: command line option?
    env = getenv("RUBY_NAMESPACE");
    if (env && strlen(env) > 0) {
        if (strcmp(env, "1") == 0) {
            namespace_availability = 1;
            return 1;
        }
    }
    namespace_availability = -1;
    return 0;
}

static void
namespace_entry_initialize(rb_namespace_t *entry)
{
    entry->load_path = rb_ary_new();
    entry->expanded_load_path = rb_ary_hidden_new(0);
    entry->load_path_snapshot = rb_ary_hidden_new(0);
    entry->load_path_check_cache = 0;
    entry->loaded_features = rb_ary_new();
    entry->loaded_features_snapshot = rb_ary_hidden_new(0);
    entry->loaded_features_index = st_init_numtable();
    entry->loaded_features_realpaths = rb_hash_new();
    entry->loaded_features_realpath_map = rb_hash_new();
    entry->loading_table = st_init_strtable();
    entry->ruby_dln_libmap = rb_hash_new_with_size(0);

    // TODO: if $LOAD_PATH returns the load_path of the current namespace,
    //       all of them have to be responsible to the method .resolve_feature_path.
    // rb_define_singleton_method(ns->load_path, "resolve_feature_path", rb_resolve_feature_path, 1);

    // TODO: reason?
    // rb_obj_hide(entry->loaded_features_realpaths);
    // rb_obj_hide(entry->loaded_features_realpath_map);
}

void rb_namespace_gc_update_references(rb_namespace_t *ns)
{
    ns->load_path = rb_gc_location(ns->load_path);
    ns->expanded_load_path = rb_gc_location(ns->expanded_load_path);
    ns->load_path_snapshot = rb_gc_location(ns->load_path_snapshot);
    if (ns->load_path_check_cache) {
        ns->load_path_check_cache = rb_gc_location(ns->load_path_check_cache);
    }
    ns->loaded_features = rb_gc_location(ns->loaded_features);
    ns->loaded_features_snapshot = rb_gc_location(ns->loaded_features_snapshot);
    // TODO: rb_gc_update_tbl_refs(ns->loaded_features_index)
    ns->loaded_features_realpaths = rb_gc_location(ns->loaded_features_realpaths);
    ns->loaded_features_realpath_map = rb_gc_location(ns->loaded_features_realpath_map);
    ns->ruby_dln_libmap = rb_gc_location(ns->ruby_dln_libmap);
}

void
rb_namespace_entry_mark(void *ptr)
{
    const rb_namespace_t *entry = (rb_namespace_t *) ptr;
    rb_gc_mark(entry->load_path);
    rb_gc_mark(entry->expanded_load_path);
    rb_gc_mark(entry->load_path_snapshot);
    if (entry->load_path_check_cache) {
        rb_gc_mark(entry->load_path_check_cache);
    }
    rb_gc_mark(entry->loaded_features);
    rb_gc_mark(entry->loaded_features_snapshot);
    // rb_gc_mark(entry->loaded_features_index); // is st_table out of mark?
    rb_gc_mark(entry->loaded_features_realpaths);
    rb_gc_mark(entry->loaded_features_realpath_map);
    rb_gc_mark(entry->ruby_dln_libmap);
}

#define namespace_entry_free RUBY_TYPED_DEFAULT_FREE
// TODO: st_table members?

static size_t
namespace_entry_memsize(const void *ptr)
{
    return sizeof(rb_namespace_t);
}

const rb_data_type_t rb_namespace_data_type = {
    "Namespace::Entry",
    {
        rb_namespace_entry_mark,
        namespace_entry_free,
        namespace_entry_memsize,
    },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

VALUE
rb_namespace_entry_alloc(VALUE klass)
{
    rb_namespace_t *entry;
    VALUE obj = TypedData_Make_Struct(klass, rb_namespace_t, &rb_namespace_data_type, entry);
    namespace_entry_initialize(entry);
    return obj;
}

rb_namespace_t *
rb_namespace_alloc_init(void)
{
    rb_namespace_t *ns = ruby_mimmalloc(sizeof(*ns));
    namespace_entry_initialize(ns);
    return ns;
}

static rb_namespace_t *
get_namespace_struct_internal(VALUE entry)
{
    rb_namespace_t *sval;
    TypedData_Get_Struct(entry, rb_namespace_t, &rb_namespace_data_type, sval);
    return sval;
}

rb_namespace_t *
rb_get_namespace_t(VALUE namespace)
{
    VALUE entry = rb_ivar_get(namespace, rb_intern("@_namespace_entry"));
    return get_namespace_struct_internal(entry);
}

static VALUE
namespace_initialize(VALUE namespace)
{
    if (!rb_namespace_available()) {
        rb_warning("Namespace is disabled (RUBY_NAMESPACE is not set), so loading extensions may cause unexpected behaviors.");
    }
    VALUE entry = rb_class_new_instance_pass_kw(0, NULL, rb_cNamespaceEntry);
    rb_namespace_t *ns = get_namespace_struct_internal(entry);
    ns->ns_object = namespace;
    ns->ns_id = NUM2LONG(rb_obj_id(namespace));
    ns->load_path = GET_VM()->load_path; /* TODO: Should it be the load_path of the current namespace in namespaces? */
    rb_ivar_set(namespace, rb_intern("@_namespace_entry"), entry);
    return namespace;
}

static VALUE
rb_namespace_s_getenabled(VALUE namespace)
{
    return RBOOL(rb_namespace_available());
}

static VALUE
rb_namespace_s_setenabled(VALUE namespace, VALUE arg)
{
    switch (arg) {
    case Qnil:
        namespace_availability = 0; // reset the forced setting
        break;
    case Qfalse:
        namespace_availability = -1; // disable forcibly
        break;
    default:
        namespace_availability = 1; // enable namespaces
    }
    return arg;
}

static VALUE
rb_namespace_current(VALUE klass)
{
    rb_namespace_t *ns = GET_THREAD()->ns;
    if (ns) {
        return ns->ns_object;
    }
    return Qnil;
}

static VALUE
rb_namespace_load_path(VALUE namespace)
{
    return rb_get_namespace_t(namespace)->load_path;
}

#ifdef _WIN32
UINT rb_w32_system_tmpdir(WCHAR *path, UINT len);
#endif

/* Copied from mjit.c Ruby 3.0.3 */
static char *
system_default_tmpdir(void)
{
    // c.f. ext/etc/etc.c:etc_systmpdir()
#ifdef _WIN32
    WCHAR tmppath[_MAX_PATH];
    UINT len = rb_w32_system_tmpdir(tmppath, numberof(tmppath));
    if (len) {
        int blen = WideCharToMultiByte(CP_UTF8, 0, tmppath, len, NULL, 0, NULL, NULL);
        char *tmpdir = xmalloc(blen + 1);
        WideCharToMultiByte(CP_UTF8, 0, tmppath, len, tmpdir, blen, NULL, NULL);
        tmpdir[blen] = '\0';
        return tmpdir;
    }
#elif defined _CS_DARWIN_USER_TEMP_DIR
    char path[MAXPATHLEN];
    size_t len = confstr(_CS_DARWIN_USER_TEMP_DIR, path, sizeof(path));
    if (len > 0) {
        char *tmpdir = xmalloc(len);
        if (len > sizeof(path)) {
            confstr(_CS_DARWIN_USER_TEMP_DIR, tmpdir, len);
        }
        else {
            memcpy(tmpdir, path, len);
        }
        return tmpdir;
    }
#endif
    return 0;
}

static int
check_tmpdir(const char *dir)
{
    struct stat st;

    if (!dir) return FALSE;
    if (stat(dir, &st)) return FALSE;
#ifndef S_ISDIR
#   define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
    if (!S_ISDIR(st.st_mode)) return FALSE;
#ifndef _WIN32
# ifndef S_IWOTH
#   define S_IWOTH 002
# endif
    if (st.st_mode & S_IWOTH) {
# ifdef S_ISVTX
        if (!(st.st_mode & S_ISVTX)) return FALSE;
# else
        return FALSE;
# endif
    }
    if (access(dir, W_OK)) return FALSE;
#endif
    return TRUE;
}

static char *
system_tmpdir(void)
{
    char *tmpdir;
# define RETURN_ENV(name) \
    if (check_tmpdir(tmpdir = getenv(name))) return ruby_strdup(tmpdir)
    RETURN_ENV("TMPDIR");
    RETURN_ENV("TMP");
    tmpdir = system_default_tmpdir();
    if (check_tmpdir(tmpdir)) return tmpdir;
    return ruby_strdup("/tmp");
# undef RETURN_ENV
}

/* end of copy */

static int
sprint_ext_filename(char *str, size_t size, long namespace_id, const char *prefix, const char *basename)
{
    return snprintf(str, size, "%s/%sp%"PRI_PIDT_PREFIX"uu_%ld_%s", tmp_dir, prefix, getpid(), namespace_id, basename);
}

#ifdef _WIN32
static const char *
copy_ext_file_error(char *message, size_t size)
{
    int error = GetLastError();
    char *p = message;
    size_t len = snprintf(message, size, "%d: ", error);

#define format_message(sublang) FormatMessage(\
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,	\
        NULL, error, MAKELANGID(LANG_NEUTRAL, (sublang)),		\
        message + len, size - len, NULL)
    if (format_message(SUBLANG_ENGLISH_US) == 0)
        format_message(SUBLANG_DEFAULT);
    for (p = message + len; *p; p++) {
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    }
    return message;
}
#else
static const char *
copy_ext_file_error(char *message, size_t size, int copy_retvalue, char *src_path, char *dst_path)
{
    switch (copy_retvalue) {
    case 1:
        snprintf(message, size, "can't open the extension path: %s", src_path);
    case 2:
        snprintf(message, size, "can't open the file to write: %s", dst_path);
    case 3:
        snprintf(message, size, "failed to read the extension path: %s", src_path);
    case 4:
        snprintf(message, size, "failed to write the extension path: %s", dst_path);
    default:
        rb_bug("unkown return value of copy_ext_file: %d", copy_retvalue);
    }
    return message;
}
#endif

static int
copy_ext_file(char *src_path, char *dst_path)
{
#if defined(_WIN32)
    return CopyFileW(src_path, dst_path, FALSE) ? 0 : 1;
#else
    FILE *src, *dst;
    char buffer[1024];
    size_t read, wrote, written;
    size_t maxread = sizeof(buffer);
    int eof = 0;
    int clean_read = 1;
    int retvalue = 0;

    src = fopen(src_path, "rb");
    if (!src) {
        return 1;
    }
    dst = fopen(dst_path, "wb");
    if (!dst) {
        return 2;
    }
    while (!eof) {
        if (clean_read) {
            read = fread(buffer, 1, sizeof(buffer), src);
            written = 0;
        }
        if (read > 0) {
            wrote = fwrite(buffer+written, 1, read-written, dst);
            if (wrote < read-written) {
                if (ferror(dst)) {
                    retvalue = 4;
                    break;
                } else { // partial write
                    clean_read = 0;
                    written += wrote;
                }
            } else { // Wrote the entire buffer to dst, next read is clean one
                clean_read = 1;
            }
        }
        if (read < maxread) {
            if (clean_read && feof(src)) {
                // If it's not clean, buffer should have bytes not written yet.
                eof = 1;
            } else if (ferror(src)) {
                retvalue = 3;
                // Writes could be partial/dirty, but this load is failure anyway
                break;
            }
        }
    }
    fclose(src);
    fclose(dst);
    return retvalue;
#endif
}

VALUE
rb_namespace_local_extension(VALUE namespace, VALUE path)
{
    char ext_path[MAXPATHLEN];
    int copy_error;
    char *src_path = RSTRING_PTR(path);
    rb_namespace_t *ns = rb_get_namespace_t(namespace);
    VALUE basename = rb_funcall(rb_cFile, rb_intern("basename"), 1, path); // TODO: C impl

    int wrote = sprint_ext_filename(ext_path, sizeof(ext_path), ns->ns_id, NAMESPACE_TMP_PREFIX, RSTRING_PTR(basename));
    if (wrote >= (int)sizeof(ext_path)) {
        rb_bug("Extension file path in namespace was too long");
    }
    copy_error = copy_ext_file(src_path, ext_path);
    if (copy_error) {
        char message[1024];
#if defined(_WIN32)
        copy_ext_file_error(message, sizeof(message));
#else
        copy_ext_file_error(message, sizeof(message), copy_error, src_path, ext_path);
#endif
        rb_raise(rb_eLoadError, "can't load the extension in namespace: %s", message);
    }
    // TODO: register the path to be clean-uped
    return rb_str_new_cstr(ext_path);
}

// TODO: delete it just after dln_load? or delay it?
//       At least for _WIN32, deleting extension files should be delayed until the namespace's destructor.
//       And it requires calling dlclose before deleting it.

static void
namespace_push(rb_thread_t *th, VALUE namespace)
{
    if (th->namespaces) {
        rb_ary_push(th->namespaces, namespace);
    } else {
        th->namespaces = rb_ary_new_from_args(1, namespace);
    }
    th->ns = rb_get_namespace_t(namespace);
}

static VALUE
namespace_pop(VALUE th_value)
{
    VALUE upper_ns;
    long stack_len;
    rb_thread_t *th = (rb_thread_t *)th_value;
    VALUE namespaces = th->namespaces;
    if (!namespaces) {
        rb_bug("Too many namespace pops");
    }
    rb_ary_pop(namespaces);
    stack_len = RARRAY_LEN(namespaces);
    if (stack_len == 0) {
        th->namespaces = 0;
        th->ns = NULL;
    } else {
        upper_ns = RARRAY_AREF(namespaces, stack_len-1);
        th->ns = rb_get_namespace_t(upper_ns);
    }
    return Qnil;
}

static VALUE
rb_namespace_load(int argc, VALUE *argv, VALUE namespace)
{
    VALUE fname, wrap;
    rb_thread_t *th = GET_THREAD();

    rb_scan_args(argc, argv, "11", &fname, &wrap);

    VALUE args = rb_ary_new_from_args(2, fname, wrap);
    namespace_push(th, namespace);
    return rb_ensure(rb_load_entrypoint, args, namespace_pop, (VALUE) th);
}

static VALUE
rb_namespace_require(VALUE namespace, VALUE fname)
{
    rb_thread_t *th = GET_THREAD();
    namespace_push(th, namespace);
    return rb_ensure(rb_require_string, fname, namespace_pop, (VALUE) th);
}

static VALUE
rb_namespace_require_relative(VALUE namespace, VALUE fname)
{
    rb_thread_t *th = GET_THREAD();
    namespace_push(th, namespace);
    return rb_ensure(rb_require_relative_entrypoint, fname, namespace_pop, (VALUE) th);
}

static VALUE
rb_namespace_autoload(VALUE namespace, VALUE sym, VALUE file)
{
    return Qnil;
}

static VALUE
rb_namespace_autoload_p(int argc, VALUE *argv, VALUE namespace)
{
    return Qnil;
}

void
Init_Namespace(void)
{
    tmp_dir = system_tmpdir();

    rb_cNamespace = rb_define_class("Namespace", rb_cModule);
    rb_define_method(rb_cNamespace, "initialize", namespace_initialize, 0);

    rb_cNamespaceEntry = rb_define_class_under(rb_cNamespace, "Entry", rb_cObject);
    rb_define_alloc_func(rb_cNamespaceEntry, rb_namespace_entry_alloc);

    rb_define_singleton_method(rb_cNamespace, "enabled", rb_namespace_s_getenabled, 0);
    rb_define_singleton_method(rb_cNamespace, "enabled=", rb_namespace_s_setenabled, 1);
    rb_define_singleton_method(rb_cNamespace, "current", rb_namespace_current, 0);

    rb_define_method(rb_cNamespace, "load_path", rb_namespace_load_path, 0);
    rb_define_method(rb_cNamespace, "load", rb_namespace_load, -1);
    rb_define_method(rb_cNamespace, "require", rb_namespace_require, 1);
    rb_define_method(rb_cNamespace, "require_relative", rb_namespace_require_relative, 1);
    rb_define_method(rb_cNamespace, "autoload", rb_namespace_autoload, 2);
    rb_define_method(rb_cNamespace, "autoload?", rb_namespace_autoload_p, -1);

    // TODO: rb_define_singleton_method(vm->load_path, "resolve_feature_path", rb_resolve_feature_path, 1);
}

