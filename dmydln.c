#include "ruby/ruby.h"

NORETURN(void *dln_load(const char *));
void*
dln_load(const char *file)
{
    rb_loaderror("this executable file can't load extension libraries");

    UNREACHABLE_RETURN(NULL);
}

NORETURN(void *dln_load_in_namespace(const char *, const char *));
void*
dln_load_in_namespace(const char *file, const char *original)
{
    rb_loaderror("this executable file can't load extension libraries");

    UNREACHABLE_RETURN(NULL);
}
