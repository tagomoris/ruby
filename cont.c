/**********************************************************************

  cont.c -

  $Author$
  created at: Thu May 23 09:03:43 2007

  Copyright (C) 2007 Koichi Sasada

**********************************************************************/

#include "internal.h"
#include "vm_core.h"
#include "gc.h"
#include "eval_intern.h"
#include "mjit.h"

#ifdef FIBER_USE_COROUTINE
#include FIBER_USE_COROUTINE
#endif

#ifndef _WIN32
#include <unistd.h>
#include <sys/mman.h>
#endif

#define RB_PAGE_SIZE (pagesize)
#define RB_PAGE_MASK (~(RB_PAGE_SIZE - 1))
static long pagesize;

#define CAPTURE_JUST_VALID_VM_STACK 1

enum context_type {
    CONTINUATION_CONTEXT = 0,
    FIBER_CONTEXT = 1
};

struct cont_saved_vm_stack {
    VALUE *ptr;
#ifdef CAPTURE_JUST_VALID_VM_STACK
    size_t slen;  /* length of stack (head of ec->vm_stack) */
    size_t clen;  /* length of control frames (tail of ec->vm_stack) */
#endif
};

typedef struct rb_context_struct {
    enum context_type type;
    int argc;
    VALUE self;
    VALUE value;

    struct cont_saved_vm_stack saved_vm_stack;

    struct {
        VALUE *stack;
        VALUE *stack_src;
        size_t stack_size;
    } machine;
    rb_execution_context_t saved_ec;
    int free_vm_stack;
    rb_jmpbuf_t jmpbuf;
    rb_ensure_entry_t *ensure_array;
    /* Pointer to MJIT info about the continuation.  */
    struct mjit_cont *mjit_cont;
} rb_context_t;


/*
 * Fiber status:
 *    [Fiber.new] ------> FIBER_CREATED
 *                        | [Fiber#resume]
 *                        v
 *                   +--> FIBER_RESUMED ----+
 *    [Fiber#resume] |    | [Fiber.yield]   |
 *                   |    v                 |
 *                   +-- FIBER_SUSPENDED    | [Terminate]
 *                                          |
 *                       FIBER_TERMINATED <-+
 */
enum fiber_status {
    FIBER_CREATED,
    FIBER_RESUMED,
    FIBER_SUSPENDED,
    FIBER_TERMINATED
};

#define FIBER_CREATED_P(fiber)    ((fiber)->status == FIBER_CREATED)
#define FIBER_RESUMED_P(fiber)    ((fiber)->status == FIBER_RESUMED)
#define FIBER_SUSPENDED_P(fiber)  ((fiber)->status == FIBER_SUSPENDED)
#define FIBER_TERMINATED_P(fiber) ((fiber)->status == FIBER_TERMINATED)
#define FIBER_RUNNABLE_P(fiber)   (FIBER_CREATED_P(fiber) || FIBER_SUSPENDED_P(fiber))

struct rb_fiber_struct {
    rb_context_t cont;
    VALUE first_proc;
    struct rb_fiber_struct *prev;
    BITFIELD(enum fiber_status, status, 2);
    /* If a fiber invokes "transfer",
     * then this fiber can't "resume" any more after that.
     * You shouldn't mix "transfer" and "resume".
     */
    unsigned int transferred : 1;

    struct coroutine_context context;
    void *ss_sp;
    size_t ss_size;
};

#define MAX_MACHINE_STACK_CACHE  10
static int machine_stack_cache_index = 0;
typedef struct machine_stack_cache_struct {
    void *ptr;
    size_t size;
} machine_stack_cache_t;
static machine_stack_cache_t machine_stack_cache[MAX_MACHINE_STACK_CACHE];
static machine_stack_cache_t terminated_machine_stack;

static const char *
fiber_status_name(enum fiber_status s)
{
    switch (s) {
      case FIBER_CREATED: return "created";
      case FIBER_RESUMED: return "resumed";
      case FIBER_SUSPENDED: return "suspended";
      case FIBER_TERMINATED: return "terminated";
    }
    VM_UNREACHABLE(fiber_status_name);
    return NULL;
}

static void
fiber_verify(const rb_fiber_t *fiber)
{
#if VM_CHECK_MODE > 0
    VM_ASSERT(fiber->cont.saved_ec.fiber_ptr == fiber);

    switch (fiber->status) {
      case FIBER_RESUMED:
        VM_ASSERT(fiber->cont.saved_ec.vm_stack != NULL);
        break;
      case FIBER_SUSPENDED:
        VM_ASSERT(fiber->cont.saved_ec.vm_stack != NULL);
        break;
      case FIBER_CREATED:
      case FIBER_TERMINATED:
        /* TODO */
        break;
      default:
        VM_UNREACHABLE(fiber_verify);
    }
#endif
}

#if VM_CHECK_MODE > 0
void
rb_ec_verify(const rb_execution_context_t *ec)
{
    /* TODO */
}
#endif

static void
fiber_status_set(rb_fiber_t *fiber, enum fiber_status s)
{
    if (0) fprintf(stderr, "fiber: %p, status: %s -> %s\n", (void *)fiber, fiber_status_name(fiber->status), fiber_status_name(s));
    VM_ASSERT(!FIBER_TERMINATED_P(fiber));
    VM_ASSERT(fiber->status != s);
    fiber_verify(fiber);
    fiber->status = s;
}

static inline void
ec_switch(rb_thread_t *th, rb_fiber_t *fiber)
{
    rb_execution_context_t *ec = &fiber->cont.saved_ec;

    ruby_current_execution_context_ptr = th->ec = ec;

    /*
     * timer-thread may set trap interrupt on previous th->ec at any time;
     * ensure we do not delay (or lose) the trap interrupt handling.
     */
    if (th->vm->main_thread == th && rb_signal_buff_size() > 0) {
        RUBY_VM_SET_TRAP_INTERRUPT(ec);
    }

    VM_ASSERT(ec->fiber_ptr->cont.self == 0 || ec->vm_stack != NULL);
}

static const rb_data_type_t cont_data_type, fiber_data_type;
static VALUE rb_cContinuation;
static VALUE rb_cFiber;
static VALUE rb_eFiberError;

static rb_context_t *
cont_ptr(VALUE obj)
{
    rb_context_t *cont;

    TypedData_Get_Struct(obj, rb_context_t, &cont_data_type, cont);

    return cont;
}

static rb_fiber_t *
fiber_ptr(VALUE obj)
{
    rb_fiber_t *fiber;

    TypedData_Get_Struct(obj, rb_fiber_t, &fiber_data_type, fiber);
    if (!fiber) rb_raise(rb_eFiberError, "uninitialized fiber");

    return fiber;
}

NOINLINE(static VALUE cont_capture(volatile int *volatile stat));

#define THREAD_MUST_BE_RUNNING(th) do { \
        if (!(th)->ec->tag) rb_raise(rb_eThreadError, "not running thread");	\
    } while (0)

static VALUE
cont_thread_value(const rb_context_t *cont)
{
    return cont->saved_ec.thread_ptr->self;
}

static void
cont_compact(void *ptr)
{
    rb_context_t *cont = ptr;

    cont->value = rb_gc_location(cont->value);
    rb_execution_context_update(&cont->saved_ec);
}

static void
cont_mark(void *ptr)
{
    rb_context_t *cont = ptr;

    RUBY_MARK_ENTER("cont");
    rb_gc_mark_no_pin(cont->value);

    rb_execution_context_mark(&cont->saved_ec);
    rb_gc_mark(cont_thread_value(cont));

    if (cont->saved_vm_stack.ptr) {
#ifdef CAPTURE_JUST_VALID_VM_STACK
        rb_gc_mark_locations(cont->saved_vm_stack.ptr,
                             cont->saved_vm_stack.ptr + cont->saved_vm_stack.slen + cont->saved_vm_stack.clen);
#else
        rb_gc_mark_locations(cont->saved_vm_stack.ptr,
                             cont->saved_vm_stack.ptr, cont->saved_ec.stack_size);
#endif
    }

    if (cont->machine.stack) {
        if (cont->type == CONTINUATION_CONTEXT) {
            /* cont */
            rb_gc_mark_locations(cont->machine.stack,
                                 cont->machine.stack + cont->machine.stack_size);
        }
        else {
            /* fiber */
            const rb_fiber_t *fiber = (rb_fiber_t*)cont;

            if (!FIBER_TERMINATED_P(fiber)) {
                rb_gc_mark_locations(cont->machine.stack,
                                     cont->machine.stack + cont->machine.stack_size);
            }
        }
    }

    RUBY_MARK_LEAVE("cont");
}

static int
fiber_is_root_p(const rb_fiber_t *fiber)
{
    return fiber == fiber->cont.saved_ec.thread_ptr->root_fiber;
}

static void
cont_free(void *ptr)
{
    rb_context_t *cont = ptr;

    RUBY_FREE_ENTER("cont");

    if (cont->free_vm_stack) {
        ruby_xfree(cont->saved_ec.vm_stack);
    }

    if (cont->type == CONTINUATION_CONTEXT) {
        ruby_xfree(cont->ensure_array);
        RUBY_FREE_UNLESS_NULL(cont->machine.stack);
    } else {
        rb_fiber_t *fiber = (rb_fiber_t*)cont;
        coroutine_destroy(&fiber->context);
        if (fiber->ss_sp != NULL) {
            if (fiber_is_root_p(fiber)) {
                rb_bug("Illegal root fiber parameter");
            }
#ifdef _WIN32
            VirtualFree((void*)fiber->ss_sp, 0, MEM_RELEASE);
#else
            munmap((void*)fiber->ss_sp, fiber->ss_size);
#endif
            fiber->ss_sp = NULL;
        }
    }

    RUBY_FREE_UNLESS_NULL(cont->saved_vm_stack.ptr);

    if (mjit_enabled && cont->mjit_cont != NULL) {
        mjit_cont_free(cont->mjit_cont);
    }
    /* free rb_cont_t or rb_fiber_t */
    ruby_xfree(ptr);
    RUBY_FREE_LEAVE("cont");
}

static size_t
cont_memsize(const void *ptr)
{
    const rb_context_t *cont = ptr;
    size_t size = 0;

    size = sizeof(*cont);
    if (cont->saved_vm_stack.ptr) {
#ifdef CAPTURE_JUST_VALID_VM_STACK
        size_t n = (cont->saved_vm_stack.slen + cont->saved_vm_stack.clen);
#else
        size_t n = cont->saved_ec.vm_stack_size;
#endif
        size += n * sizeof(*cont->saved_vm_stack.ptr);
    }

    if (cont->machine.stack) {
        size += cont->machine.stack_size * sizeof(*cont->machine.stack);
    }

    return size;
}

void
rb_fiber_update_self(rb_fiber_t *fiber)
{
    if (fiber->cont.self) {
        fiber->cont.self = rb_gc_location(fiber->cont.self);
    }
    else {
        rb_execution_context_update(&fiber->cont.saved_ec);
    }
}

void
rb_fiber_mark_self(const rb_fiber_t *fiber)
{
    if (fiber->cont.self) {
        rb_gc_mark_no_pin(fiber->cont.self);
    }
    else {
        rb_execution_context_mark(&fiber->cont.saved_ec);
    }
}

static void
fiber_compact(void *ptr)
{
    rb_fiber_t *fiber = ptr;
    fiber->first_proc = rb_gc_location(fiber->first_proc);

    if (fiber->prev) rb_fiber_update_self(fiber->prev);

    cont_compact(&fiber->cont);
    fiber_verify(fiber);
}

static void
fiber_mark(void *ptr)
{
    rb_fiber_t *fiber = ptr;
    RUBY_MARK_ENTER("cont");
    fiber_verify(fiber);
    rb_gc_mark_no_pin(fiber->first_proc);
    if (fiber->prev) rb_fiber_mark_self(fiber->prev);
    cont_mark(&fiber->cont);
    RUBY_MARK_LEAVE("cont");
}

static void
fiber_free(void *ptr)
{
    rb_fiber_t *fiber = ptr;
    RUBY_FREE_ENTER("fiber");

    if (fiber->cont.saved_ec.local_storage) {
        st_free_table(fiber->cont.saved_ec.local_storage);
    }

    cont_free(&fiber->cont);
    RUBY_FREE_LEAVE("fiber");
}

static size_t
fiber_memsize(const void *ptr)
{
    const rb_fiber_t *fiber = ptr;
    size_t size = sizeof(*fiber);
    const rb_execution_context_t *saved_ec = &fiber->cont.saved_ec;
    const rb_thread_t *th = rb_ec_thread_ptr(saved_ec);

    /*
     * vm.c::thread_memsize already counts th->ec->local_storage
     */
    if (saved_ec->local_storage && fiber != th->root_fiber) {
        size += st_memsize(saved_ec->local_storage);
    }
    size += cont_memsize(&fiber->cont);
    return size;
}

VALUE
rb_obj_is_fiber(VALUE obj)
{
    if (rb_typeddata_is_kind_of(obj, &fiber_data_type)) {
        return Qtrue;
    }
    else {
        return Qfalse;
    }
}

static void
cont_save_machine_stack(rb_thread_t *th, rb_context_t *cont)
{
    size_t size;

    SET_MACHINE_STACK_END(&th->ec->machine.stack_end);

    if (th->ec->machine.stack_start > th->ec->machine.stack_end) {
        size = cont->machine.stack_size = th->ec->machine.stack_start - th->ec->machine.stack_end;
        cont->machine.stack_src = th->ec->machine.stack_end;
    }
    else {
        size = cont->machine.stack_size = th->ec->machine.stack_end - th->ec->machine.stack_start;
        cont->machine.stack_src = th->ec->machine.stack_start;
    }

    if (cont->machine.stack) {
        REALLOC_N(cont->machine.stack, VALUE, size);
    }
    else {
        cont->machine.stack = ALLOC_N(VALUE, size);
    }

    FLUSH_REGISTER_WINDOWS;
    MEMCPY(cont->machine.stack, cont->machine.stack_src, VALUE, size);
}

static const rb_data_type_t cont_data_type = {
    "continuation",
    {cont_mark, cont_free, cont_memsize, cont_compact},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static inline void
cont_save_thread(rb_context_t *cont, rb_thread_t *th)
{
    rb_execution_context_t *sec = &cont->saved_ec;

    VM_ASSERT(th->status == THREAD_RUNNABLE);

    /* save thread context */
    *sec = *th->ec;

    /* saved_ec->machine.stack_end should be NULL */
    /* because it may happen GC afterward */
    sec->machine.stack_end = NULL;
}

static void
cont_init(rb_context_t *cont, rb_thread_t *th)
{
    /* save thread context */
    cont_save_thread(cont, th);
    cont->saved_ec.thread_ptr = th;
    cont->saved_ec.local_storage = NULL;
    cont->saved_ec.local_storage_recursive_hash = Qnil;
    cont->saved_ec.local_storage_recursive_hash_for_trace = Qnil;
    if (mjit_enabled) {
        cont->mjit_cont = mjit_cont_new(&cont->saved_ec);
    }
}

static rb_context_t *
cont_new(VALUE klass)
{
    rb_context_t *cont;
    volatile VALUE contval;
    rb_thread_t *th = GET_THREAD();

    THREAD_MUST_BE_RUNNING(th);
    contval = TypedData_Make_Struct(klass, rb_context_t, &cont_data_type, cont);
    cont->self = contval;
    cont_init(cont, th);
    return cont;
}

#if 0
void
show_vm_stack(const rb_execution_context_t *ec)
{
    VALUE *p = ec->vm_stack;
    while (p < ec->cfp->sp) {
        fprintf(stderr, "%3d ", (int)(p - ec->vm_stack));
        rb_obj_info_dump(*p);
        p++;
    }
}

void
show_vm_pcs(const rb_control_frame_t *cfp,
            const rb_control_frame_t *end_of_cfp)
{
    int i=0;
    while (cfp != end_of_cfp) {
        int pc = 0;
        if (cfp->iseq) {
            pc = cfp->pc - cfp->iseq->body->iseq_encoded;
        }
        fprintf(stderr, "%2d pc: %d\n", i++, pc);
        cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
    }
}
#endif
COMPILER_WARNING_PUSH
#ifdef __clang__
COMPILER_WARNING_IGNORED(-Wduplicate-decl-specifier)
#endif
static VALUE
cont_capture(volatile int *volatile stat)
{
    rb_context_t *volatile cont;
    rb_thread_t *th = GET_THREAD();
    volatile VALUE contval;
    const rb_execution_context_t *ec = th->ec;

    THREAD_MUST_BE_RUNNING(th);
    rb_vm_stack_to_heap(th->ec);
    cont = cont_new(rb_cContinuation);
    contval = cont->self;

#ifdef CAPTURE_JUST_VALID_VM_STACK
    cont->saved_vm_stack.slen = ec->cfp->sp - ec->vm_stack;
    cont->saved_vm_stack.clen = ec->vm_stack + ec->vm_stack_size - (VALUE*)ec->cfp;
    cont->saved_vm_stack.ptr = ALLOC_N(VALUE, cont->saved_vm_stack.slen + cont->saved_vm_stack.clen);
    MEMCPY(cont->saved_vm_stack.ptr,
           ec->vm_stack,
           VALUE, cont->saved_vm_stack.slen);
    MEMCPY(cont->saved_vm_stack.ptr + cont->saved_vm_stack.slen,
           (VALUE*)ec->cfp,
           VALUE,
           cont->saved_vm_stack.clen);
#else
    cont->saved_vm_stack.ptr = ALLOC_N(VALUE, ec->vm_stack_size);
    MEMCPY(cont->saved_vm_stack.ptr, ec->vm_stack, VALUE, ec->vm_stack_size);
#endif
    rb_ec_clear_vm_stack(&cont->saved_ec);
    cont_save_machine_stack(th, cont);

    /* backup ensure_list to array for search in another context */
    {
        rb_ensure_list_t *p;
        int size = 0;
        rb_ensure_entry_t *entry;
        for (p=th->ec->ensure_list; p; p=p->next)
            size++;
        entry = cont->ensure_array = ALLOC_N(rb_ensure_entry_t,size+1);
        for (p=th->ec->ensure_list; p; p=p->next) {
            if (!p->entry.marker)
                p->entry.marker = rb_ary_tmp_new(0); /* dummy object */
            *entry++ = p->entry;
        }
        entry->marker = 0;
    }

    if (ruby_setjmp(cont->jmpbuf)) {
        VALUE value;

        VAR_INITIALIZED(cont);
        value = cont->value;
        if (cont->argc == -1) rb_exc_raise(value);
        cont->value = Qnil;
        *stat = 1;
        return value;
    }
    else {
        *stat = 0;
        return contval;
    }
}
COMPILER_WARNING_POP

static inline void
fiber_restore_thread(rb_thread_t *th, rb_fiber_t *fiber)
{
    ec_switch(th, fiber);
    VM_ASSERT(th->ec->fiber_ptr == fiber);
}

static inline void
cont_restore_thread(rb_context_t *cont)
{
    rb_thread_t *th = GET_THREAD();

    /* restore thread context */
    if (cont->type == CONTINUATION_CONTEXT) {
        /* continuation */
        rb_execution_context_t *sec = &cont->saved_ec;
        rb_fiber_t *fiber = NULL;

        if (sec->fiber_ptr != NULL) {
            fiber = sec->fiber_ptr;
        }
        else if (th->root_fiber) {
            fiber = th->root_fiber;
        }

        if (fiber && th->ec != &fiber->cont.saved_ec) {
            ec_switch(th, fiber);
        }

        if (th->ec->trace_arg != sec->trace_arg) {
            rb_raise(rb_eRuntimeError, "can't call across trace_func");
        }

        /* copy vm stack */
#ifdef CAPTURE_JUST_VALID_VM_STACK
        MEMCPY(th->ec->vm_stack,
               cont->saved_vm_stack.ptr,
               VALUE, cont->saved_vm_stack.slen);
        MEMCPY(th->ec->vm_stack + th->ec->vm_stack_size - cont->saved_vm_stack.clen,
               cont->saved_vm_stack.ptr + cont->saved_vm_stack.slen,
               VALUE, cont->saved_vm_stack.clen);
#else
        MEMCPY(th->ec->vm_stack, cont->saved_vm_stack.ptr, VALUE, sec->vm_stack_size);
#endif
        /* other members of ec */

        th->ec->cfp = sec->cfp;
        th->ec->raised_flag = sec->raised_flag;
        th->ec->tag = sec->tag;
        th->ec->protect_tag = sec->protect_tag;
        th->ec->root_lep = sec->root_lep;
        th->ec->root_svar = sec->root_svar;
        th->ec->ensure_list = sec->ensure_list;
        th->ec->errinfo = sec->errinfo;

        VM_ASSERT(th->ec->vm_stack != NULL);
    }
    else {
        /* fiber */
        fiber_restore_thread(th, (rb_fiber_t*)cont);
    }
}

static COROUTINE
fiber_entry(struct coroutine_context * from, struct coroutine_context * to)
{
    rb_fiber_start();
}

/*
 * FreeBSD require a first (i.e. addr) argument of mmap(2) is not NULL
 * if MAP_STACK is passed.
 * http://www.FreeBSD.org/cgi/query-pr.cgi?pr=158755
 */
#if defined(MAP_STACK) && !defined(__FreeBSD__) && !defined(__FreeBSD_kernel__)
#define FIBER_STACK_FLAGS (MAP_PRIVATE | MAP_ANON | MAP_STACK)
#else
#define FIBER_STACK_FLAGS (MAP_PRIVATE | MAP_ANON)
#endif

#define ERRNOMSG strerror(errno)

static char*
fiber_machine_stack_alloc(size_t size)
{
    char *ptr;
#ifdef _WIN32
    DWORD old_protect;
#endif

    if (machine_stack_cache_index > 0) {
        if (machine_stack_cache[machine_stack_cache_index - 1].size == (size / sizeof(VALUE))) {
            ptr = machine_stack_cache[machine_stack_cache_index - 1].ptr;
            machine_stack_cache_index--;
            machine_stack_cache[machine_stack_cache_index].ptr = NULL;
            machine_stack_cache[machine_stack_cache_index].size = 0;
        }
        else {
            /* TODO handle multiple machine stack size */
            rb_bug("machine_stack_cache size is not canonicalized");
        }
    }
    else {
#ifdef _WIN32
        ptr = VirtualAlloc(0, size, MEM_COMMIT, PAGE_READWRITE);

        if (!ptr) {
            rb_raise(rb_eFiberError, "can't allocate machine stack to fiber: %s", ERRNOMSG);
        }

        if (!VirtualProtect(ptr, RB_PAGE_SIZE, PAGE_READWRITE | PAGE_GUARD, &old_protect)) {
            rb_raise(rb_eFiberError, "can't set a guard page: %s", ERRNOMSG);
        }
#else
        void *page;
        STACK_GROW_DIR_DETECTION;

        errno = 0;
        ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, FIBER_STACK_FLAGS, -1, 0);
        if (ptr == MAP_FAILED) {
            rb_raise(rb_eFiberError, "can't alloc machine stack to fiber: %s", ERRNOMSG);
        }

        /* guard page setup */
        page = ptr + STACK_DIR_UPPER(size - RB_PAGE_SIZE, 0);
        if (mprotect(page, RB_PAGE_SIZE, PROT_NONE) < 0) {
            rb_raise(rb_eFiberError, "can't set a guard page: %s", ERRNOMSG);
        }
#endif
    }

    return ptr;
}

static void
fiber_initialize_machine_stack_context(rb_fiber_t *fiber, size_t size)
{
    rb_execution_context_t *sec = &fiber->cont.saved_ec;

    char *ptr;
    STACK_GROW_DIR_DETECTION;

    ptr = fiber_machine_stack_alloc(size);
    fiber->ss_sp = ptr;
    fiber->ss_size = size;
    coroutine_initialize(&fiber->context, fiber_entry, ptr, size);
    sec->machine.stack_start = (VALUE*)(ptr + STACK_DIR_UPPER(0, size));
    sec->machine.stack_maxsize = size - RB_PAGE_SIZE;
}

NOINLINE(static void fiber_setcontext(rb_fiber_t *new_fiber, rb_fiber_t *old_fiber));

static void
fiber_setcontext(rb_fiber_t *new_fiber, rb_fiber_t *old_fiber)
{
    rb_thread_t *th = GET_THREAD();

    /* save old_fiber's machine stack / TODO: is it needed? */
    if (!FIBER_TERMINATED_P(old_fiber)) {
        STACK_GROW_DIR_DETECTION;
        SET_MACHINE_STACK_END(&th->ec->machine.stack_end);
        if (STACK_DIR_UPPER(0, 1)) {
            old_fiber->cont.machine.stack_size = th->ec->machine.stack_start - th->ec->machine.stack_end;
            old_fiber->cont.machine.stack = th->ec->machine.stack_end;
        }
        else {
            old_fiber->cont.machine.stack_size = th->ec->machine.stack_end - th->ec->machine.stack_start;
            old_fiber->cont.machine.stack = th->ec->machine.stack_start;
        }
    }

    /* exchange machine_stack_start between old_fiber and new_fiber */
    old_fiber->cont.saved_ec.machine.stack_start = th->ec->machine.stack_start;

    /* old_fiber->machine.stack_end should be NULL */
    old_fiber->cont.saved_ec.machine.stack_end = NULL;

    /* restore thread context */
    fiber_restore_thread(th, new_fiber);

    /* swap machine context */
    coroutine_transfer(&old_fiber->context, &new_fiber->context);
}

NOINLINE(NORETURN(static void cont_restore_1(rb_context_t *)));

static void
cont_restore_1(rb_context_t *cont)
{
    cont_restore_thread(cont);

    /* restore machine stack */
#ifdef _M_AMD64
    {
        /* workaround for x64 SEH */
        jmp_buf buf;
        setjmp(buf);
        ((_JUMP_BUFFER*)(&cont->jmpbuf))->Frame =
            ((_JUMP_BUFFER*)(&buf))->Frame;
    }
#endif
    if (cont->machine.stack_src) {
        FLUSH_REGISTER_WINDOWS;
        MEMCPY(cont->machine.stack_src, cont->machine.stack,
                VALUE, cont->machine.stack_size);
    }

    ruby_longjmp(cont->jmpbuf, 1);
}

NORETURN(NOINLINE(static void cont_restore_0(rb_context_t *, VALUE *)));

static void
cont_restore_0(rb_context_t *cont, VALUE *addr_in_prev_frame)
{
    if (cont->machine.stack_src) {
#ifdef HAVE_ALLOCA
#define STACK_PAD_SIZE 1
#else
#define STACK_PAD_SIZE 1024
#endif
        VALUE space[STACK_PAD_SIZE];

#if !STACK_GROW_DIRECTION
        if (addr_in_prev_frame > &space[0]) {
            /* Stack grows downward */
#endif
#if STACK_GROW_DIRECTION <= 0
            volatile VALUE *const end = cont->machine.stack_src;
            if (&space[0] > end) {
# ifdef HAVE_ALLOCA
                volatile VALUE *sp = ALLOCA_N(VALUE, &space[0] - end);
                space[0] = *sp;
# else
                cont_restore_0(cont, &space[0]);
# endif
            }
#endif
#if !STACK_GROW_DIRECTION
        }
        else {
            /* Stack grows upward */
#endif
#if STACK_GROW_DIRECTION >= 0
            volatile VALUE *const end = cont->machine.stack_src + cont->machine.stack_size;
            if (&space[STACK_PAD_SIZE] < end) {
# ifdef HAVE_ALLOCA
                volatile VALUE *sp = ALLOCA_N(VALUE, end - &space[STACK_PAD_SIZE]);
                space[0] = *sp;
# else
                cont_restore_0(cont, &space[STACK_PAD_SIZE-1]);
# endif
            }
#endif
#if !STACK_GROW_DIRECTION
        }
#endif
    }
    cont_restore_1(cont);
}

/*
 *  Document-class: Continuation
 *
 *  Continuation objects are generated by Kernel#callcc,
 *  after having +require+d <i>continuation</i>. They hold
 *  a return address and execution context, allowing a nonlocal return
 *  to the end of the #callcc block from anywhere within a
 *  program. Continuations are somewhat analogous to a structured
 *  version of C's <code>setjmp/longjmp</code> (although they contain
 *  more state, so you might consider them closer to threads).
 *
 *  For instance:
 *
 *     require "continuation"
 *     arr = [ "Freddie", "Herbie", "Ron", "Max", "Ringo" ]
 *     callcc{|cc| $cc = cc}
 *     puts(message = arr.shift)
 *     $cc.call unless message =~ /Max/
 *
 *  <em>produces:</em>
 *
 *     Freddie
 *     Herbie
 *     Ron
 *     Max
 *
 *  Also you can call callcc in other methods:
 *
 *     require "continuation"
 *
 *     def g
 *       arr = [ "Freddie", "Herbie", "Ron", "Max", "Ringo" ]
 *       cc = callcc { |cc| cc }
 *       puts arr.shift
 *       return cc, arr.size
 *     end
 *
 *     def f
 *       c, size = g
 *       c.call(c) if size > 1
 *     end
 *
 *     f
 *
 *  This (somewhat contrived) example allows the inner loop to abandon
 *  processing early:
 *
 *     require "continuation"
 *     callcc {|cont|
 *       for i in 0..4
 *         print "\n#{i}: "
 *         for j in i*5...(i+1)*5
 *           cont.call() if j == 17
 *           printf "%3d", j
 *         end
 *       end
 *     }
 *     puts
 *
 *  <em>produces:</em>
 *
 *     0:   0  1  2  3  4
 *     1:   5  6  7  8  9
 *     2:  10 11 12 13 14
 *     3:  15 16
 */

/*
 *  call-seq:
 *     callcc {|cont| block }   ->  obj
 *
 *  Generates a Continuation object, which it passes to
 *  the associated block. You need to <code>require
 *  'continuation'</code> before using this method. Performing a
 *  <em>cont</em><code>.call</code> will cause the #callcc
 *  to return (as will falling through the end of the block). The
 *  value returned by the #callcc is the value of the
 *  block, or the value passed to <em>cont</em><code>.call</code>. See
 *  class Continuation for more details. Also see
 *  Kernel#throw for an alternative mechanism for
 *  unwinding a call stack.
 */

static VALUE
rb_callcc(VALUE self)
{
    volatile int called;
    volatile VALUE val = cont_capture(&called);

    if (called) {
        return val;
    }
    else {
        return rb_yield(val);
    }
}

static VALUE
make_passing_arg(int argc, const VALUE *argv)
{
    switch (argc) {
      case -1:
        return argv[0];
      case 0:
        return Qnil;
      case 1:
        return argv[0];
      default:
        return rb_ary_new4(argc, argv);
    }
}

/* CAUTION!! : Currently, error in rollback_func is not supported  */
/* same as rb_protect if set rollback_func to NULL */
void
ruby_register_rollback_func_for_ensure(VALUE (*ensure_func)(ANYARGS), VALUE (*rollback_func)(ANYARGS))
{
    st_table **table_p = &GET_VM()->ensure_rollback_table;
    if (UNLIKELY(*table_p == NULL)) {
        *table_p = st_init_numtable();
    }
    st_insert(*table_p, (st_data_t)ensure_func, (st_data_t)rollback_func);
}

static inline VALUE
lookup_rollback_func(VALUE (*ensure_func)(ANYARGS))
{
    st_table *table = GET_VM()->ensure_rollback_table;
    st_data_t val;
    if (table && st_lookup(table, (st_data_t)ensure_func, &val))
        return (VALUE) val;
    return Qundef;
}


static inline void
rollback_ensure_stack(VALUE self,rb_ensure_list_t *current,rb_ensure_entry_t *target)
{
    rb_ensure_list_t *p;
    rb_ensure_entry_t *entry;
    size_t i, j;
    size_t cur_size;
    size_t target_size;
    size_t base_point;
    VALUE (*func)(ANYARGS);

    cur_size = 0;
    for (p=current; p; p=p->next)
        cur_size++;
    target_size = 0;
    for (entry=target; entry->marker; entry++)
        target_size++;

    /* search common stack point */
    p = current;
    base_point = cur_size;
    while (base_point) {
        if (target_size >= base_point &&
            p->entry.marker == target[target_size - base_point].marker)
            break;
        base_point --;
        p = p->next;
    }

    /* rollback function check */
    for (i=0; i < target_size - base_point; i++) {
        if (!lookup_rollback_func(target[i].e_proc)) {
            rb_raise(rb_eRuntimeError, "continuation called from out of critical rb_ensure scope");
        }
    }
    /* pop ensure stack */
    while (cur_size > base_point) {
        /* escape from ensure block */
        (*current->entry.e_proc)(current->entry.data2);
        current = current->next;
        cur_size--;
    }
    /* push ensure stack */
    for (j = 0; j < i; j++) {
        func = (VALUE (*)(ANYARGS)) lookup_rollback_func(target[i - j - 1].e_proc);
        if ((VALUE)func != Qundef) {
            (*func)(target[i - j - 1].data2);
        }
    }
}

/*
 *  call-seq:
 *     cont.call(args, ...)
 *     cont[args, ...]
 *
 *  Invokes the continuation. The program continues from the end of
 *  the #callcc block. If no arguments are given, the original #callcc
 *  returns +nil+. If one argument is given, #callcc returns
 *  it. Otherwise, an array containing <i>args</i> is returned.
 *
 *     callcc {|cont|  cont.call }           #=> nil
 *     callcc {|cont|  cont.call 1 }         #=> 1
 *     callcc {|cont|  cont.call 1, 2, 3 }   #=> [1, 2, 3]
 */

static VALUE
rb_cont_call(int argc, VALUE *argv, VALUE contval)
{
    rb_context_t *cont = cont_ptr(contval);
    rb_thread_t *th = GET_THREAD();

    if (cont_thread_value(cont) != th->self) {
        rb_raise(rb_eRuntimeError, "continuation called across threads");
    }
    if (cont->saved_ec.protect_tag != th->ec->protect_tag) {
        rb_raise(rb_eRuntimeError, "continuation called across stack rewinding barrier");
    }
    if (cont->saved_ec.fiber_ptr) {
        if (th->ec->fiber_ptr != cont->saved_ec.fiber_ptr) {
            rb_raise(rb_eRuntimeError, "continuation called across fiber");
        }
    }
    rollback_ensure_stack(contval, th->ec->ensure_list, cont->ensure_array);

    cont->argc = argc;
    cont->value = make_passing_arg(argc, argv);

    cont_restore_0(cont, &contval);
    return Qnil; /* unreachable */
}

/*********/
/* fiber */
/*********/

/*
 *  Document-class: Fiber
 *
 *  Fibers are primitives for implementing light weight cooperative
 *  concurrency in Ruby. Basically they are a means of creating code blocks
 *  that can be paused and resumed, much like threads. The main difference
 *  is that they are never preempted and that the scheduling must be done by
 *  the programmer and not the VM.
 *
 *  As opposed to other stackless light weight concurrency models, each fiber
 *  comes with a stack.  This enables the fiber to be paused from deeply
 *  nested function calls within the fiber block.  See the ruby(1)
 *  manpage to configure the size of the fiber stack(s).
 *
 *  When a fiber is created it will not run automatically. Rather it must
 *  be explicitly asked to run using the Fiber#resume method.
 *  The code running inside the fiber can give up control by calling
 *  Fiber.yield in which case it yields control back to caller (the
 *  caller of the Fiber#resume).
 *
 *  Upon yielding or termination the Fiber returns the value of the last
 *  executed expression
 *
 *  For instance:
 *
 *    fiber = Fiber.new do
 *      Fiber.yield 1
 *      2
 *    end
 *
 *    puts fiber.resume
 *    puts fiber.resume
 *    puts fiber.resume
 *
 *  <em>produces</em>
 *
 *    1
 *    2
 *    FiberError: dead fiber called
 *
 *  The Fiber#resume method accepts an arbitrary number of parameters,
 *  if it is the first call to #resume then they will be passed as
 *  block arguments. Otherwise they will be the return value of the
 *  call to Fiber.yield
 *
 *  Example:
 *
 *    fiber = Fiber.new do |first|
 *      second = Fiber.yield first + 2
 *    end
 *
 *    puts fiber.resume 10
 *    puts fiber.resume 14
 *    puts fiber.resume 18
 *
 *  <em>produces</em>
 *
 *    12
 *    14
 *    FiberError: dead fiber called
 *
 */

static const rb_data_type_t fiber_data_type = {
    "fiber",
    {fiber_mark, fiber_free, fiber_memsize, fiber_compact,},
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY
};

static VALUE
fiber_alloc(VALUE klass)
{
    return TypedData_Wrap_Struct(klass, &fiber_data_type, 0);
}

static rb_fiber_t*
fiber_t_alloc(VALUE fiber_value)
{
    rb_fiber_t *fiber;
    rb_thread_t *th = GET_THREAD();

    if (DATA_PTR(fiber_value) != 0) {
        rb_raise(rb_eRuntimeError, "cannot initialize twice");
    }

    THREAD_MUST_BE_RUNNING(th);
    fiber = ZALLOC(rb_fiber_t);
    fiber->cont.self = fiber_value;
    fiber->cont.type = FIBER_CONTEXT;
    cont_init(&fiber->cont, th);
    fiber->cont.saved_ec.fiber_ptr = fiber;
    fiber->prev = NULL;

    /* fiber->status == 0 == CREATED
     * So that we don't need to set status: fiber_status_set(fiber, FIBER_CREATED); */
    VM_ASSERT(FIBER_CREATED_P(fiber));

    DATA_PTR(fiber_value) = fiber;

    return fiber;
}

rb_control_frame_t *
rb_vm_push_frame(rb_execution_context_t *sec,
                 const rb_iseq_t *iseq,
                 VALUE type,
                 VALUE self,
                 VALUE specval,
                 VALUE cref_or_me,
                 const VALUE *pc,
                 VALUE *sp,
                 int local_size,
                 int stack_max);

static VALUE
fiber_init(VALUE fiber_value, VALUE proc)
{
    rb_fiber_t *fiber = fiber_t_alloc(fiber_value);
    rb_context_t *cont = &fiber->cont;
    rb_execution_context_t *sec = &cont->saved_ec;
    rb_thread_t *cth = GET_THREAD();
    rb_vm_t *vm = cth->vm;
    size_t fiber_vm_stack_size = vm->default_params.fiber_vm_stack_size;
    size_t thread_vm_stack_size = vm->default_params.thread_vm_stack_size;
    VALUE *vm_stack;

    /* initialize cont */
    cont->saved_vm_stack.ptr = NULL;
    if (fiber_vm_stack_size == thread_vm_stack_size) {
        vm_stack = rb_thread_recycle_stack(fiber_vm_stack_size / sizeof(VALUE));
    }
    else {
        vm_stack = ruby_xmalloc(fiber_vm_stack_size);
    }

    cont->free_vm_stack = 1;
    rb_ec_initialize_vm_stack(sec, vm_stack, fiber_vm_stack_size / sizeof(VALUE));

    sec->tag = NULL;
    sec->local_storage = NULL;
    sec->local_storage_recursive_hash = Qnil;
    sec->local_storage_recursive_hash_for_trace = Qnil;

    fiber->first_proc = proc;

    return fiber_value;
}

/* :nodoc: */
static VALUE
rb_fiber_init(VALUE fiber_value)
{
    return fiber_init(fiber_value, rb_block_proc());
}

VALUE
rb_fiber_new(VALUE (*func)(ANYARGS), VALUE obj)
{
    return fiber_init(fiber_alloc(rb_cFiber), rb_proc_new(func, obj));
}

static void rb_fiber_terminate(rb_fiber_t *fiber, int need_interrupt);

void
rb_fiber_start(void)
{
    rb_thread_t * volatile th = GET_THREAD();
    rb_fiber_t *fiber = th->ec->fiber_ptr;
    rb_proc_t *proc;
    enum ruby_tag_type state;
    int need_interrupt = TRUE;

    VM_ASSERT(th->ec == ruby_current_execution_context_ptr);
    VM_ASSERT(FIBER_RESUMED_P(fiber));

    EC_PUSH_TAG(th->ec);
    if ((state = EC_EXEC_TAG()) == TAG_NONE) {
        rb_context_t *cont = &VAR_FROM_MEMORY(fiber)->cont;
        int argc;
        const VALUE *argv, args = cont->value;
        GetProcPtr(fiber->first_proc, proc);
        argv = (argc = cont->argc) > 1 ? RARRAY_CONST_PTR(args) : &args;
        cont->value = Qnil;
        th->ec->errinfo = Qnil;
        th->ec->root_lep = rb_vm_proc_local_ep(fiber->first_proc);
        th->ec->root_svar = Qfalse;

        EXEC_EVENT_HOOK(th->ec, RUBY_EVENT_FIBER_SWITCH, th->self, 0, 0, 0, Qnil);
        cont->value = rb_vm_invoke_proc(th->ec, proc, argc, argv, VM_BLOCK_HANDLER_NONE);
    }
    EC_POP_TAG();

    if (state) {
        VALUE err = th->ec->errinfo;
        VM_ASSERT(FIBER_RESUMED_P(fiber));

        if (state == TAG_RAISE || state == TAG_FATAL) {
            rb_threadptr_pending_interrupt_enque(th, err);
        }
        else {
            err = rb_vm_make_jump_tag_but_local_jump(state, err);
            if (!NIL_P(err)) {
                rb_threadptr_pending_interrupt_enque(th, err);
            }
        }
        need_interrupt = TRUE;
    }

    rb_fiber_terminate(fiber, need_interrupt);
    VM_UNREACHABLE(rb_fiber_start);
}

static rb_fiber_t *
root_fiber_alloc(rb_thread_t *th)
{
    VALUE fiber_value = fiber_alloc(rb_cFiber);
    rb_fiber_t *fiber = th->ec->fiber_ptr;

    VM_ASSERT(DATA_PTR(fiber_value) == NULL);
    VM_ASSERT(fiber->cont.type == FIBER_CONTEXT);
    VM_ASSERT(fiber->status == FIBER_RESUMED);

    th->root_fiber = fiber;
    DATA_PTR(fiber_value) = fiber;
    fiber->cont.self = fiber_value;

    coroutine_initialize_main(&fiber->context);

    return fiber;
}

void
rb_threadptr_root_fiber_setup(rb_thread_t *th)
{
    rb_fiber_t *fiber = ruby_mimmalloc(sizeof(rb_fiber_t));
    MEMZERO(fiber, rb_fiber_t, 1);
    fiber->cont.type = FIBER_CONTEXT;
    fiber->cont.saved_ec.fiber_ptr = fiber;
    fiber->cont.saved_ec.thread_ptr = th;
    fiber_status_set(fiber, FIBER_RESUMED); /* skip CREATED */
    th->ec = &fiber->cont.saved_ec;

    VM_ASSERT(fiber->cont.free_vm_stack == 0);

    /* NOTE: On WIN32, fiber_handle is not allocated yet. */
}

void
rb_threadptr_root_fiber_release(rb_thread_t *th)
{
    if (th->root_fiber) {
        /* ignore. A root fiber object will free th->ec */
    }
    else {
        VM_ASSERT(th->ec->fiber_ptr->cont.type == FIBER_CONTEXT);
        VM_ASSERT(th->ec->fiber_ptr->cont.self == 0);
        fiber_free(th->ec->fiber_ptr);

        if (th->ec == ruby_current_execution_context_ptr) {
            ruby_current_execution_context_ptr = NULL;
        }
        th->ec = NULL;
    }
}

void
rb_threadptr_root_fiber_terminate(rb_thread_t *th)
{
    rb_fiber_t *fiber = th->ec->fiber_ptr;

    fiber->status = FIBER_TERMINATED;

    // The vm_stack is `alloca`ed on the thread stack, so it's gone too:
    rb_ec_clear_vm_stack(th->ec);
}

static inline rb_fiber_t*
fiber_current(void)
{
    rb_execution_context_t *ec = GET_EC();
    if (ec->fiber_ptr->cont.self == 0) {
        root_fiber_alloc(rb_ec_thread_ptr(ec));
    }
    return ec->fiber_ptr;
}

static inline rb_fiber_t*
return_fiber(void)
{
    rb_fiber_t *fiber = fiber_current();
    rb_fiber_t *prev = fiber->prev;

    if (!prev) {
        rb_thread_t *th = GET_THREAD();
        rb_fiber_t *root_fiber = th->root_fiber;

        VM_ASSERT(root_fiber != NULL);

        if (root_fiber == fiber) {
            rb_raise(rb_eFiberError, "can't yield from root fiber");
        }
        return root_fiber;
    }
    else {
        fiber->prev = NULL;
        return prev;
    }
}

VALUE
rb_fiber_current(void)
{
    return fiber_current()->cont.self;
}

static inline VALUE
fiber_store(rb_fiber_t *next_fiber, rb_thread_t *th)
{
    rb_fiber_t *fiber;

    if (th->ec->fiber_ptr != NULL) {
        fiber = th->ec->fiber_ptr;
    }
    else {
        /* create root fiber */
        fiber = root_fiber_alloc(th);
    }

    VM_ASSERT(FIBER_RESUMED_P(fiber) || FIBER_TERMINATED_P(fiber));
    VM_ASSERT(FIBER_RUNNABLE_P(next_fiber));

    if (FIBER_CREATED_P(next_fiber)) {
        fiber_initialize_machine_stack_context(next_fiber, th->vm->default_params.fiber_machine_stack_size);
    }

    if (FIBER_RESUMED_P(fiber)) fiber_status_set(fiber, FIBER_SUSPENDED);

    fiber_status_set(next_fiber, FIBER_RESUMED);

    fiber_setcontext(next_fiber, fiber);

    if (terminated_machine_stack.ptr) {
        if (machine_stack_cache_index < MAX_MACHINE_STACK_CACHE) {
            machine_stack_cache[machine_stack_cache_index++] = terminated_machine_stack;
        }
        else {
            if (terminated_machine_stack.ptr != fiber->cont.machine.stack) {
#ifdef _WIN32
                VirtualFree(terminated_machine_stack.ptr, 0, MEM_RELEASE);
#else
                munmap((void*)terminated_machine_stack.ptr, terminated_machine_stack.size * sizeof(VALUE));
#endif
            }
            else {
                rb_bug("terminated fiber resumed");
            }
        }
        terminated_machine_stack.ptr = NULL;
        terminated_machine_stack.size = 0;
    }

    fiber = th->ec->fiber_ptr;
    if (fiber->cont.argc == -1) rb_exc_raise(fiber->cont.value);
    return fiber->cont.value;
}

static inline VALUE
fiber_switch(rb_fiber_t *fiber, int argc, const VALUE *argv, int is_resume)
{
    VALUE value;
    rb_context_t *cont = &fiber->cont;
    rb_thread_t *th = GET_THREAD();

    /* make sure the root_fiber object is available */
    if (th->root_fiber == NULL) root_fiber_alloc(th);

    if (th->ec->fiber_ptr == fiber) {
        /* ignore fiber context switch
         * because destination fiber is same as current fiber
         */
        return make_passing_arg(argc, argv);
    }

    if (cont_thread_value(cont) != th->self) {
        rb_raise(rb_eFiberError, "fiber called across threads");
    }
    else if (cont->saved_ec.protect_tag != th->ec->protect_tag) {
        rb_raise(rb_eFiberError, "fiber called across stack rewinding barrier");
    }
    else if (FIBER_TERMINATED_P(fiber)) {
        value = rb_exc_new2(rb_eFiberError, "dead fiber called");

        if (!FIBER_TERMINATED_P(th->ec->fiber_ptr)) {
            rb_exc_raise(value);
            VM_UNREACHABLE(fiber_switch);
        }
        else {
            /* th->ec->fiber_ptr is also dead => switch to root fiber */
            /* (this means we're being called from rb_fiber_terminate, */
            /* and the terminated fiber's return_fiber() is already dead) */
            VM_ASSERT(FIBER_SUSPENDED_P(th->root_fiber));

            cont = &th->root_fiber->cont;
            cont->argc = -1;
            cont->value = value;

            fiber_setcontext(th->root_fiber, th->ec->fiber_ptr);

            VM_UNREACHABLE(fiber_switch);
        }
    }

    if (is_resume) {
        fiber->prev = fiber_current();
    }

    VM_ASSERT(FIBER_RUNNABLE_P(fiber));

    cont->argc = argc;
    cont->value = make_passing_arg(argc, argv);
    value = fiber_store(fiber, th);
    RUBY_VM_CHECK_INTS(th->ec);

    EXEC_EVENT_HOOK(th->ec, RUBY_EVENT_FIBER_SWITCH, th->self, 0, 0, 0, Qnil);

    return value;
}

VALUE
rb_fiber_transfer(VALUE fiber_value, int argc, const VALUE *argv)
{
    return fiber_switch(fiber_ptr(fiber_value), argc, argv, 0);
}

void
rb_fiber_close(rb_fiber_t *fiber)
{
    rb_execution_context_t *ec = &fiber->cont.saved_ec;
    VALUE *vm_stack = ec->vm_stack;
    size_t stack_bytes = ec->vm_stack_size * sizeof(VALUE);

    fiber_status_set(fiber, FIBER_TERMINATED);
    if (fiber->cont.free_vm_stack) {
        if (stack_bytes == rb_ec_vm_ptr(ec)->default_params.thread_vm_stack_size) {
            rb_thread_recycle_stack_release(vm_stack);
        }
        else {
            ruby_xfree(vm_stack);
        }
    }

    rb_ec_clear_vm_stack(ec);
}

static void
rb_fiber_terminate(rb_fiber_t *fiber, int need_interrupt)
{
    VALUE value = fiber->cont.value;
    rb_fiber_t *ret_fiber;

    VM_ASSERT(FIBER_RESUMED_P(fiber));
    rb_fiber_close(fiber);

    coroutine_destroy(&fiber->context);

    /* Ruby must not switch to other thread until storing terminated_machine_stack */
    terminated_machine_stack.ptr = fiber->ss_sp;
    terminated_machine_stack.size = fiber->ss_size / sizeof(VALUE);
    fiber->ss_sp = NULL;
    fiber->cont.machine.stack = NULL;
    fiber->cont.machine.stack_size = 0;

    ret_fiber = return_fiber();
    if (need_interrupt) RUBY_VM_SET_INTERRUPT(&ret_fiber->cont.saved_ec);
    fiber_switch(ret_fiber, 1, &value, 0);
}

VALUE
rb_fiber_resume(VALUE fiber_value, int argc, const VALUE *argv)
{
    rb_fiber_t *fiber = fiber_ptr(fiber_value);

    if (argc == -1 && FIBER_CREATED_P(fiber)) {
        rb_raise(rb_eFiberError, "cannot raise exception on unborn fiber");
    }

    if (fiber->prev != 0 || fiber_is_root_p(fiber)) {
        rb_raise(rb_eFiberError, "double resume");
    }

    if (fiber->transferred != 0) {
        rb_raise(rb_eFiberError, "cannot resume transferred Fiber");
    }

    return fiber_switch(fiber, argc, argv, 1);
}

VALUE
rb_fiber_yield(int argc, const VALUE *argv)
{
    return fiber_switch(return_fiber(), argc, argv, 0);
}

void
rb_fiber_reset_root_local_storage(rb_thread_t *th)
{
    if (th->root_fiber && th->root_fiber != th->ec->fiber_ptr) {
        th->ec->local_storage = th->root_fiber->cont.saved_ec.local_storage;
    }
}

/*
 *  call-seq:
 *     fiber.alive? -> true or false
 *
 *  Returns true if the fiber can still be resumed (or transferred
 *  to). After finishing execution of the fiber block this method will
 *  always return false. You need to <code>require 'fiber'</code>
 *  before using this method.
 */
VALUE
rb_fiber_alive_p(VALUE fiber_value)
{
    return FIBER_TERMINATED_P(fiber_ptr(fiber_value)) ? Qfalse : Qtrue;
}

/*
 *  call-seq:
 *     fiber.resume(args, ...) -> obj
 *
 *  Resumes the fiber from the point at which the last Fiber.yield was
 *  called, or starts running it if it is the first call to
 *  #resume. Arguments passed to resume will be the value of the
 *  Fiber.yield expression or will be passed as block parameters to
 *  the fiber's block if this is the first #resume.
 *
 *  Alternatively, when resume is called it evaluates to the arguments passed
 *  to the next Fiber.yield statement inside the fiber's block
 *  or to the block value if it runs to completion without any
 *  Fiber.yield
 */
static VALUE
rb_fiber_m_resume(int argc, VALUE *argv, VALUE fiber)
{
    return rb_fiber_resume(fiber, argc, argv);
}

/*
 *  call-seq:
 *     fiber.raise                                 -> obj
 *     fiber.raise(string)                         -> obj
 *     fiber.raise(exception [, string [, array]]) -> obj
 *
 *  Raises an exception in the fiber at the point at which the last
 *  Fiber.yield was called, or at the start if neither +resume+
 *  nor +raise+ were called before.
 *
 *  With no arguments, raises a +RuntimeError+. With a single +String+
 *  argument, raises a +RuntimeError+ with the string as a message.  Otherwise,
 *  the first parameter should be the name of an +Exception+ class (or an
 *  object that returns an +Exception+ object when sent an +exception+
 *  message). The optional second parameter sets the message associated with
 *  the exception, and the third parameter is an array of callback information.
 *  Exceptions are caught by the +rescue+ clause of <code>begin...end</code>
 *  blocks.
 */
static VALUE
rb_fiber_raise(int argc, VALUE *argv, VALUE fiber)
{
    VALUE exc = rb_make_exception(argc, argv);
    return rb_fiber_resume(fiber, -1, &exc);
}

/*
 *  call-seq:
 *     fiber.transfer(args, ...) -> obj
 *
 *  Transfer control to another fiber, resuming it from where it last
 *  stopped or starting it if it was not resumed before. The calling
 *  fiber will be suspended much like in a call to
 *  Fiber.yield. You need to <code>require 'fiber'</code>
 *  before using this method.
 *
 *  The fiber which receives the transfer call is treats it much like
 *  a resume call. Arguments passed to transfer are treated like those
 *  passed to resume.
 *
 *  You cannot resume a fiber that transferred control to another one.
 *  This will cause a double resume error. You need to transfer control
 *  back to this fiber before it can yield and resume.
 *
 *  Example:
 *
 *    fiber1 = Fiber.new do
 *      puts "In Fiber 1"
 *      Fiber.yield
 *    end
 *
 *    fiber2 = Fiber.new do
 *      puts "In Fiber 2"
 *      fiber1.transfer
 *      puts "Never see this message"
 *    end
 *
 *    fiber3 = Fiber.new do
 *      puts "In Fiber 3"
 *    end
 *
 *    fiber2.resume
 *    fiber3.resume
 *
 *  <em>produces</em>
 *
 *    In fiber 2
 *    In fiber 1
 *    In fiber 3
 *
 */
static VALUE
rb_fiber_m_transfer(int argc, VALUE *argv, VALUE fiber_value)
{
    rb_fiber_t *fiber = fiber_ptr(fiber_value);
    fiber->transferred = 1;
    return fiber_switch(fiber, argc, argv, 0);
}

/*
 *  call-seq:
 *     Fiber.yield(args, ...) -> obj
 *
 *  Yields control back to the context that resumed the fiber, passing
 *  along any arguments that were passed to it. The fiber will resume
 *  processing at this point when #resume is called next.
 *  Any arguments passed to the next #resume will be the value that
 *  this Fiber.yield expression evaluates to.
 */
static VALUE
rb_fiber_s_yield(int argc, VALUE *argv, VALUE klass)
{
    return rb_fiber_yield(argc, argv);
}

/*
 *  call-seq:
 *     Fiber.current() -> fiber
 *
 *  Returns the current fiber. You need to <code>require 'fiber'</code>
 *  before using this method. If you are not running in the context of
 *  a fiber this method will return the root fiber.
 */
static VALUE
rb_fiber_s_current(VALUE klass)
{
    return rb_fiber_current();
}

/*
 * call-seq:
 *   fiber.to_s   -> string
 *
 * Returns fiber information string.
 *
 */

static VALUE
fiber_to_s(VALUE fiber_value)
{
    const rb_fiber_t *fiber = fiber_ptr(fiber_value);
    const rb_proc_t *proc;
    char status_info[0x10];

    snprintf(status_info, 0x10, " (%s)", fiber_status_name(fiber->status));
    if (!rb_obj_is_proc(fiber->first_proc)) {
        VALUE str = rb_any_to_s(fiber_value);
        strlcat(status_info, ">", sizeof(status_info));
        rb_str_set_len(str, RSTRING_LEN(str)-1);
        rb_str_cat_cstr(str, status_info);
        return str;
    }
    GetProcPtr(fiber->first_proc, proc);
    return rb_block_to_s(fiber_value, &proc->block, status_info);
}

#ifdef HAVE_WORKING_FORK
void
rb_fiber_atfork(rb_thread_t *th)
{
    if (th->root_fiber) {
        if (&th->root_fiber->cont.saved_ec != th->ec) {
            th->root_fiber = th->ec->fiber_ptr;
        }
        th->root_fiber->prev = 0;
    }
}
#endif

/*
 *  Document-class: FiberError
 *
 *  Raised when an invalid operation is attempted on a Fiber, in
 *  particular when attempting to call/resume a dead fiber,
 *  attempting to yield from the root fiber, or calling a fiber across
 *  threads.
 *
 *     fiber = Fiber.new{}
 *     fiber.resume #=> nil
 *     fiber.resume #=> FiberError: dead fiber called
 */

void
Init_Cont(void)
{
    rb_thread_t *th = GET_THREAD();

#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    pagesize = info.dwPageSize;
#else /* not WIN32 */
    pagesize = sysconf(_SC_PAGESIZE);
#endif
    SET_MACHINE_STACK_END(&th->ec->machine.stack_end);

    rb_cFiber = rb_define_class("Fiber", rb_cObject);
    rb_define_alloc_func(rb_cFiber, fiber_alloc);
    rb_eFiberError = rb_define_class("FiberError", rb_eStandardError);
    rb_define_singleton_method(rb_cFiber, "yield", rb_fiber_s_yield, -1);
    rb_define_method(rb_cFiber, "initialize", rb_fiber_init, 0);
    rb_define_method(rb_cFiber, "resume", rb_fiber_m_resume, -1);
    rb_define_method(rb_cFiber, "raise", rb_fiber_raise, -1);
    rb_define_method(rb_cFiber, "to_s", fiber_to_s, 0);
    rb_define_alias(rb_cFiber, "inspect", "to_s");
}

RUBY_SYMBOL_EXPORT_BEGIN

void
ruby_Init_Continuation_body(void)
{
    rb_cContinuation = rb_define_class("Continuation", rb_cObject);
    rb_undef_alloc_func(rb_cContinuation);
    rb_undef_method(CLASS_OF(rb_cContinuation), "new");
    rb_define_method(rb_cContinuation, "call", rb_cont_call, -1);
    rb_define_method(rb_cContinuation, "[]", rb_cont_call, -1);
    rb_define_global_function("callcc", rb_callcc, 0);
}

void
ruby_Init_Fiber_as_Coroutine(void)
{
    rb_define_method(rb_cFiber, "transfer", rb_fiber_m_transfer, -1);
    rb_define_method(rb_cFiber, "alive?", rb_fiber_alive_p, 0);
    rb_define_singleton_method(rb_cFiber, "current", rb_fiber_s_current, 0);
}

RUBY_SYMBOL_EXPORT_END
