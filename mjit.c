/**********************************************************************

  mjit.c - MRI method JIT compiler

  Copyright (C) 2017 Vladimir Makarov <vmakarov@redhat.com>.

**********************************************************************/

/* We utilize widely used C compilers (GCC and LLVM Clang) to
   implement MJIT.  We feed them a C code generated from ISEQ.  The
   industrial C compilers are slower than regular JIT engines.
   Generated code performance of the used C compilers has a higher
   priority over the compilation speed.

   So our major goal is to minimize the ISEQ compilation time when we
   use widely optimization level (-O2).  It is achieved by

   o Minimizing C code header needed for RTL insns (a special Ruby
     script is used for this)
   o Using a precompiled version of the header
   o Keeping all files in `/tmp`.  On modern Linux `/tmp` is a file
     system in memory. So it is pretty fast
   o An optional compiling more one ISEQ (batch of ISEQs) by C compiler
   o Implementing MJIT as a multi-threaded code because we want to
     compile ISEQs in parallel with iseq execution to speed up Ruby
     code execution.  MJIT has a several threads (*workers*) to do
     parallel compilations:
      o One worker prepares a precompiled code of the minimized
        header. It starts at the MRI execution start
      o One or more workers generate PIC object files of batch ISEQs
      o They start when the precompiled header is ready
      o They take one batch from a priority queue unless it is empty.
      o They translate the batch ISEQs into C-code using the precompiled
        header, call CC and load PIC code when it is ready
      o A worker don't start processing another batch until it finishes
        processing the current batch.
      o Currently MJIT put ISEQ in the queue when ISEQ is called or right
        after generating ISEQ for AOT
      o MJIT can reorder ISEQs in the queue if some ISEQ has been called
        many times and its compilation did not start yet or we need the
        ISEQ code for AOT
      o MRI reuses the machine code if it already exists for ISEQ
      o The machine code we generate can stop and switch to the ISEQ
        interpretation if some condition is not satisfied as the machine
        code can be speculative or some exception raises
      o Speculative machine code can be canceled, and a new
        mutated machine code can be queued for creation.  It can
        happen when insn speculation was wrong.  There is a constraint
        on the mutation number.  The last mutation will contain the
        code without speculation

   Here is a diagram showing the MJIT organization:

       _______     _________________
      |header |-->| minimized header|
      |_______|   |_________________|
                    |                         MRI building
      --------------|----------------------------------------
                    |                         MRI execution
     	            |
       _____________|_____
      |             |     |
      |          ___V__   |  CC      ____________________
      |         |      |----------->| precompiled header |
      |         |      |  |         |____________________|
      |         |      |  |              |
      |         | MJIT |  |              |
      |         |      |  |              |
      |         |      |  |          ____V___  CC  __________
      |         |______|----------->| C code |--->| .so file |
      |                   |         |________|    |__________|
      |                   |                              |
      |                   |                              |
      | MRI machine code  |<-----------------------------
      |___________________|             loading


   We don't use SIGCHLD signal and WNOHANG waitpid in MJIT as it
   might mess with ruby code dealing with signals.  Also as SIGCHLD
   signal can be delivered to non-main thread, the stack might have a
   constraint.  So the correct version of code based on SIGCHLD and
   WNOHANG waitpid would be very complicated.

   TODO: ISEQ JIT code unloading.  */

#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <dlfcn.h>
#include "internal.h"
#include "vm_core.h"
#include "iseq.h"
#include "insns.inc"
#include "insns_info.inc"
#include "mjit.h"
#include "version.h"

/* Numbers of the interpreted insns and JIT executed insns when
   MJIT_INSN_STATISTICS is non-zero.  */
unsigned long byte_code_insns_num;
RUBY_SYMBOL_EXPORT_BEGIN
unsigned long jit_insns_num;
RUBY_SYMBOL_EXPORT_END

/* Return time in milliseconds as a double.  */
static double
real_ms_time(void) {
    struct timeval  tv;

    gettimeofday(&tv, NULL);
    return tv.tv_usec / 1000.0 + tv.tv_sec * 1000.0;
}

/* A copy of MJIT portion of MRI options since MJIT initialization.  We
   need them as MJIT threads still can work when the most MRI data were
   freed. */
RUBY_SYMBOL_EXPORT_BEGIN
struct mjit_options mjit_opts;
RUBY_SYMBOL_EXPORT_END

/* Default level of details in the debug info.  */
static int debug_level = 3;

/* The MJIT start time.  */
static double mjit_time_start;

/* Return time in milliseconds as a double relative to MJIT start.  */
static double
relative_ms_time(void) {
    return real_ms_time() - mjit_time_start;
}

/* TRUE if MJIT is initialized and will be used.  */
RUBY_SYMBOL_EXPORT_BEGIN
int mjit_init_p = FALSE;
RUBY_SYMBOL_EXPORT_END

/* PID of major MRI thread which is a client of MJIT. */
static pthread_t client_pid;


/*  A compilation unit is called a batch.  More one ISEQ can belong to
    a batch.  The batch has a status described by the following
    enumeration.

    The diagram shows the possible status transitions:

    NOT_FORMED -> IN_QUEUE -> IN_EXECUTION -> FAILED
                     ^             |            ^
                     |             |            |   load_batch
                     |             |            |
                     |              -------> SUCCESS ----> LOADED
                     |                          |            |
                     |                          V            |
                      ------------------<--------------------
                                change in global speculation
*/

enum batch_status {
    /* The batch is not in the queue.  More ISEQs can be added to it.  */
    BATCH_NOT_FORMED,
    /* The batch is in the queue for compilation.  */
    BATCH_IN_QUEUE,
    /* The batch is being processed by a MJIT worker (C code
       generation, the C code compilation, and the object code
       load). */
    BATCH_IN_EXECUTION,
    /* Batch compilation or its load failed.  */
    BATCH_FAILED,
    /* Batch compilation successfully finished.  */
    BATCH_SUCCESS,
    /* The batch ISEQ machine code was successfully loaded.  */
    BATCH_LOADED,
};

/* State of global speculation.
   TODO: Fine grain flags for different bop redefinitions.  */
struct global_spec_state {
    /* The following flags reflect presence of tracing and basic
       operation redefinitions.  */
    int trace_p:1;
    int bop_redefined_p:1;
};

/* The batch structure.  */
struct rb_mjit_batch {
    int num; /* batch order number */
    enum batch_status status;
    /* Name of the C code file of the batch ISEQs.  Defined for status
       BATCH_IN_EXECUTION.  */
    char *cfname;
    /* Name of the object file of the batch ISEQs.  Defined for status
       BATCH_IN_EXECUTION and BATCH_SUCCESS.  */
    char *ofname;
    /* PID of C compiler processing the batch.  Defined for status
       BATCH_IN_EXECUTION. */
    pid_t pid;
    /* Bathes in the queue are linked with the following members.  */
    struct rb_mjit_batch *next, *prev;
    /* Dlopen handle of the loaded object file.  Defined for status
       BATCH_LOADED.  */
    void *handle;
    /* ISEQs in the batch form a doubly linked list.  The following
       members are the head and the tail of the list.  */
    struct rb_mjit_batch_iseq *first, *last;
    /* Number of non-canceled iseqs in the batch.  */
    int active_iseqs_num;
    /* Overall byte code size of all ISEQs in the batch in VALUEs.  */
    size_t iseqs_size;
    /* The following member is used to generate a code with global
       speculation.  */
    struct global_spec_state spec_state;
    /* The relative time when a worker started to process the
       batch.  It is used in the debug mode.  */
    double time_start;
};

/* Info about insn resulted into a mutation.  */
struct mjit_mutation_insns {
    enum ruby_vminsn_type insn;
    size_t pc; /* the relative insn pc.  */
};

/* The structure describing an ISEQ in the batch.  */
struct rb_mjit_batch_iseq {
    /* Order number of all batch ISEQs.  It is unique even for ISEQs
       from different batches.  */
    int num;
    rb_iseq_t *iseq;
    /* The ISEQ byte code size in VALUEs.  */
    size_t iseq_size;
    struct rb_mjit_batch *batch;
    /* The previous and the next ISEQ in the batch.  */
    struct rb_mjit_batch_iseq *prev, *next;
    /* The fields used for profiling only:  */
    char *label; /* Name of the ISEQ */
    /* Number of iseq calls, number of them in JIT mode, and number of
       JIT calls with speculation failures.  */
    unsigned long overall_calls, jit_calls, failed_jit_calls;
    /* The following flag reflects speculation about equality of ep
       and bp which we used during last iseq translation.  */
    int ep_neq_bp_p:1;
    /* -1 means we speculate that self has few instance variables.
       Positive means we speculate that self has > IVAR_SPEC instance
       variables and IVAR_SPEC > ROBJECT_EMBED_LEN_MAX.  Zero means we
       know nothing about the number.  */
    size_t ivar_spec;
    /* Serial number of self for speculative translations for nonzero
       ivar_spec. */
    rb_serial_t ivar_serial;
    /* True if we use C vars for temporary Ruby variables during last
       iseq translation.  */
    char use_temp_vars_p;
    /* Number of JIT code mutations (and cancellations).  */
    int jit_mutations_num;
    /* Array of structures describing insns which initiated mutations.
       The array has JIT_MUTATIONS_NUM defined elements.  */
    struct mjit_mutation_insns *mutation_insns;
};

/* Defined in the client thread before starting MJIT threads:  */
/* Used C compiler path.  */
static char *cc_path;
/* Name of the header file.  */
static char *header_fname;
/* Name of the precompiled header file.  */
static char *pch_fname;

/* Return length of NULL-terminated array ARGS excluding the NULL
   marker.  */
static size_t
args_len(char *const *args) {
    size_t i;

    for (i = 0; (args[i]) != NULL;i++)
	;
    return i;
}

/* Concatenate NUM passed NULL-terminated arrays of strings, put the
   result (with NULL end marker) into the heap, and return the
   result.  */
static char **
form_args(int num, ...) {
    va_list argp, argp2;
    size_t len, disp;
    int i;
    char **args, **res;

    va_start(argp, num);
    va_copy(argp2, argp);
    for (i = len = 0; i < num; i++) {
	args = va_arg(argp, char **);
	len += args_len(args);
    }
    va_end(argp);
    if ((res = xmalloc((len + 1) * sizeof(char *))) == NULL)
	return NULL;
    for (i = disp = 0; i < num; i++) {
	args = va_arg(argp2, char **);
	len = args_len(args);
	memmove(res + disp, args, len * sizeof(char *));
	disp += len;
    }
    res[disp] = NULL;
    va_end(argp2);
    return res;
}

/* Make and return copy of STR in the heap.  Return NULL in case of a
   failure.  */
static char *
get_string(const char *str) {
    char *res;

    if ((res = xmalloc(strlen(str) + 1)) != NULL)
	strcpy(res, str);
    return res;
}

/* Return an unique file name in /tmp with PREFIX and SUFFIX and
   number ID.  Use getpid if ID == 0.  The return file name exists
   until the next function call.  */
static char *
get_uniq_fname(unsigned long id, const char *prefix, const char *suffix) {
    char str[70];

    if (id == 0)
	sprintf(str, "/tmp/%sp%lu%s", prefix, (unsigned long) getpid(), suffix);
    else
	sprintf(str, "/tmp/%sp%lub%lu%s", prefix, (unsigned long) getpid(), id, suffix);
    return get_string(str);
}

/* Maximum length for C function name generated for an ISEQ.  */
#define MAX_MJIT_FNAME_LEN 30

/* Put C function name of batch iseq BI into HOLDER.  Return
   HOLDER.  */
static char *
get_batch_iseq_fname(struct rb_mjit_batch_iseq *bi, char *holder) {
    sprintf(holder, "_%d", bi->num);
    return holder;
}

/* A mutex for conitionals and critical sections.  */
static pthread_mutex_t mjit_engine_mutex;
/* A thread conditional to wake up workers if at the end of PCH thread.  */
static pthread_cond_t mjit_pch_wakeup;
/* A thread conditional to wake up the client if there is a change in
   executed batch status.  */
static pthread_cond_t mjit_client_wakeup;
/* A thread conditional to wake up a worker if there we have something
   to add or we need to stop MJIT engine.  */
static pthread_cond_t mjit_worker_wakeup;

/* Doubly linked list of batches.  */
struct rb_mjit_batch_list {
    struct rb_mjit_batch *head, *tail;
};

/* The batch queue.  The client and MJIT threads work on the queue.
   So code using the following variable should be synced.  */
static struct rb_mjit_batch_list batch_queue;

/* The client and MJIT threads work on the list of done batches
   (doubly linked list).  So code using the following variable should
   be synced.  */
static struct rb_mjit_batch_list done_batches;

/* The following functions are low level (ignoring thread
   synchronization) functions working with the lists.  */

/* Remove and return a head batch from the head of doubly linked
   LIST.  */
static struct rb_mjit_batch *
get_from_list(struct rb_mjit_batch_list *list) {
    struct rb_mjit_batch *b;

    if ((b = list->head) == NULL)
	return NULL;
    list->head = list->head->next;
    if (list->head == NULL)
	list->tail = NULL;
    else
	list->head->prev = NULL;
    return b;
}

/* Add batch B to the tail of doubly linked LIST.  It should be not in
   the list before.  */
static void
add_to_list(struct rb_mjit_batch *b, struct rb_mjit_batch_list *list) {
    b->next = NULL;
    if (list->head == NULL)
	list->head = list->tail = b;
    else {
	list->tail->next = b;
	b->prev = list->tail;
	list->tail = b;
    }
}

/* Remove batch B from the doubly linked LIST.  It should be in the
   list before.  */
static void
remove_from_list(struct rb_mjit_batch *b, struct rb_mjit_batch_list *list) {
    if (b == list->head)
	list->head = b->next;
    else
	b->prev->next = b->next;
    if (b == list->tail)
	list->tail = b->prev;
    else
	b->next->prev = b->prev;
}

/* Print ARGS according to FORMAT to stderr.  */
static void
va_list_log(const char *format, va_list args) {
    char str[256];

    vsprintf(str, format, args);
    /* Use one call for non-interrupted output:  */
    fprintf(stderr, "%s%s: time - %.3f ms\n",
	    pthread_self() == client_pid ? "" : "+++",
	    str, relative_ms_time());
}

/* Print the arguments according to FORMAT to stderr only if MJIT
   verbose option is on.  */
static void
verbose(const char *format, ...) {
    va_list args;

    va_start(args, format);
    if (mjit_opts.verbose)
	va_list_log(format, args);
    va_end(args);
}

/* Print the arguments according to FORMAT to stderr only if the
   message LEVEL is not greater to the current debug level.  */
static void
debug(int level, const char *format, ...) {
    va_list args;

    if (! mjit_opts.debug || ! mjit_opts.verbose)
	return;
    va_start(args, format);
    if (debug_level >= level)
	va_list_log(format, args);
    va_end(args);
}

/* Start a critical section.  Use message MSG to print debug info.  */
static inline void
CRITICAL_SECTION_START(const char *msg) {
    int err_code;


    debug(3, "Locking %s", msg);
    if ((err_code = pthread_mutex_lock(&mjit_engine_mutex)) != 0) {
	fprintf(stderr, "%sCannot lock MJIT mutex %s: time - %.3f ms\n",
		pthread_self() == client_pid ? "" : "++", msg,
		relative_ms_time());
	fprintf(stderr, "%serror: %s\n",
		pthread_self() == client_pid ? "" : "++", strerror(err_code));
    }
    debug(3, "Locked %s", msg);
}

/* Finish the current critical section.  */
static inline void
CRITICAL_SECTION_FINISH(const char *msg) {
    pthread_mutex_unlock(&mjit_engine_mutex);
    debug(3, "Unlocked %s", msg);
}

/* XXX_COMMONN_ARGS define the command line arguments of XXX C
   compiler used by MJIT.

   XXX_EMIT_PCH_ARGS define additional options to generate the
   precomiled header.

   XXX_USE_PCH_ARAGS define additional options to use the precomiled
   header.  */
static const char *GCC_COMMON_ARGS_DEBUG[] = {"gcc", "-O0", "-g", "-Wfatal-errors", "-fPIC", "-shared", "-w", "-pipe", "-nostartfiles", "-nodefaultlibs", "-nostdlib", NULL};
static const char *GCC_COMMON_ARGS[] = {"gcc", "-O2", "-Wfatal-errors", "-fPIC", "-shared", "-w", "-pipe", "-nostartfiles", "-nodefaultlibs", "-nostdlib", NULL};
static const char *GCC_USE_PCH_ARGS[] = {"-I/tmp", NULL};
static const char *GCC_EMIT_PCH_ARGS[] = {NULL};

#ifdef __MACH__

static const char *LLVM_COMMON_ARGS_DEBUG[] = {"clang", "-O0", "-g", "-dynamic", "-I/usr/local/include", "-L/usr/local/lib", "-w", "-bundle", NULL};
static const char *LLVM_COMMON_ARGS[] = {"clang", "-O2", "-dynamic", "-I/usr/local/include", "-L/usr/local/lib", "-w", "-bundle", NULL};

#else

static const char *LLVM_COMMON_ARGS_DEBUG[] = {"clang", "-O0", "-g", "-fPIC", "-shared", "-I/usr/local/include", "-L/usr/local/lib", "-w", "-bundle", NULL};
static const char *LLVM_COMMON_ARGS[] = {"clang", "-O2", "-fPIC", "-shared", "-I/usr/local/include", "-L/usr/local/lib", "-w", "-bundle", NULL};

#endif /* #if __MACH__ */

static const char *LLVM_USE_PCH_ARGS[] = {"-include-pch", NULL, "-Wl,-undefined", "-Wl,dynamic_lookup", NULL};
static const char *LLVM_EMIT_PCH_ARGS[] = {"-emit-pch", NULL};

/* All or most code for execution of any byte code insn is contained
   in the corresponding C function (see rtl_exec.c).  The following
   structure describes how to use the function (mostly its parameters
   passing) to implement the insn.  */
struct insn_fun_features {
    /* Pass argument th.  */
    char th_p;
    /* True if the first insn operand is a continuation insn (see
       comments of insns.def).  We don't pass such operands.  */
    char skip_first_p;
    /* Just go to spec section if the function returns non-zero.  */
    char op_end_p;
    /* Pass structure calling and call function mjit_call_method
       afterwards.  */
    char call_p;
    /* Defined only for call insns.  True if the call recv should be
       present on the stack.  */
    char recv_p;
    /* Jump the dest (1st of 2nd insn operand) if the function returns
       non-zero.  */
    char jmp_p;
    /* It is a bcmp insn, call function mjit_bcmp_end if it is
       necessary.  */
    char bcmp_p;
    /* A value passed to function jmp_bcmp_end.  */
    char jmp_true_p;
    /* Use a separate code to generate C code for the insn.  */
    char special_p;
    /* Flag of an insn which can become a speculative one.  */
    char changing_p;
    /* Flag of a speculative insn.  */
    char speculative_p;
};

/* Initiate S with no speculation.  */
static void
init_global_spec_state(struct global_spec_state *s) {
    s->trace_p = s->bop_redefined_p = TRUE;
}

/* Set up S to the currently possible speculation state.  */
static void
setup_global_spec_state(struct global_spec_state *s) {
    int i;
    
    s->trace_p = ruby_vm_event_flags != 0;
    s->bop_redefined_p = FALSE;
    for (i = 0; i < BOP_LAST_; i++)
	if (GET_VM()->redefined_flag[i] != 0) {
	    s->bop_redefined_p = TRUE;
	    break;
	}
}

/* Return TRUE if speculations described by S can be used in
   CURR_STATE.  */
static int
valid_global_spec_state_p(const struct global_spec_state *s,
			  const struct global_spec_state *curr_state) {
    return ((s->trace_p || ! curr_state->trace_p)
	    && (s->bop_redefined_p || ! curr_state->bop_redefined_p));
}

/*-------All the following code is executed in the worker threads only-----------*/

/* Return features of C function corresponding to the byte code INSN
   through F.  */
static void
get_insn_fun_features(VALUE insn, struct insn_fun_features *f) {
    f->th_p = f->skip_first_p = f->op_end_p = f->bcmp_p = FALSE;
    f->call_p = f->recv_p = f->jmp_p = f->jmp_true_p = FALSE;
    f->special_p = f->changing_p = f->speculative_p = FALSE;
    switch (insn) {
    case BIN(const2var):
    case BIN(const_ld_val):
    case BIN(const_cached_val_ld):
    case BIN(special2var):
    case BIN(var2special):
    case BIN(run_once):
    case BIN(define_class):
    case BIN(defined_p):
    case BIN(val_defined_p):
	f->th_p = TRUE;
	break;
    case BIN(simple_call):
    case BIN(call):
    case BIN(vmcore_call):
    case BIN(call_super):
	f->recv_p = TRUE;
	/* Fall through.  */
    case BIN(simple_call_self):
    case BIN(simple_call_recv):
    case BIN(call_self):
    case BIN(call_recv):
	f->th_p = f->call_p = TRUE;
	break;
    case BIN(length):
    case BIN(size):
    case BIN(empty_p):
    case BIN(succ):
    case BIN(not):
    case BIN(plus):
    case BIN(minus):
    case BIN(mult):
    case BIN(div):
    case BIN(mod):
    case BIN(ltlt):
    case BIN(ind):
    case BIN(eq):
    case BIN(ne):
    case BIN(lt):
    case BIN(gt):
    case BIN(le):
    case BIN(ge):
    case BIN(plusi):
    case BIN(plusf):
    case BIN(minusi):
    case BIN(minusf):
    case BIN(multi):
    case BIN(multf):
    case BIN(divi):
    case BIN(divf):
    case BIN(modi):
    case BIN(modf):
    case BIN(ltlti):
    case BIN(indi):
    case BIN(inds):
    case BIN(eqi):
    case BIN(eqf):
    case BIN(nei):
    case BIN(nef):
    case BIN(lti):
    case BIN(ltf):
    case BIN(gti):
    case BIN(gtf):
    case BIN(lei):
    case BIN(lef):
    case BIN(gei):
    case BIN(gef):
	f->changing_p = TRUE;
	/* fall through: */
    case BIN(uplus):
    case BIN(uminus):
    case BIN(umult):
    case BIN(udiv):
    case BIN(umod):
    case BIN(ueq):
    case BIN(une):
    case BIN(ult):
    case BIN(ugt):
    case BIN(ule):
    case BIN(uge):
    case BIN(uplusi):
    case BIN(uplusf):
    case BIN(uminusi):
    case BIN(uminusf):
    case BIN(umulti):
    case BIN(umultf):
    case BIN(udivi):
    case BIN(udivf):
    case BIN(umodi):
    case BIN(umodf):
    case BIN(ueqi):
    case BIN(ueqf):
    case BIN(unei):
    case BIN(unef):
    case BIN(ulti):
    case BIN(ultf):
    case BIN(ugti):
    case BIN(ugtf):
    case BIN(ulei):
    case BIN(ulef):
    case BIN(ugei):
    case BIN(ugef):
    case BIN(regexp_match2):
    case BIN(uind):
    case BIN(uindi):
    case BIN(uinds):
	f->th_p = f->skip_first_p = f->op_end_p = TRUE;
	break;
    case BIN(iplus):
    case BIN(iminus):
    case BIN(imult):
    case BIN(idiv):
    case BIN(imod):
    case BIN(aind):
    case BIN(hind):
    case BIN(ieq):
    case BIN(ine):
    case BIN(ilt):
    case BIN(igt):
    case BIN(ile):
    case BIN(ige):
    case BIN(fplus):
    case BIN(fminus):
    case BIN(fmult):
    case BIN(fdiv):
    case BIN(fmod):
    case BIN(feq):
    case BIN(fne):
    case BIN(flt):
    case BIN(fgt):
    case BIN(fle):
    case BIN(fge):
    case BIN(iplusi):
    case BIN(iminusi):
    case BIN(imulti):
    case BIN(idivi):
    case BIN(imodi):
    case BIN(aindi):
    case BIN(hindi):
    case BIN(hinds):
    case BIN(ieqi):
    case BIN(inei):
    case BIN(ilti):
    case BIN(igti):
    case BIN(ilei):
    case BIN(igei):
    case BIN(fplusf):
    case BIN(fminusf):
    case BIN(fmultf):
    case BIN(fdivf):
    case BIN(fmodf):
    case BIN(feqf):
    case BIN(fnef):
    case BIN(fltf):
    case BIN(fgtf):
    case BIN(flef):
    case BIN(fgef):
	f->skip_first_p = f->speculative_p = TRUE;
	break;
    case BIN(indset):
    case BIN(indseti):
    case BIN(indsets):
	f->changing_p = TRUE;
	/* fall through: */
    case BIN(uindset):
    case BIN(uindseti):
    case BIN(uindsets):
	f->th_p = f->op_end_p = TRUE;
	break;
    case BIN(aindset):
    case BIN(hindset):
    case BIN(aindseti):
    case BIN(hindseti):
    case BIN(hindsets):
	f->speculative_p = TRUE;
	break;
    case BIN(trace):
    case BIN(goto):
    case BIN(get_inline_cache):
    case BIN(case_dispatch):
	f->special_p = TRUE;
	break;
    case BIN(bt):
    case BIN(bf):
    case BIN(bnil):
    case BIN(bkw):
	f->th_p = f->jmp_p = TRUE;
	break;
    case BIN(bteq):
    case BIN(btne):
    case BIN(btlt):
    case BIN(btgt):
    case BIN(btle):
    case BIN(btge):
    case BIN(bteqi):
    case BIN(bteqf):
    case BIN(btnei):
    case BIN(btnef):
    case BIN(btlti):
    case BIN(btltf):
    case BIN(btgti):
    case BIN(btgtf):
    case BIN(btlei):
    case BIN(btlef):
    case BIN(btgei):
    case BIN(btgef):
	f->changing_p = TRUE;
	/* fall through: */
    case BIN(ubteq):
    case BIN(ubtne):
    case BIN(ubtlt):
    case BIN(ubtgt):
    case BIN(ubtle):
    case BIN(ubtge):
    case BIN(ubteqi):
    case BIN(ubteqf):
    case BIN(ubtnei):
    case BIN(ubtnef):
    case BIN(ubtlti):
    case BIN(ubtltf):
    case BIN(ubtgti):
    case BIN(ubtgtf):
    case BIN(ubtlei):
    case BIN(ubtlef):
    case BIN(ubtgei):
    case BIN(ubtgef):
	f->th_p = f->jmp_p = f->jmp_true_p = f->bcmp_p = f->skip_first_p = TRUE;
	break;
    case BIN(bfeq):
    case BIN(bfne):
    case BIN(bflt):
    case BIN(bfgt):
    case BIN(bfle):
    case BIN(bfge):
    case BIN(bfeqi):
    case BIN(bfeqf):
    case BIN(bfnei):
    case BIN(bfnef):
    case BIN(bflti):
    case BIN(bfltf):
    case BIN(bfgti):
    case BIN(bfgtf):
    case BIN(bflei):
    case BIN(bflef):
    case BIN(bfgei):
    case BIN(bfgef):
	/* fall through: */
	f->changing_p = TRUE;
    case BIN(ubfeq):
    case BIN(ubfne):
    case BIN(ubflt):
    case BIN(ubfgt):
    case BIN(ubfle):
    case BIN(ubfge):
    case BIN(ubfeqi):
    case BIN(ubfeqf):
    case BIN(ubfnei):
    case BIN(ubfnef):
    case BIN(ubflti):
    case BIN(ubfltf):
    case BIN(ubfgti):
    case BIN(ubfgtf):
    case BIN(ubflei):
    case BIN(ubflef):
    case BIN(ubfgei):
    case BIN(ubfgef):
	f->th_p = f->jmp_p = f->bcmp_p = f->skip_first_p = TRUE;
	break;
    case BIN(ibteq):
    case BIN(ibtne):
    case BIN(ibtlt):
    case BIN(ibtgt):
    case BIN(ibtle):
    case BIN(ibtge):
    case BIN(fbteq):
    case BIN(fbtne):
    case BIN(fbtlt):
    case BIN(fbtgt):
    case BIN(fbtle):
    case BIN(fbtge):
    case BIN(ibteqi):
    case BIN(ibtnei):
    case BIN(ibtlti):
    case BIN(ibtgti):
    case BIN(ibtlei):
    case BIN(ibtgei):
    case BIN(fbteqf):
    case BIN(fbtnef):
    case BIN(fbtltf):
    case BIN(fbtgtf):
    case BIN(fbtlef):
    case BIN(fbtgef):
	f->jmp_true_p = TRUE;
	/* fall through: */
    case BIN(ibfeq):
    case BIN(ibfne):
    case BIN(ibflt):
    case BIN(ibfgt):
    case BIN(ibfle):
    case BIN(ibfge):
    case BIN(fbfeq):
    case BIN(fbfne):
    case BIN(fbflt):
    case BIN(fbfgt):
    case BIN(fbfle):
    case BIN(fbfge):
    case BIN(ibfeqi):
    case BIN(ibfnei):
    case BIN(ibflti):
    case BIN(ibfgti):
    case BIN(ibflei):
    case BIN(ibfgei):
    case BIN(fbfeqf):
    case BIN(fbfnef):
    case BIN(fbfltf):
    case BIN(fbfgtf):
    case BIN(fbflef):
    case BIN(fbfgef):
	f->jmp_p = f->bcmp_p = f->skip_first_p = f->speculative_p = TRUE;
	break;
    case BIN(nop):
    case BIN(temp_ret):
    case BIN(loc_ret):
    case BIN(val_ret):
    case BIN(raise_except):
    case BIN(raise_except_val):
    case BIN(ret_to_loc):
    case BIN(ret_to_temp):
    case BIN(call_block):
	f->call_p = f->special_p = TRUE;
	break;
    case BIN(var2ivar):
    case BIN(val2ivar):
    case BIN(ivar2var):
    case BIN(var2var):
    case BIN(var_swap):
    case BIN(temp2temp):
    case BIN(temp_swap):
    case BIN(loc2loc):
    case BIN(loc2temp):
    case BIN(temp2loc):
    case BIN(uploc2temp):
    case BIN(uploc2var):
    case BIN(val2temp):
    case BIN(val2loc):
    case BIN(str2var):
    case BIN(set_inline_cache):
    case BIN(specialobj2var):
    case BIN(self2var):
    case BIN(global2var):
    case BIN(cvar2var):
    case BIN(iseq2var):
    case BIN(var2uploc):
    case BIN(val2uploc):
    case BIN(var2const):
    case BIN(var2global):
    case BIN(var2cvar):
    case BIN(make_range):
    case BIN(make_array):
    case BIN(make_hash):
    case BIN(new_array_min):
    case BIN(new_array_max):
    case BIN(clone_array):
    case BIN(spread_array):
    case BIN(splat_array):
    case BIN(concat_array):
    case BIN(check_match):
    case BIN(regexp_match1):
    case BIN(to_string):
    case BIN(concat_strings):
    case BIN(to_regexp):
    case BIN(str_freeze_call):
    case BIN(freeze_string):
	break;
    case BIN(call_c_func):
	/* C code for the following insns should be never
	   generated.  */
    case BIN(cont_op1):
    case BIN(cont_op2):
    case BIN(cont_btcmp):
    case BIN(cont_bfcmp):
    default:
	fprintf(stderr, "Not implemented %s\n", insn_name(insn));
	abort();
	break;
    }
}

/* Return a safe version of INSN.  */
static VALUE
get_safe_insn(VALUE insn) {
    switch (insn) {
    case BIN(plus): case BIN(iplus): case BIN(fplus): return BIN(uplus);
    case BIN(minus): case BIN(iminus): case BIN(fminus): return BIN(uminus);
    case BIN(mult): case BIN(imult): case BIN(fmult): return BIN(umult);
    case BIN(div): case BIN(idiv): case BIN(fdiv): return BIN(udiv);
    case BIN(mod): case BIN(imod): case BIN(fmod): return BIN(umod);
    case BIN(eq): case BIN(ieq): case BIN(feq): return BIN(ueq);
    case BIN(ne): case BIN(ine): case BIN(fne): return BIN(une);
    case BIN(lt): case BIN(ilt): case BIN(flt): return BIN(ult);
    case BIN(gt): case BIN(igt): case BIN(fgt): return BIN(ugt);
    case BIN(le): case BIN(ile): case BIN(fle): return BIN(ule);
    case BIN(ge): case BIN(ige): case BIN(fge): return BIN(uge);
    case BIN(plusi): case BIN(iplusi): return BIN(uplusi);
    case BIN(minusi): case BIN(iminusi): return BIN(uminusi);
    case BIN(multi): case BIN(imulti): return BIN(umulti);
    case BIN(divi): case BIN(idivi): return BIN(udivi);
    case BIN(modi): case BIN(imodi): return BIN(umodi);
    case BIN(eqi): case BIN(ieqi): return BIN(ueqi);
    case BIN(nei): case BIN(inei): return BIN(unei);
    case BIN(lti): case BIN(ilti): return BIN(ulti);
    case BIN(gti): case BIN(igti): return BIN(ugti);
    case BIN(lei): case BIN(ilei): return BIN(ulei);
    case BIN(gei): case BIN(igei): return BIN(ugei);
    case BIN(plusf): case BIN(fplusf): return BIN(uplusf);
    case BIN(minusf): case BIN(fminusf): return BIN(uminusf);
    case BIN(multf): case BIN(fmultf): return BIN(umultf);
    case BIN(divf): case BIN(fdivf): return BIN(udivf);
    case BIN(modf): case BIN(fmodf): return BIN(umodf);
    case BIN(eqf): case BIN(feqf): return BIN(ueqf);
    case BIN(nef): case BIN(fnef): return BIN(unef);
    case BIN(ltf): case BIN(fltf): return BIN(ultf);
    case BIN(gtf): case BIN(fgtf): return BIN(ugtf);
    case BIN(lef): case BIN(flef): return BIN(ulef);
    case BIN(gef): case BIN(fgef): return BIN(ugef);
    case BIN(bteq): case BIN(ibteq): case BIN(fbteq): return BIN(ubteq);
    case BIN(btne): case BIN(ibtne): case BIN(fbtne): return BIN(ubtne);
    case BIN(btlt): case BIN(ibtlt): case BIN(fbtlt): return BIN(ubtlt);
    case BIN(btgt): case BIN(ibtgt): case BIN(fbtgt): return BIN(ubtgt);
    case BIN(btle): case BIN(ibtle): case BIN(fbtle): return BIN(ubtle);
    case BIN(btge): case BIN(ibtge): case BIN(fbtge): return BIN(ubtge);
    case BIN(bteqi): case BIN(ibteqi): return BIN(ubteqi);
    case BIN(btnei): case BIN(ibtnei): return BIN(ubtnei);
    case BIN(btlti): case BIN(ibtlti): return BIN(ubtlti);
    case BIN(btgti): case BIN(ibtgti): return BIN(ubtgti);
    case BIN(btlei): case BIN(ibtlei): return BIN(ubtlei);
    case BIN(btgei): case BIN(ibtgei): return BIN(ubtgei);
    case BIN(bteqf): case BIN(fbteqf): return BIN(ubteqf);
    case BIN(btnef): case BIN(fbtnef): return BIN(ubtnef);
    case BIN(btltf): case BIN(fbtltf): return BIN(ubtltf);
    case BIN(btgtf): case BIN(fbtgtf): return BIN(ubtgtf);
    case BIN(btlef): case BIN(fbtlef): return BIN(ubtlef);
    case BIN(btgef): case BIN(fbtgef): return BIN(ubtgef);
    case BIN(bfeq): case BIN(ibfeq): case BIN(fbfeq): return BIN(ubfeq);
    case BIN(bfne): case BIN(ibfne): case BIN(fbfne): return BIN(ubfne);
    case BIN(bflt): case BIN(ibflt): case BIN(fbflt): return BIN(ubflt);
    case BIN(bfgt): case BIN(ibfgt): case BIN(fbfgt): return BIN(ubfgt);
    case BIN(bfle): case BIN(ibfle): case BIN(fbfle): return BIN(ubfle);
    case BIN(bfge): case BIN(ibfge): case BIN(fbfge): return BIN(ubfge);
    case BIN(bfeqi): case BIN(ibfeqi): return BIN(ubfeqi);
    case BIN(bfnei): case BIN(ibfnei): return BIN(ubfnei);
    case BIN(bflti): case BIN(ibflti): return BIN(ubflti);
    case BIN(bfgti): case BIN(ibfgti): return BIN(ubfgti);
    case BIN(bflei): case BIN(ibflei): return BIN(ubflei);
    case BIN(bfgei): case BIN(ibfgei): return BIN(ubfgei);
    case BIN(bfeqf): case BIN(fbfeqf): return BIN(ubfeqf);
    case BIN(bfnef): case BIN(fbfnef): return BIN(ubfnef);
    case BIN(bfltf): case BIN(fbfltf): return BIN(ubfltf);
    case BIN(bfgtf): case BIN(fbfgtf): return BIN(ubfgtf);
    case BIN(bflef): case BIN(fbflef): return BIN(ubflef);
    case BIN(bfgef): case BIN(fbfgef): return BIN(ubfgef);
    default:
	return insn;
    }
}

/* Describes parameters affecting ISEQ compilation.  The parameters
   are calculated for every ISEQ compilation.  */
struct translation_control {
    /* True if we should not speculative insns.  */
    char safe_p;
    /* True to use C local variables for ISEQ RTL local and temporary
       variables.  */
    char use_local_vars_p;
    char use_temp_vars_p;
};

/* Return C code string representing address of local/temporary
   variable with index IND.  TCP defines where values of the ISEQ
   local/temporary variables will be kept inside C function
   representing the ISEQ.  */
static const char *
get_op_str(char *buf, ptrdiff_t ind, struct translation_control *tcp) {
    if (ind < 0) {
	if (tcp->use_temp_vars_p)
	    sprintf(buf, "&t%ld", (long) -ind - 1);
	else
	    sprintf(buf, "get_temp_addr(cfp, %ld)", (long) ind);
    } else if (! tcp->use_local_vars_p) {
	sprintf(buf, "get_loc_addr(cfp, %ld)", (long) ind);
    } else {
	sprintf(buf, "&v%ld", (long) ind - VM_ENV_DATA_SIZE);
    }
    return buf;
}

/* Move args of a call insn in code with position POS to the MRI
   stack.  Reserve a stack slot for the call receiver if RECV_P.  */
static void
generate_param_setup(FILE *f, const VALUE *code, size_t pos, int recv_p) {
    /* Always the 1st call insn operand.  */
    CALL_DATA cd = (CALL_DATA) code[pos + 1];
    /* Always the 2nd call insn operand.  */
    int call_start = -(int) (ptrdiff_t) code[pos + 2];
    int i, args_num = cd->call_info.orig_argc;

    if (cd->call_info.flag & VM_CALL_ARGS_BLOCKARG)
	args_num++;
    for (i = !recv_p; i <= args_num; i++)
	fprintf(f, "  *get_temp_addr(cfp, %d) = t%d;\n", -call_start - i, call_start + i - 1);
}

/* Return a string which is C assignment of failed_insn_pc of insn
   with POS.  Use BUF as a string container.  */
static const char *
set_failed_insn_str(char *buf, size_t pos) {
    sprintf(buf, "failed_insn_pc = %lu; ", pos);
    return buf;
}

/* If SET_P is FALSE, return empty string.  Otherwise, return string
   representing C code setting cfp pc to PC.  Use BUF as the string
   container.  */
static const char *
generate_set_pc(int set_p, char *buf, const VALUE *pc) {
    if (! set_p)
	return "";
    sprintf(buf, "  cfp->pc = (void *) 0x%"PRIxVALUE ";\n", (VALUE) pc);
    return buf;
}

/* Return number of mutations which insn with POS from BI iseq
   caused.  */
static int
get_insn_mutation_num(struct rb_mjit_batch_iseq *bi, size_t pos) {
    int i, num = 0;
    
    for (i = 0; i < bi->jit_mutations_num; i++)
	if (bi->mutation_insns[i].insn != BIN(nop) && pos == bi->mutation_insns[i].pc)
	    num++;
    return num;
}

/* An aditional argument to generate cases for values in case_dispatch
   insn hash.  */
struct case_arg {
    /* iseq basic offset for destinations in case_dispatch  */
    size_t offset;
    FILE *f; /* a file where to print */
};

/* Print a case for destination VAL in case_dispatch insn.  An
   additional info to do this is given by ARG. */
static int
print_case(st_data_t key, st_data_t val, st_data_t arg) {
    struct case_arg *case_arg = (struct case_arg *) arg;

    fprintf(case_arg->f, "    case %ld: goto l%ld;\n",
	    FIX2LONG(val), case_arg->offset + FIX2LONG(val));
    return ST_CONTINUE;
}

/* Output C code implementing an iseq BI insn starting with position
   POS to file F.  Generate the code according to TCP.  */
static int
translate_iseq_insn(FILE *f, size_t pos, struct rb_mjit_batch_iseq *bi,
		    struct translation_control *tcp) {
    rb_iseq_t *iseq = bi->iseq;
    const VALUE *code = iseq->body->iseq_encoded;
    VALUE insn, op;
    int len, i, ivar_p, const_p, insn_mutation_num;
    const char *types;
    const char *iname;
    struct insn_fun_features features;
    struct rb_call_cache local_cc;
    struct iseq_inline_cache_entry local_ic;
    CALL_CACHE cc = NULL;
    CALL_INFO ci = NULL;
    char buf[150];

    insn_mutation_num = get_insn_mutation_num(bi, pos);
    insn = code[pos];
#if OPT_DIRECT_THREADED_CODE || OPT_CALL_THREADED_CODE
    insn = rb_vm_insn_addr2insn((void *) insn);
#endif
    if (tcp->safe_p || insn_mutation_num != 0)
	insn = get_safe_insn(insn);
    len = insn_len(insn);
    types = insn_op_types(insn);
    iname = insn_name(insn);
    fprintf(f, "l%ld:\n", pos);
    if (mjit_opts.debug)
	fprintf(f, "  /* %s:%u */\n", bi->label, rb_iseq_line_no(iseq, pos));
#if MJIT_INSN_STATISTICS
    fprintf(f, "  jit_insns_num++;\n");
#endif
    get_insn_fun_features(insn, &features);
    ivar_p = const_p = FALSE;
    fprintf(f, "%s", generate_set_pc(TRUE, buf, &code[pos] + len));
    if (features.call_p && ! features.special_p) {
	/* CD is always the 1st operand.  */
	cc = &((CALL_DATA) code[pos + 1])->call_cache;
	ci = &((CALL_DATA) code[pos + 1])->call_info;
	/* Remember cc can change in the interpreter thread in
	   parallel.  TODO: Make the following atomic.  */
	local_cc = *cc;
    } else if (insn == BIN(ivar2var) || insn == BIN(var2ivar) || insn == BIN(val2ivar)) {
	ivar_p = TRUE;
	local_ic = *(IC) (insn == BIN(ivar2var) ? code[pos + 3] : code[pos + 2]);
    } else if (insn == BIN(const_cached_val_ld) || insn == BIN(get_inline_cache)) {
	const_p = TRUE;
	local_ic = *(IC) (insn == BIN(get_inline_cache) ? code[pos + 3] : code[pos + 4]);
    }
    if (features.special_p) {
	switch (insn) {
	case BIN(nop):
	    break;
	case BIN(goto):
	    fprintf(f, "  ruby_vm_check_ints(th);\n");
	    fprintf(f, "  goto l%ld;\n", pos + len + code[pos + 1]);
	    break;
	case BIN(get_inline_cache): {
	    unsigned long dest = pos + len + code[pos + 1];

	    assert(const_p);
	    if (tcp->safe_p || insn_mutation_num != 0) {
		fprintf(f, "  if (%s_f(cfp, %s, (lindex_t) %"PRIdVALUE " , (void *) 0x%"PRIxVALUE "))\n  ",
			iname, get_op_str(buf, code[pos + 2], tcp), code[pos + 2], code[pos + 3]);
	    } else {
		fprintf(f, "  if (mjit_get_inline_cache(cfp, %llu, %llu, 0x%"PRIxVALUE ", %s, %ld",
			(unsigned long long) local_ic.ic_serial,
			(unsigned long long) local_ic.ic_cref, local_ic.ic_value.value,
			get_op_str(buf, code[pos + 2], tcp), code[pos + 2]);
		fprintf(f, ")) {\n  %s", generate_set_pc(TRUE, buf, &code[pos]));
		fprintf(f, "    %sgoto stop_spec;\n  }\n", set_failed_insn_str(buf, pos));
	    }
	    fprintf(f, "  goto l%ld;\n", dest);
	    break;
	}
	case BIN(trace):
	    fprintf(f, "  %s_f(th, cfp, %"PRIdVALUE ");\n", iname, code[pos + 1]);
	    break;
	case BIN(case_dispatch): {
	    CDHASH hash = code[pos + 2];
	    struct case_arg arg;
	    
	    fprintf(f, "  switch (case_dispatch_f(cfp, %s, 0x%"PRIxVALUE ", %ld)) {\n",
		    get_op_str(buf, code[pos + 1], tcp), (VALUE) hash, code[pos + 3]);
	    fprintf(f, "    case 0: break;\n");
	    arg.offset = pos + len;
	    arg.f = f;
	    st_foreach(RHASH_TBL_RAW(hash), print_case, (st_data_t) &arg);  
	    fprintf(f, "  }\n");
	    break;
	}
	case BIN(temp_ret):
	    fprintf(f, "  %s_f(th, cfp, %s, %"PRIuVALUE ", &val);\n  return val;\n",
		    iname, get_op_str(buf, code[pos + 1], tcp), code[pos + 2]);
	    break;
	case BIN(loc_ret):
	    fprintf(f, "  %s_f(th, cfp, %s, %"PRIuVALUE ", &val);\n  return val;\n",
		    iname, get_op_str(buf, code[pos + 1], tcp), code[pos + 2]);
	    break;
	case BIN(val_ret):
	    fprintf(f, "  %s_f(th, cfp, %"PRIuVALUE ", %"PRIuVALUE ", &val);\n  return val;\n",
		    iname, code[pos + 1], code[pos + 2]);
	    break;
	case BIN(raise_except):
	    fprintf(f, "  val = %s_f(th, cfp, %s, %"PRIuVALUE ");\n",
		    iname, get_op_str(buf, code[pos + 1], tcp), code[pos + 2]);
	    fprintf(f, "  th->errinfo = val; rb_threadptr_tag_jump(th, th->state);\n");
	    break;
	case BIN(raise_except_val):
	    fprintf(f, "  val = %s_f(th, cfp, 0x%"PRIxVALUE ", %"PRIuVALUE ");\n",
		    iname, code[pos + 1], code[pos + 2]);
	    fprintf(f, "  th->errinfo = val; rb_threadptr_tag_jump(th, th->state);\n");
	    break;
	case BIN(ret_to_loc):
	case BIN(ret_to_temp):
	    fprintf(f, "  %s_f(th, cfp, (sindex_t) %"PRIdVALUE ", %s);\n  return RUBY_Qnil;\n",
		    iname, code[pos + 1], get_op_str(buf, code[pos + 2], tcp));
	    break;
	case BIN(call_block):
	    /* Generate copying temps to the stack.  */
	    if (tcp->use_temp_vars_p)
		generate_param_setup(f, code, pos, TRUE);
	    fprintf(f, "  val = %s_f(th, cfp, (void *) 0x%"PRIxVALUE ", (sindex_t) %"PRIdVALUE ");\n",
	            iname, code[pos + 1], code[pos + 2]);
	    fprintf(f, "  if (mjit_call_block_end(th, cfp, val, %s)) {\n",
	            get_op_str(buf, code[pos + 2], tcp));
	    fprintf(f, "    %sgoto stop_spec;\n  }\n", set_failed_insn_str(buf, pos));
	    break;
	default:
	    break;
	}
    } else if (features.call_p && !tcp->safe_p && insn_mutation_num == 0
	       && (local_cc.call == vm_call_cfunc || vm_call_iseq_setup_normal_p(local_cc.call))) {
	int simple_p = (insn == BIN(simple_call)
			|| insn == BIN(simple_call_self) || insn == BIN(simple_call_recv));
	int self_p = insn == BIN(call_self) || insn == BIN(simple_call_self);
	ptrdiff_t call_start = code[pos + 2];
	ptrdiff_t recv_op = (insn == BIN(call_recv)
			     ? (ptrdiff_t) code[pos + 4] : insn == BIN(simple_call_recv)
			     ? (ptrdiff_t) code[pos + 3] : call_start);
	VALUE block_iseq = simple_p ? 0 : code[pos + 3];
	const char *rec;
	
	if (tcp->use_temp_vars_p)
	    /* Generate copying temps to the stack.  */
	    generate_param_setup(f, code, pos, features.recv_p);
	rec = (self_p ? "&cfp->self" : get_op_str(buf, recv_op, tcp));
	fprintf(f, "  if (mjit_check_cc_attr_p(*%s, %llu, %llu)) {\n",
		rec, (unsigned long long) local_cc.method_state,
		(unsigned long long) local_cc.class_serial);
	fprintf(f, "  %s", generate_set_pc(TRUE, buf, &code[pos]));
	fprintf(f, "    %sgoto stop_spec;\n  }\n", set_failed_insn_str(buf, pos));
	rec = (self_p ? "&cfp->self" : get_op_str(buf, recv_op, tcp));
	if (local_cc.call == vm_call_cfunc) {
	    fprintf(f, "  if (mjit_call_cfunc(th, cfp, %llu, (void *) 0x%"PRIxVALUE
		    ", %u, %d, 0x%x, (void *) 0x%"PRIxVALUE
		    ", %ld, (void *) 0x%"PRIxVALUE ", *%s, %d, %d",
		    (unsigned long long) ci->mid, (VALUE) local_cc.me,
		    iseq->body->temp_vars_num, ci->orig_argc, ci->flag, (VALUE) &((struct rb_call_data_with_kwarg *)ci)->kw_arg,
		    call_start, block_iseq, rec, !features.recv_p, simple_p);
	} else {
	    const rb_iseq_t *callee_iseq = rb_iseq_check(local_cc.me->def->body.iseq.iseqptr);
	    struct rb_iseq_constant_body *callee_body = callee_iseq->body;

	    fprintf(f, "  if (mjit_iseq_call(th, cfp, (void *) 0x%"PRIxVALUE ", (void *) 0x%"PRIxVALUE
		    ", (void *) 0x%"PRIxVALUE ", (void *) 0x%"PRIxVALUE
		    ", %d, %d, %d, %d, %u, %d, 0x%x, %ld, (void *) 0x%"PRIxVALUE
		    ", *%s, %d, %d",
		    (VALUE) local_cc.me, (VALUE) callee_iseq, (VALUE) callee_body, (VALUE) callee_body->iseq_encoded,
		    callee_body->type, callee_body->param.size, callee_body->local_table_size,
		    iseq->body->temp_vars_num, callee_body->stack_max,
		    ci->orig_argc, ci->flag, call_start, block_iseq,
		    rec, !features.recv_p, simple_p);
	}
	fprintf(f, ", %s)) {\n", get_op_str(buf, call_start, tcp));
	fprintf(f, "    %sgoto stop_spec;\n  }\n", set_failed_insn_str(buf, pos));
    } else if (!tcp->safe_p && features.call_p && insn_mutation_num == 0
	       && (local_cc.call == vm_call_ivar || local_cc.call == vm_call_attrset)
	       && local_cc.aux.index > 0) {
	ptrdiff_t call_start = code[pos + 2];
	int call_ivar_obj_op = features.recv_p ? code[pos + 2] : code[pos + 3];
	const char *rec = (insn == BIN(simple_call_self)
			   ? "&cfp->self" : get_op_str(buf, call_ivar_obj_op, tcp));

	assert(insn == BIN(simple_call_recv) || insn == BIN(simple_call) || insn == BIN(simple_call_self));
	fprintf(f, "  if (mjit_check_cc_attr_p(*%s, %llu, %llu) || ",
		rec, (unsigned long long) local_cc.method_state,
		(unsigned long long) local_cc.class_serial);
	if (local_cc.call == vm_call_ivar) {
	    fprintf(f, "mjit_call_ivar(*%s, %u, ", rec, (unsigned) local_cc.aux.index);
	    fprintf(f, "%s)) {\n", get_op_str(buf, call_start, tcp));
	} else {
	    fprintf(f, "mjit_call_setivar(*%s, %u, ", rec, (unsigned) local_cc.aux.index);
	    fprintf(f, "*%s)) {\n", get_op_str(buf, call_start - 1, tcp));
	}
	fprintf(f, "  %s", generate_set_pc(TRUE, buf, &code[pos]));
	fprintf(f, "    %sgoto stop_spec;\n  }\n", set_failed_insn_str(buf, pos));
    } else if (!tcp->safe_p && ivar_p && insn_mutation_num == 0) {
	assert(insn == BIN(ivar2var) || insn == BIN(var2ivar) || insn == BIN(val2ivar));
	if (bi->ivar_spec != 0) {
	    if (insn == BIN(ivar2var))
		fprintf(f, "  mjit_ivar2var_no_check(cfp, self, %d, %llu, %s, %ld);\n",
			bi->ivar_spec != (size_t) -1, (unsigned long long) local_ic.ic_value.index,
			get_op_str(buf, code[pos + 1], tcp), code[pos + 1]);
	    else {
		fprintf(f, "  mjit_%s_no_check(cfp, self, %d, %llu, ",
			iname, bi->ivar_spec != (size_t) -1, (unsigned long long) local_ic.ic_value.index);
		if (insn == BIN(var2ivar))
		    fprintf (f, "%s", get_op_str(buf, code[pos + 3], tcp));
		else
		    fprintf (f, "0x%"PRIxVALUE, code[pos + 3]);
		fprintf (f, ");\n");
	    }
	} else {
	    if (insn == BIN(ivar2var))
		fprintf(f, "  if (mjit_ivar2var(cfp, self, %d, %llu, %llu, %s, %ld",
			iseq->body->in_type_object_p, (unsigned long long) local_ic.ic_serial,
			(unsigned long long) local_ic.ic_value.index,
			get_op_str(buf, code[pos + 1], tcp), code[pos + 1]);
	    else {
		fprintf(f, "  if (mjit_%s(cfp, self, %d, %llu, %llu, ",
			iname, iseq->body->in_type_object_p, (unsigned long long) local_ic.ic_serial,
			(unsigned long long) local_ic.ic_value.index);
		if (insn == BIN(var2ivar))
		    fprintf (f, "%s", get_op_str(buf, code[pos + 3], tcp));
		else
		    fprintf (f, "0x%"PRIxVALUE, code[pos + 3]);
	    }
	    fprintf(f, ")) {\n  %s", generate_set_pc(TRUE, buf, &code[pos]));
	    fprintf(f, "    %sgoto stop_spec;\n  }\n", set_failed_insn_str(buf, pos));
	}
    } else if (!tcp->safe_p && const_p && insn_mutation_num == 0 && insn == BIN(const_cached_val_ld)) {
	assert(insn == BIN(const_cached_val_ld));
	fprintf(f, "  if (mjit_const_cached_val_ld(cfp, %llu, %llu, 0x%"PRIxVALUE ", %s, %ld",
		(unsigned long long) local_ic.ic_serial,
		(unsigned long long) local_ic.ic_cref, local_ic.ic_value.value,
		get_op_str(buf, code[pos + 1], tcp), code[pos + 1]);
	fprintf(f, ")) {\n  %s", generate_set_pc(TRUE, buf, &code[pos]));
	fprintf(f, "    %sgoto stop_spec;\n  }\n", set_failed_insn_str(buf, pos));
    } else {
	if (tcp->use_temp_vars_p && features.call_p)
	    /* Generate copying temps to the stack.  */
	    generate_param_setup(f, code, pos, features.recv_p);
	if (features.jmp_p && (features.bcmp_p || features.speculative_p))
	    fprintf(f, "  flag = ");
	else
	    fprintf(f, (features.jmp_p || features.op_end_p
			|| features.speculative_p
			? "  if (" : "  "));
	fprintf(f, "%s_f(", iname);
	if (features.th_p)
	    fprintf(f, "th, ");
	fprintf(f, "cfp");
	if (features.call_p)
	    fprintf(f, ", &calling");
	for (i = (features.jmp_p ? 2 : 1) + (features.skip_first_p ? 1 : 0); i < len; i++) {
	    op = code[pos + i];
	    if (types[i - 1] != TS_VARIABLE)
		fprintf(f, ", ");
	    switch (types[i - 1]) {
	    case TS_OFFSET:		/* LONG */
		fprintf(f, "%"PRIdVALUE, (VALUE)(pos + len + op));
		break;
	    case TS_NUM:		/* ULONG */
	    case TS_ID:
		fprintf(f, "%"PRIuVALUE, op);
		break;
	    case TS_LINDEX:
		fprintf(f, "%s", get_op_str(buf, op, tcp));
		break;
	    case TS_RINDEX:
		fprintf(f, "%s, (lindex_t) %"PRIdVALUE,
			get_op_str(buf, op, tcp), op);
		break;
	    case TS_SINDEX:
		fprintf(f, "(lindex_t) %"PRIdVALUE, op);
		break;
	    case TS_IC:
		fprintf(f, "(void *) 0x%"PRIxVALUE, op);
		break;
	    case TS_CDHASH:
	    case TS_VALUE:
	    case TS_ISEQ:
	    case TS_CALLINFO:
	    case TS_CALLCACHE:
	    case TS_CALLDATA:
	    case TS_GENTRY:
		fprintf(f, "(void *) 0x%"PRIxVALUE, op);
		break;
	    case TS_VARIABLE:
		break;
	    case TS_INSN:
		/* An insn operand should be never processed.  */
	    default:
		fprintf(stderr, "Unknown %d operand %c of %s\n", i, types[i - 1], iname);
		break;
	    }
	}
	if (features.op_end_p)
	    fprintf(f, ")) {\n    %sgoto stop_spec;\n  }\n", set_failed_insn_str(buf, pos));
	else if (features.speculative_p) {
	    if (features.jmp_p)
		fprintf(f, ", &val);\n  if (val == RUBY_Qundef) {\n");
	    else
		fprintf(f, ")) {\n");
	    fprintf(f, "  %s", generate_set_pc(TRUE, buf, &code[pos]));
	    fprintf(f, "    %sgoto stop_spec;\n  }\n", set_failed_insn_str(buf, pos));
	    if (features.jmp_p) {
		unsigned long dest = pos + len + code[pos + 2];
		fprintf(f, "  if (flag) goto l%ld;\n", dest);
	    }
	} else if (! features.jmp_p)
	    fprintf(f, ");\n");
	else if (! features.bcmp_p)
	    fprintf(f, "))\n    goto l%ld;\n", pos + len + code[pos + 1]);
	else {
            unsigned long dest = pos + len + code[pos + 2];
	    
	    fprintf(f, ", &val);\n  if (val == RUBY_Qundef) {\n");
	    fprintf(f, "    if (flag)%s", generate_set_pc(TRUE, buf, &code[dest]));
	    fprintf(f, "    %sgoto stop_spec;\n  }\n", set_failed_insn_str(buf, pos));
	    fprintf(f, "  if (flag) goto l%lu;\n", dest);
	}
	if (features.call_p) {
	    ptrdiff_t call_start = code[pos + 2];
	    
	    if (!tcp->safe_p && vm_call_iseq_setup_normal_p(local_cc.call) && insn_mutation_num == 0) {
		const rb_iseq_t *callee_iseq = rb_iseq_check(local_cc.me->def->body.iseq.iseqptr);

		fprintf(f, "  if (((CALL_CACHE) 0x%"PRIxVALUE ")->call != 0x%"PRIxVALUE ")",
			(VALUE) cc, (VALUE) local_cc.call);
		fprintf(f, " {\n  %s", generate_set_pc(TRUE, buf, &code[pos]));
		fprintf(f, "    %sgoto stop_spec;\n  }\n", set_failed_insn_str(buf, pos));
		fprintf(f, "  if (mjit_call_iseq_normal(th, cfp, &calling, (void *) 0x%"PRIxVALUE ", %d, %d, %s)) {\n",
			code[pos + 1], callee_iseq->body->param.size, callee_iseq->body->local_table_size,
			get_op_str(buf, call_start, tcp));
		fprintf(f, "    %sgoto stop_spec;\n  }\n", set_failed_insn_str(buf, pos));
	    } else {
		fprintf(f, "  if (mjit_call_method(th, cfp, &calling, (void *) 0x%"PRIxVALUE ", %s)) {\n",
			code[pos + 1], get_op_str(buf, call_start, tcp));
		fprintf(f, "    goto cancel;\n  }\n");
	    }
        }
    }
    return len;
}

/* Translate iseqs of batch B into C code and output it to the
   corresponding file.  Add include directives with INCLUDE_FNAME
   unless it is NULL.  Return 0 for a success.  Otherwise return IO
   error code.  */
static int
translate_batch_iseqs(struct rb_mjit_batch *b, const char *include_fname) {
    struct rb_mjit_batch_iseq *bi;
    int fd, err, ep_neq_bp_p;
    FILE *f = fopen(b->cfname, "w");
    char mjit_fname_holder[MAX_MJIT_FNAME_LEN];

    if (f == NULL)
	return errno;
    if (include_fname != NULL) {
	const char *s;

	fprintf(f, "#include \"");
	for (s = pch_fname; strcmp(s, ".gch") != 0; s++)
	    fprintf(f, "%c", *s);
	fprintf(f, "\"\n");
    }
#if MJIT_INSN_STATISTICS
    fprintf(f, "extern unsigned long jit_insns_num;\n");
#endif
    setup_global_spec_state(&b->spec_state);
    ep_neq_bp_p = FALSE;
    for (bi = b->first; bi != NULL; bi = bi->next)
	if (bi->ep_neq_bp_p) {
	    ep_neq_bp_p = TRUE;
	    break;
	}
    fprintf(f, "static const char mjit_profile_p = %d;\n", mjit_opts.profile);
    fprintf(f, "static const char mjit_trace_p = %d;\n", b->spec_state.trace_p);
    fprintf(f, "static const char mjit_bop_redefined_p = %d;\n", b->spec_state.bop_redefined_p);
    fprintf(f, "static const char mjit_ep_neq_bp_p = %d;\n", ep_neq_bp_p);
    for (bi = b->first; bi != NULL; bi = bi->next) {
	struct rb_iseq_constant_body *body;
	size_t i, size = bi->iseq_size;
	struct translation_control tc;
	
	if (bi->iseq == NULL)
	    continue;
	body = bi->iseq->body;
	tc.safe_p = bi->jit_mutations_num >= mjit_opts.max_mutations;
	tc.use_temp_vars_p = bi->use_temp_vars_p;
	/* If the current iseq contains a block, we should not use C
	   vars for local Ruby vars because a binding can be created
	   in a block and used inside for access to a variable of the
	   current iseq.  The current frame local vars will be saved
	   as bp and ep equality is changed into their inequality
	   after the binding call.  */
	tc.use_local_vars_p = ! body->parent_iseq_p && ! ep_neq_bp_p && tc.use_temp_vars_p;
	debug(3, "translating %d(0x%lx)", bi->num, (long unsigned) bi->iseq);
	fprintf(f, "VALUE %s(rb_thread_t *th, rb_control_frame_t *cfp) {\n",
		get_batch_iseq_fname(bi, mjit_fname_holder));
	fprintf(f, "  struct rb_calling_info calling;\n  VALUE val; int flag;\n");
	fprintf(f, "  static const char mutation_num = %d;\n", bi->jit_mutations_num);
	fprintf(f, "  size_t failed_insn_pc;\n");
	fprintf(f, "  VALUE self = cfp->self;\n");
	if (tc.use_local_vars_p)
	    for (i = 0; i < body->local_table_size; i++)
		fprintf(f, "  VALUE v%lu;\n", i);
	if (tc.use_temp_vars_p)
	    for (i = 0; i <= body->temp_vars_num; i++)
		fprintf(f, "  VALUE t%lu;\n", i);
	if (! ep_neq_bp_p) {
	    fprintf(f, "  if (cfp->bp != cfp->ep) {\n");
	    fprintf(f, "    mjit_ep_eq_bp_fail(cfp->iseq); return RUBY_Qundef;\n  }\n");
	}
	if (bi->ivar_spec != 0) {
	    fprintf(f, "  if (mjit_check_self_p(self, %llu, %lu)) {\n",
		    (unsigned long long) bi->ivar_serial, bi->ivar_spec);
	    fprintf(f, "    mjit_ivar_spec_fail(cfp->iseq); return RUBY_Qundef;\n  }\n");
	}
	fprintf(f, "  set_default_sp_0(cfp, cfp->bp, %u);\n",
		body->temp_vars_num);
	if (tc.use_local_vars_p) {
	    for (i = 0; i < body->local_table_size; i++)
		fprintf(f, "  v%ld = *get_loc_addr(cfp, %ld);\n", i, i + VM_ENV_DATA_SIZE);
	}
	if (body->param.flags.has_opt) {
	  int n;

	  fprintf(f, "  switch (cfp->pc - cfp->iseq->body->iseq_encoded) {\n");
	  for (n = 1; n <= body->param.opt_num; n++) {
	    fprintf(f, "  case %d: goto l%d;\n", (int) body->param.opt_table[n],
		    (int) body->param.opt_table[n]);
	  }
	  fprintf(f, "  }\n");
	}
	for (i = 0; i < size;)
	    i += translate_iseq_insn(f, i, bi, &tc);
	fprintf(f, "stop_spec:\n");
	fprintf(f, "  mjit_store_failed_spec_insn(cfp->iseq, failed_insn_pc, mutation_num);\n");
	fprintf(f, "  mjit_change_iseq(cfp->iseq);\n");
	fprintf(f, "cancel:\n");
	if (tc.use_local_vars_p) {
	    for (i = 0; i < body->local_table_size; i++)
		fprintf(f, "  *get_loc_addr(cfp, %ld) = v%ld;\n", i + VM_ENV_DATA_SIZE, (long) i);
	}
	if (tc.use_temp_vars_p) {
	    for (i = 0; i <= body->temp_vars_num; i++)
		fprintf(f, "  *get_temp_addr_safe(cfp, %ld) = t%ld;\n", -1 - (long) i, (long) i);
	}
	fprintf(f, "  return RUBY_Qundef;\n}\n");
    }
    fd = fileno(f);
    fsync(fd);
    err = ferror(f);
    fclose(f);
    return err;
}

/* Start an OS process of executable PATH with arguments ARGV.  Return
   PID of the process.  */
static pid_t
start_process(const char *path, char *const argv[]) {
  pid_t pid;

  if (mjit_opts.verbose) {
      int i;
      const char *arg;

      fprintf(stderr, "++Starting process: %s", path);
      for (i = 0; (arg = argv[i]) != NULL; i++)
	  fprintf(stderr, " %s", arg);
      fprintf(stderr, ": time - %.3f ms\n", relative_ms_time());
  }
  if ((pid = fork()) == 0) {
      if (mjit_opts.verbose) {
	  /* CC can be started in a thread using a file which has been
	     already removed while MJIT is finishing.  Discard the
	     messages about missing files.  */
	  FILE *f = fopen("/dev/null", "w");

	  dup2(fileno(f), STDERR_FILENO);
      }
      pid = execvp(path, argv); /* Pid will be negative on an error */
      debug(1, "Error in execvp: %s", path);
  }
  return pid;
}

/* Status of the the precompiled header creation.  The status is
   shared by the workers and the pch thread.  */
static enum {PCH_NOT_READY, PCH_FAILED,  PCH_SUCCESS} pch_status;

/* The function producing the pre-compiled header.  It is executed in
   a separate thread started by pthread_create. */
static void *
make_pch(void *arg) {
    int stat, exit_code, ok_p;
    pid_t pid;
    static const char *input[] = {NULL, NULL};
    static const char *output[] = {"-o",  NULL, NULL};
    char **args;

    if (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) != 0) {
	fprintf(stderr, "+++Cannot enable cancelation in pch-thread: time - %.3f ms\n",
		relative_ms_time());
    }
    verbose("Creating precompiled header");
    input[0] = header_fname;
    output[1] = pch_fname;
    if (mjit_opts.llvm)
	args = form_args(4, (mjit_opts.debug ? LLVM_COMMON_ARGS_DEBUG : LLVM_COMMON_ARGS),
			 LLVM_EMIT_PCH_ARGS, input, output);
    else
	args = form_args(4, (mjit_opts.debug ? GCC_COMMON_ARGS_DEBUG : GCC_COMMON_ARGS),
			 GCC_EMIT_PCH_ARGS, input, output);
    if (args == NULL)
	pid = -1;
    else
	pid = start_process(cc_path, args);
    ok_p = pid > 0;
    if (ok_p) {
	for (;;) {
	    waitpid(pid, &stat, 0);
	    if (WIFEXITED(stat)) {
		exit_code = WEXITSTATUS(stat);
		break;
	    } else if (WIFSIGNALED(stat)) {
		exit_code = -1;
		break;
	    }
	}
	ok_p = exit_code == 0;
    }
    free(args);
    CRITICAL_SECTION_START("in make_pch");
    if (ok_p) {
	verbose("Precompiled header was succesfully created");
	pch_status = PCH_SUCCESS;
    } else {
	if (mjit_opts.warnings || mjit_opts.verbose)
	    fprintf(stderr, "MJIT warning: making precompiled header failed\n");
	pch_status = PCH_FAILED;
    }
    debug(3, "Sending a wakeup signal to workers in make_pch");
    if (pthread_cond_broadcast(&mjit_pch_wakeup) != 0) {
	fprintf(stderr, "++Cannot send client wakeup signal in make_pch: time - %.3f ms\n",
		relative_ms_time());
    }
    CRITICAL_SECTION_FINISH("in make_pch");
    return NULL;
}

/* This function is executed in a worker thread.  The function creates
   a C file for iseqs in the batch B and starts a C compiler to
   generate an object file of the C file.  Return TRUE in a success
   case.  */
static int
start_batch(struct rb_mjit_batch *b) {
    pid_t pid;
    static const char *input[] = {NULL, NULL};
    static const char *output[] = {"-o",  NULL, NULL};
    char **args;

    verbose("Starting batch %d execution", b->num);
    if ((b->cfname = get_uniq_fname(b->num, "_mjit", ".c")) == NULL) {
	b->status = BATCH_FAILED;
	return FALSE;
    }
    if ((b->ofname = get_uniq_fname(b->num, "_mjit", ".so")) == NULL) {
	b->status = BATCH_FAILED;
	free(b->cfname); b->cfname = NULL;
	return FALSE;
    }
    if (mjit_opts.debug)
	b->time_start = real_ms_time();
    if (translate_batch_iseqs(b, mjit_opts.llvm ? NULL : header_fname)) {
	pid = -1;
    } else {
	input[0] = b->cfname;
	output[1] = b->ofname;
	if (mjit_opts.llvm) {
	    LLVM_USE_PCH_ARGS[1] = pch_fname;
	    args = form_args(4, (mjit_opts.debug ? LLVM_COMMON_ARGS_DEBUG : LLVM_COMMON_ARGS),
			     LLVM_USE_PCH_ARGS, input, output);
	} else {
	    args = form_args(4, (mjit_opts.debug ? GCC_COMMON_ARGS_DEBUG : GCC_COMMON_ARGS),
			     GCC_USE_PCH_ARGS, input, output);
	}
	if (args == NULL)
	    pid = -1;
	else {
	    pid = start_process(cc_path, args);
	    free(args);
	}
    }
    if (pid < 0) {
        debug(1, "Failed starting batch %d execution", b->num);
	b->status = BATCH_FAILED;
	if (! mjit_opts.save_temps) {
	    remove(b->cfname);
	    free(b->cfname); b->cfname = NULL;
	    remove(b->ofname);
	    free(b->ofname); b->ofname = NULL;
	}
	return FALSE;
    } else {
	debug(2, "Success in starting batch %d execution", b->num);
	b->pid = pid;
	return TRUE;
    }
}

/* The function should be called after successul creation of the
   object file for iseqs of batch B.  The function loads the object
   file.  */
static void
load_batch(struct rb_mjit_batch *b) {
    struct rb_mjit_batch_iseq *bi;
    void *addr;
    char mjit_fname_holder[MAX_MJIT_FNAME_LEN];
    const char *fname, *err_name;

    assert(b->status == BATCH_SUCCESS);
    b->handle = dlopen(b->ofname, RTLD_NOW | RTLD_GLOBAL);
    if (! mjit_opts.save_temps) {
	remove(b->ofname);
	free(b->ofname); b->ofname = NULL;
    }
    CRITICAL_SECTION_START("in load_batch to load the batch");
    b->status = b->handle == NULL ? BATCH_FAILED : BATCH_LOADED;
    CRITICAL_SECTION_FINISH("in load_batch to load the batch");
    if (b->handle != NULL)
	verbose("Success in loading code of batch %d",	b->num);
    else if (mjit_opts.warnings || mjit_opts.verbose)
	fprintf(stderr, "MJIT warning: failure in loading code of batch %d(%s)\n", b->num, dlerror());
    for (bi = b->first; bi != NULL; bi = bi->next) {
	addr = (void *) NOT_ADDED_JIT_ISEQ_FUN;
	if (b->status == BATCH_LOADED) {
	    fname = get_batch_iseq_fname(bi, mjit_fname_holder);
	    addr = dlsym(b->handle, fname);
	    if ((err_name = dlerror ()) != NULL) {
		debug(0, "Failure (%s) in setting address of iseq %d(%s)", err_name, bi->num, fname);
		addr = (void *) NOT_ADDED_JIT_ISEQ_FUN;
	    } else {
		debug(2, "Success in setting address of iseq %d(%s)(%s) 0x%"PRIxVALUE,
		      bi->num, fname, bi->label, addr);
	    }
	}
	/* TODO: Do we need a critical section here.  */
	CRITICAL_SECTION_START("in load_batch to setup MJIT code");
	if (bi->iseq != NULL) {
	    bi->iseq->body->jit_code = addr;
	}
	CRITICAL_SECTION_FINISH("in load_batch to setup MJIT code");
    }
}

/* Maximum number of worker threads.  As worker can process only one
   batch at a time, the number also represents the maximal number of C
   compiler processes started by MJIT and running at any given
   time.  */
#define MAX_WORKERS_NUM 100
/* The default number of worker threads.  */
#define DEFAULT_WORKERS_NUM 1

/* Set to TRUE to finish workers.  */
static int finish_workers_p;
/* A number of teh finished workers so far.  */
static int finished_workers;

/* The function implementing a worker. It is executed in a separate
   thread started by pthread_create. */
static void *
worker(void *arg) {
    if (pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) != 0) {
	fprintf(stderr, "+++Cannot enable cancelation in worker: time - %.3f ms\n",
		relative_ms_time());
    }
    CRITICAL_SECTION_START("in worker to wakeup from pch");
    while (pch_status == PCH_NOT_READY) {
	debug(3, "Waiting wakeup from make_pch");
	pthread_cond_wait(&mjit_pch_wakeup, &mjit_engine_mutex);
    }
    CRITICAL_SECTION_FINISH("in worker to wakeup from pch");
    if (pch_status == PCH_FAILED) {
	mjit_init_p = FALSE;
	CRITICAL_SECTION_START("in worker to update finished_workers");
	finished_workers++;
	CRITICAL_SECTION_FINISH("in worker to update finished_workers");
	debug(3, "Sending wakeup signal to client in a mjit-worker");
	if (pthread_cond_signal(&mjit_client_wakeup) != 0) {
	    fprintf(stderr, "+++Cannot send wakeup signal to client in mjit-worker: time - %.3f ms\n",
		    relative_ms_time());
	}
	return NULL;
    }
    CRITICAL_SECTION_START("in worker to start the batch");
    for (;;) {
	int stat, exit_code;
	struct rb_mjit_batch *b;

	if (finish_workers_p) {
	    finished_workers++;
	    break;
	}
	b = get_from_list(&batch_queue);
	if (b != NULL)
	  b->status = BATCH_IN_EXECUTION;
	CRITICAL_SECTION_FINISH("in worker to start the batch");
	if (b != NULL) {
	    start_batch(b);
	    waitpid(b->pid, &stat, 0);
	    exit_code = -1;
	    if (WIFEXITED(stat)) {
		exit_code = WEXITSTATUS(stat);
	    }
	    CRITICAL_SECTION_START("in worker to setup status");
	    b->status = exit_code != 0 ? BATCH_FAILED : BATCH_SUCCESS;
	    if (exit_code == 0)
		verbose("Success in compilation of batch %d");
	    else if (mjit_opts.warnings || mjit_opts.verbose)
		fprintf(stderr, "MJIT warning: failure in compilation of batch %d\n", b->num);
	    add_to_list(b, &done_batches);
	    CRITICAL_SECTION_FINISH("in worker to setup status");
	    if (! mjit_opts.save_temps) {
		remove(b->cfname);
		free(b->cfname); b->cfname = NULL;
	    }
	    if (exit_code == 0) {
		struct global_spec_state curr_state;
		int recompile_p;
		
		CRITICAL_SECTION_START("in worker to check global speculation status");
		setup_global_spec_state(&curr_state);
		recompile_p = ! valid_global_spec_state_p(&b->spec_state, &curr_state);
		if (recompile_p) {
		    remove_from_list(b, &done_batches);
		    add_to_list(b, &batch_queue);
		    b->status = BATCH_IN_QUEUE;
		    verbose("Global speculation changed -- put batch %d back into the queue", b->num);
		}
		CRITICAL_SECTION_FINISH("in worker to check global speculation status");
		if (! recompile_p) {
		    debug(2, "Start loading batch %d", b->num);
		    load_batch(b);
		}
	    }
	    debug(3, "Sending wakeup signal to client in a mjit-worker");
	    if (pthread_cond_signal(&mjit_client_wakeup) != 0) {
		fprintf(stderr, "+++Cannot send wakeup signal to client in mjit-worker: time - %.3f ms\n",
			relative_ms_time());
	    }
	} else {
	    debug(3, "Waiting wakeup from client");
	    CRITICAL_SECTION_START("in worker for a worker wakeup");
	    while (batch_queue.head == NULL && ! finish_workers_p)
		pthread_cond_wait(&mjit_worker_wakeup, &mjit_engine_mutex);
	    debug(3, "Getting wakeup from client");
	}
    }
    CRITICAL_SECTION_FINISH("in worker to finish");
    debug(1, "Finishing worker");
    return NULL;
}

/*-------All the following code is executed in the client thread only-----------*/
/* PID of thread creating the pre-compiled header.  */
static pthread_t pch_pid;
/* The current number of worker threads. */
static int workers_num;
/* Only first workers_num elements are defined.  The element contains
   PID of worker thread with its number equal to the index.  */
static pthread_t worker_pids[MAX_WORKERS_NUM];

/* Singly linked list of allocated but marked free batch structures.  */
static struct rb_mjit_batch *free_batch_list;
/* Singly linked list of allocated but marked free batch iseq structures.  */
static struct rb_mjit_batch_iseq *free_batch_iseq_list;
/* The number of so far processed ISEQs.  */
static int curr_mjit_iseq_num;
/* The batch currently being formed.  */
static struct rb_mjit_batch *curr_batch;
/* The number of so far created batches.  */
static int curr_batch_num;

/* Initialize code for work with workers.  */
static void
init_workers(void) {
    workers_num = 0;
    free_batch_list = NULL;
    free_batch_iseq_list = NULL;
    curr_mjit_iseq_num = 0;
    done_batches.head = done_batches.tail = NULL;
    batch_queue.head = batch_queue.tail = NULL;
    curr_batch = NULL;
    curr_batch_num = 0;
    pch_status = PCH_NOT_READY;
}

/* Update initial values of BI ivar_spec, ivar_serial, and
   use_temp_vars_p.  */
static void
update_batch_iseq_info_from_insns(struct rb_mjit_batch_iseq *bi) {
    rb_iseq_t *iseq = bi->iseq;
    struct rb_iseq_constant_body *body = iseq->body;
    size_t pos, ic_disp, size = bi->iseq_size;
    rb_serial_t ivar_serial;
    int ivar_spec_update_p;
    size_t ivar_access_num, index, max_ivar_spec_index;
    const VALUE *code = iseq->body->iseq_encoded;
    VALUE insn;
    IC ic;
    
    ivar_spec_update_p = TRUE;
    ivar_serial = 0;
    ivar_access_num = 0;
    max_ivar_spec_index = 0;
    for (pos = 0; pos < size;) {
	insn = code[pos];
#if OPT_DIRECT_THREADED_CODE || OPT_CALL_THREADED_CODE
	insn = rb_vm_insn_addr2insn((void *) insn);
#endif
	ic_disp = 2;
	switch (insn) {
	case BIN(var2var):
	case BIN(concat_strings):
	case BIN(to_regexp):
	case BIN(make_array):
	case BIN(make_hash):
	case BIN(new_array_min):
	case BIN(new_array_max):
	case BIN(spread_array):
	case BIN(define_class):
	    bi->use_temp_vars_p = FALSE;
	    break;
	case BIN(ivar2var):
	    ic_disp = 3;
	case BIN(val2ivar):
	case BIN(var2ivar):
	    ic = (IC) code[pos + ic_disp];
	    if (ivar_access_num == 0)
		ivar_serial = ic->ic_serial;
	    else if (ivar_serial != ic->ic_serial)
		ivar_spec_update_p = FALSE;
	    index = ic->ic_value.index;
	    if (ivar_access_num == 0 || max_ivar_spec_index < index)
		max_ivar_spec_index = index;
	    ivar_access_num++;
	    break;
	}
	pos += insn_len(insn);
    }
    if (ivar_spec_update_p && ivar_access_num > 2 && body->in_type_object_p) {
	/* We have enough ivar accesses to make whole function
	   speculation about them.  */
	bi->ivar_spec = (max_ivar_spec_index >= ROBJECT_EMBED_LEN_MAX
			 ? max_ivar_spec_index : (size_t) -1);
	bi->ivar_serial = ivar_serial;
    }
    if (body->except_p)
	bi->use_temp_vars_p = FALSE;
}

/* Return a free batch iseq.  It can allocate a new structure if there
   are no free batch iseqs.  */
static struct rb_mjit_batch_iseq *
create_batch_iseq(rb_iseq_t *iseq) {
    int i;
    struct rb_mjit_batch_iseq *bi;

    if (free_batch_iseq_list == NULL) {
	bi = xmalloc(sizeof(struct rb_mjit_batch_iseq));
	if (bi == NULL)
	    return NULL;
    } else {
	bi = free_batch_iseq_list;
	free_batch_iseq_list = free_batch_iseq_list->next;
    }
    bi->num = curr_mjit_iseq_num++;
    bi->iseq = iseq;
    iseq->body->batch_iseq = bi;
    bi->batch = NULL;
    bi->iseq_size = iseq->body->iseq_size;
    bi->label = NULL;
    if (mjit_opts.debug || mjit_opts.profile) {
	bi->label = get_string(RSTRING_PTR(iseq->body->location.label));
	bi->overall_calls = bi->jit_calls = bi->failed_jit_calls = 0;
    }
    bi->ep_neq_bp_p = FALSE;
    bi->ivar_spec = 0;
    bi->ivar_serial = 0;
    bi->use_temp_vars_p = TRUE;
    bi->jit_mutations_num = 0;
    bi->mutation_insns = xmalloc(sizeof (struct mjit_mutation_insns) * (mjit_opts.max_mutations + 1));
    for (i = 0; i <= mjit_opts.max_mutations; i++)
	bi->mutation_insns[i].insn = BIN(nop);
    update_batch_iseq_info_from_insns(bi);
    return bi;
}

/* Return a free batch iseq.  */
static struct rb_mjit_batch *
create_batch(void) {
    struct rb_mjit_batch *b;

    if (free_batch_list == NULL) {
	b = xmalloc(sizeof(struct rb_mjit_batch));
	if (b == NULL)
	    return NULL;
    } else {
	b = free_batch_list;
	free_batch_list = free_batch_list->next;
    }
    b->status = BATCH_NOT_FORMED;
    b->num = curr_batch_num++;
    b->active_iseqs_num = 0;
    b->iseqs_size = 0;
    b->cfname = b->ofname = NULL;
    b->next = NULL;
    b->first = b->last = NULL;
    init_global_spec_state(&b->spec_state);
    return b;
}

/* Mark batch iseq BI as free.  */
static void
free_batch_iseq(struct rb_mjit_batch_iseq *bi) {
    if (bi->label != NULL) {
	free(bi->label);
	bi->label = NULL;
    }
    bi->next = free_batch_iseq_list;
    free_batch_iseq_list = bi;
}

/* Mark all batch iseqs from LIST as free.  */
static void
free_batch_iseqs(struct rb_mjit_batch_iseq *list) {
    struct rb_mjit_batch_iseq *bi, *bi_next;

    for (bi = list; bi != NULL; bi = bi_next) {
	bi_next = bi->next;
	free_batch_iseq(bi);
    }
}

/* Mark the batch B as free.  The function markes the batch iseqs as
   free first.  */
static void
free_batch(struct rb_mjit_batch *b) {
    if (b->status == BATCH_SUCCESS && ! mjit_opts.save_temps) {
	remove(b->ofname);
    }
    if (b->ofname != NULL) {
	free(b->ofname);
	b->ofname = NULL;
    }
    free_batch_iseqs(b->first);
    b->next = free_batch_list;
    free_batch_list = b;
}

/* Mark the batch from LIST as free.  */
static void
free_batches(struct rb_mjit_batch *list) {
  struct rb_mjit_batch *b, *next;

  for (b = list; b != NULL; b = next) {
      next = b->next;
      free_batch(b);
  }
}

/* Free memory for all marked free batches and batch iseqs.  */
static void
finish_batches(void) {
  struct rb_mjit_batch *b, *next;
  struct rb_mjit_batch_iseq *bi, *bi_next;

  for (b = free_batch_list; b != NULL; b = next) {
      next = b->next;
      free(b);
  }
  for (bi = free_batch_iseq_list; bi != NULL; bi = bi_next) {
      bi_next = bi->next;
      free(bi->mutation_insns);
      free(bi);
  }
  free_batch_list = NULL;
  free_batch_iseq_list = NULL;
}

/* Free memory allocated for all batches and batch iseqs.  */
static void
finish_workers(void){
    free_batches(done_batches.head);
    free_batches(batch_queue.head);
    if (curr_batch != NULL)
	free_batch(curr_batch);
    finish_batches();
}

/* Add the batch iseq BI to the batch B.  */
static void
add_iseq_to_batch(struct rb_mjit_batch *b, struct rb_mjit_batch_iseq *bi) {
    bi->next = b->first;
    bi->prev = NULL;
    bi->batch = b;
    if (b->first != NULL)
	b->first->prev = bi;
    else
	b->last = bi;
    b->first = bi;
    b->iseqs_size += bi->iseq_size;
    b->active_iseqs_num++;
    debug(1, "iseq %d is added to batch %d (size %lu)",
	  bi->num, b->num, (long unsigned) b->iseqs_size);
}
/* Remove the batch iseq BI to the batch B.  The batch iseq should
   belong to another batch before the call.  */
static void
remove_iseq_from_batch(struct rb_mjit_batch *b, struct rb_mjit_batch_iseq *bi) {
    struct rb_mjit_batch_iseq *prev, *next;

    prev = bi->prev;
    next = bi->next;
    if (prev != NULL)
	prev->next = next;
    else
	bi->batch->first = next;
    if (next != NULL)
	next->prev = prev;
    else
	bi->batch->last = prev;
    bi->batch->iseqs_size -= bi->iseq_size;
    bi->batch->active_iseqs_num--;
    debug(2, "iseq %d is removed from batch %d (size = %lu)",
	  bi->num, bi->batch->num,
	  (long unsigned) bi->batch->iseqs_size);
}

/* Move the batch iseq BI to the batch B.  The batch iseq should
   belong to another batch before the call.  */
static void
move_iseq_to_batch(struct rb_mjit_batch *b, struct rb_mjit_batch_iseq *bi) {
    remove_iseq_from_batch(b, bi);
    add_iseq_to_batch(b, bi);
}

/* Add the current batch to the queue.  */
static void
finish_forming_curr_batch(void) {
    if (curr_batch == NULL)
	return;
    add_to_list(curr_batch, &batch_queue);
    curr_batch->status = BATCH_IN_QUEUE;
    debug(2, "Finish forming batch %d (size = %lu)",
	  curr_batch->num, (long unsigned) curr_batch->iseqs_size);
    curr_batch = NULL;
}

/* If it is true any batch will contain at most one batch iseq.  */
static const int quick_response_p = TRUE;

/* When the following variable is FALSE, the batch becomes formed and
   added to the queue if overall size of its iseqs becomes bigger the
   macro value.  */
#define MAX_BATCH_ISEQ_SIZE 1000

RUBY_SYMBOL_EXPORT_BEGIN
/* Add ISEQ to be JITed in parallel with the current thread.  Add it
   to the current batch.  Add the current batch to the queue if
   QUICK_RESPONSE_P or the current batch becomes big.  */
void
mjit_add_iseq_to_process(rb_iseq_t *iseq) {
    struct rb_mjit_batch_iseq *bi;

    if (!mjit_init_p)
	return;
    verbose("Adding iseq");
    if (curr_batch == NULL && (curr_batch = create_batch()) == NULL)
	return;
    if ((bi = iseq->body->batch_iseq) == NULL
	&& (bi = create_batch_iseq(iseq)) == NULL)
	return;
    add_iseq_to_batch(curr_batch, bi);
    if (quick_response_p || curr_batch->iseqs_size > MAX_BATCH_ISEQ_SIZE) {
	CRITICAL_SECTION_START("in add_iseq_to_process");
	finish_forming_curr_batch();
	debug(3, "Sending wakeup signal to workers in mjit_add_iseq_to_process");
	if (pthread_cond_broadcast(&mjit_worker_wakeup) != 0) {
	    fprintf(stderr, "Cannot send wakeup signal to workers in add_iseq_to_process: time - %.3f ms\n",
		    relative_ms_time());
	}
	CRITICAL_SECTION_FINISH("in add_iseq_to_process");
    }
}

/* Increase ISEQ process priority.  It means moving the iseq to a
   batch at the head of the queue.  As result the iseq JIT code will
   be produced faster.  The iseq should be already in a batch.  */
void
mjit_increase_iseq_priority(const rb_iseq_t *iseq) {
    int move_p = FALSE;
    struct rb_mjit_batch *b;
    struct rb_mjit_batch_iseq *last, *bi = iseq->body->batch_iseq;

    if (!mjit_init_p)
	return;
    assert(bi != NULL && bi->batch != NULL);
    b = bi->batch;
    verbose("Increasing priority for iseq %d", bi->num);
    CRITICAL_SECTION_START("in mjit_increase_iseq_priority");
    if (b == curr_batch) {
	if (batch_queue.head != NULL)
	    move_p = TRUE;
	else
	    finish_forming_curr_batch();
    } else if (b->status == BATCH_IN_QUEUE && batch_queue.head != b) {
	assert(batch_queue.head != NULL);
	move_p = TRUE;
    }
    if (move_p) {
	/* Move to the head batch: */
	last = batch_queue.head->last;
	move_iseq_to_batch(batch_queue.head, bi);
	if (last != NULL)
	    move_iseq_to_batch(b, last);
	if (b->first == NULL) {
	    remove_from_list(b, &batch_queue);
	    free_batch(b);
	}
    }
    debug(3, "Sending wakeup signal to workers in mjit_increase_iseq_priority");
    if (pthread_cond_broadcast(&mjit_worker_wakeup) != 0) {
	fprintf(stderr, "Cannot send wakeup signal to workers in mjit_increase_iseq_priority: time - %.3f ms\n",
		relative_ms_time());
    }
    CRITICAL_SECTION_FINISH("in mjit_increase_iseq_priority");
}

/* Redo ISEQ.  It means canceling the current JIT code and adding ISEQ
   to the queue for processing again.  */
void
mjit_redo_iseq(rb_iseq_t *iseq) {
    struct rb_mjit_batch_iseq *bi = iseq->body->batch_iseq;
    struct rb_mjit_batch *b = bi->batch;

    assert(b->status == BATCH_LOADED
	   && b->handle != NULL && b->active_iseqs_num > 0);
    verbose("Iseq #%d (so far mutations=%d) in batch %d is canceled",
	    bi->num, bi->jit_mutations_num, b->num);
    remove_iseq_from_batch(b, bi);
    // iseq->body->batch_iseq = NULL;
    bi->jit_mutations_num++;
    if (b->active_iseqs_num <= 0) {
#if 0
	/* TODO: Implement unloading.  We need to check that we left
	   all generated JIT code (remember about cancellation during
	   recursive calls).  */
	dlclose(b->handle);
	b->handle = NULL;
#endif
	verbose("Code of batch %d is removed", b->num);
	assert(b->first == NULL);
	CRITICAL_SECTION_START("in removing a done batch");
	remove_from_list(b, &done_batches);
	CRITICAL_SECTION_FINISH("in removing a done batch");
	free_batch(b);
    }
    iseq->body->jit_code
	= (void *) (ptrdiff_t) (mjit_opts.aot
				? NOT_READY_AOT_ISEQ_FUN
				: NOT_READY_JIT_ISEQ_FUN);
    mjit_add_iseq_to_process(iseq);
}

/* Wait for finishing ISEQ generation.  To decrease the wait, increase
   ISEQ priority.  Return the code address, NULL in a failure
   case.  */
mjit_fun_t
mjit_get_iseq_fun(const rb_iseq_t *iseq) {
    enum batch_status status;
    struct rb_mjit_batch_iseq *bi = iseq->body->batch_iseq;
    struct rb_mjit_batch *b = bi->batch;

    if (!mjit_init_p)
	return NULL;
    mjit_increase_iseq_priority(iseq);
    bi = iseq->body->batch_iseq;
    b = bi->batch;
    CRITICAL_SECTION_START("in mjit_add_iseq_to_process for a client wakeup");
    while ((status = b->status) != BATCH_LOADED && status != BATCH_FAILED) {
	debug(3, "Waiting wakeup from a worker in mjit_add_iseq_to_process");
	pthread_cond_wait(&mjit_client_wakeup, &mjit_engine_mutex);
	debug(3, "Getting wakeup from a worker in mjit_add_iseq_to_process");
    }
    CRITICAL_SECTION_FINISH("in mjit_add_iseq_to_process for a client wakeup");
    if (b->status == BATCH_FAILED)
	return NULL;
    return bi->iseq->body->jit_code;
}

/* Called when an ISEQ insn with a relative PC caused MUTATION_NUM-th
   mutation.  We just collect this info in mutation_insns array. */
void
mjit_store_failed_spec_insn(rb_iseq_t *iseq, size_t pc, int mutation_num)  {
    struct rb_mjit_batch_iseq *bi = iseq->body->batch_iseq;
    VALUE insn;

    insn = iseq->body->iseq_encoded[pc];
#if OPT_DIRECT_THREADED_CODE || OPT_CALL_THREADED_CODE
    insn = rb_vm_insn_addr2insn((void *) insn);
#endif
    bi->mutation_insns[mutation_num].pc = pc;
    bi->mutation_insns[mutation_num].insn = insn;
}

/* It is called when our whole function speculation about ivar
   accesses during ISEQ execution failed.  */
void
mjit_ivar_spec_fail(rb_iseq_t *iseq) {
    if (iseq->body->jit_code >= (void *) LAST_JIT_ISEQ_FUN) {
	iseq->body->batch_iseq->ivar_spec = 0;
	mjit_redo_iseq(iseq);
    }
}

/* It is called when our speculation about ep and bp equality during
   ISEQ execution failed.  */
void
mjit_ep_eq_bp_fail(rb_iseq_t *iseq) {
    if (iseq->body->jit_code >= (void *) LAST_JIT_ISEQ_FUN) {
	iseq->body->batch_iseq->ep_neq_bp_p = TRUE;
	mjit_redo_iseq(iseq);
    }
}

RUBY_SYMBOL_EXPORT_END

/* Iseqs can be garbage collected.  This function should call when it
   happens.  It removes iseq from any batch.  */
void
mjit_free_iseq(const rb_iseq_t *iseq) {
    struct rb_mjit_batch_iseq *bi;

    if (!mjit_init_p || (bi = iseq->body->batch_iseq) == NULL)
	return;
    CRITICAL_SECTION_START("to clear iseq in mjit_free_iseq");
    bi->iseq = NULL;
    CRITICAL_SECTION_FINISH("to clear iseq in mjit_free_iseq");
    if (mjit_opts.debug || mjit_opts.profile) {
	bi->overall_calls = iseq->body->overall_calls;
	bi->jit_calls = iseq->body->jit_calls;
	bi->failed_jit_calls = iseq->body->failed_jit_calls;
    }
}

/* Mark all JIT code being executed for cancellation.  Redo JIT code
   with invalid speculation. It happens when a global speculation
   fails.  For example, a basic operation is redefined or tracing
   starts.  */
void
mjit_cancel_all(void)
{
    rb_iseq_t *iseq;
    rb_control_frame_t *fp;
    rb_vm_t *vm = GET_THREAD()->vm;
    rb_thread_t *th = 0;
    struct global_spec_state curr_state;
    struct rb_mjit_batch *b, *next;

    if (!mjit_init_p)
	return;
    list_for_each(&vm->living_threads, th, vmlt_node) {
	rb_control_frame_t *limit_cfp = (void *)(th->stack + th->stack_size);
	for(fp = th->cfp; fp < limit_cfp; fp = RUBY_VM_PREVIOUS_CONTROL_FRAME(fp))
	  if ((iseq = fp->iseq) != NULL && imemo_type((VALUE) iseq) == imemo_iseq
	      && iseq->body->jit_code >= (void *) LAST_JIT_ISEQ_FUN) {
	      fp->bp[0] |= VM_FRAME_FLAG_CANCEL;
	  }
    }
    CRITICAL_SECTION_START("mjit_cancel_all");
    verbose("Cancel all wrongly speculative JIT code");
    setup_global_spec_state(&curr_state);
    for (b = done_batches.head; b != NULL; b = next) {
	next = b->next;
	if (b->status == BATCH_LOADED
	    && ! valid_global_spec_state_p(&b->spec_state, &curr_state)) {
#if 0
	    verbose("Global speculation changed -- unload code of batch", b->num);
	    dlclose(b->handle);
	    b->handle = NULL;
#endif
	    verbose("Global speculation changed -- recompiling batch %d", b->num);
	    remove_from_list(b, &done_batches);
	    add_to_list(b, &batch_queue);
	    b->status = BATCH_IN_QUEUE;
	}
    }
    debug(3, "Sending wakeup signal to workers in mjit_cancel_all");
    if (pthread_cond_broadcast(&mjit_worker_wakeup) != 0) {
        fprintf(stderr, "Cannot send wakeup signal to workers in mjit_cancel_all: time - %.3f ms\n",
		relative_ms_time());
    }
    CRITICAL_SECTION_FINISH("mjit_cancel_all");
}

/* A name of the header file included in any C file generated by MJIT for iseqs.  */
#define RUBY_MJIT_HEADER_FNAME ("rb_mjit_min_header-" RUBY_VERSION ".h")
/* GCC and LLVM executable paths.  TODO: The paths should absolute
   ones to prevent changing C compiler for security reasons.  */
#define GCC_PATH "gcc"
#define LLVM_PATH "clang"

/* The default number of permitted ISEQ MJIT code mutations.  */
#define DEFAULT_MUTATIONS_NUM 2

/* This is called after each fork in the child in to switch off MJIT
   engine in the child as it does no inherit MJIT threads.  */
static void child_after_fork(void) {
  verbose("Switching off MJIT in a forked child");
  mjit_init_p = FALSE;
  /* TODO: Should we initiate MJIT in the forked Ruby.  */
}

/* Initialize MJIT.  Start a thread creating the precompiled
   header.  Create worker threads processing batches.  The function
   should be called first for using MJIT.  If everything is
   successfull, MJIT_INIT_P will be TRUE.  */
void
mjit_init(struct mjit_options *opts) {
    pthread_attr_t attr;
    int init_state;
    pthread_t pid;
    const char *path;
    FILE *f;

    mjit_opts = *opts;
    if (mjit_opts.threads <= 0)
	mjit_opts.threads = DEFAULT_WORKERS_NUM;
    if (mjit_opts.threads > MAX_WORKERS_NUM)
	mjit_opts.threads = MAX_WORKERS_NUM;
    if (mjit_opts.max_mutations <= 0)
	mjit_opts.max_mutations = DEFAULT_MUTATIONS_NUM;
    mjit_time_start = real_ms_time();
    client_pid = pthread_self();
    if (mjit_init_p)
	return;
    pthread_atfork(NULL, NULL, child_after_fork);
    debug(2, "Start initializing MJIT");
    finish_workers_p = FALSE;
    finished_workers = 0;
    header_fname = xmalloc(strlen(BUILD_DIR) + 2 + strlen(RUBY_MJIT_HEADER_FNAME));
    if (header_fname == NULL)
	return;
    strcpy(header_fname, BUILD_DIR);
    strcat(header_fname, "/");
    strcat(header_fname, RUBY_MJIT_HEADER_FNAME);
    if ((f = fopen(header_fname, "r")) == NULL) {
	free(header_fname);
	header_fname = xmalloc(strlen(DEST_INCDIR) + 2 + strlen(RUBY_MJIT_HEADER_FNAME));
	if (header_fname == NULL)
	    return;
	strcpy(header_fname, DEST_INCDIR);
	strcat(header_fname, "/");
	strcat(header_fname, RUBY_MJIT_HEADER_FNAME);
	if ((f = fopen(header_fname, "r")) == NULL) {
	    free(header_fname); header_fname = NULL;
	    return;
	}
    }
    fclose(f);
#ifdef __MACH__
    if (! mjit_opts.llvm) {
	if (mjit_opts.warnings || mjit_opts.verbose)
	    fprintf(stderr, "MJIT warning: we use only clang on Mac OS X\n");
	mjit_opts.llvm = 1;
    }
#endif
    path = mjit_opts.llvm ? LLVM_PATH : GCC_PATH;
    cc_path = xmalloc(strlen(path) + 1);
    if (cc_path == NULL) {
	free(header_fname); header_fname = NULL;
	return;
    }
    strcpy(cc_path, path);
    if ((pch_fname = get_uniq_fname(0, "_mjit_h", ".h.gch")) == NULL) {
	free(header_fname); header_fname = NULL;
	free(cc_path); cc_path = NULL;
	return;
    }
    init_workers();
    pthread_mutex_init(&mjit_engine_mutex, NULL);
    init_state = 0;
    if (pthread_cond_init(&mjit_pch_wakeup, NULL) == 0) {
      init_state = 1;
      if (pthread_cond_init(&mjit_client_wakeup, NULL) == 0) {
	init_state = 2;
	if (pthread_cond_init(&mjit_worker_wakeup, NULL) == 0) {
	    init_state = 3;
	    if (pthread_attr_init(&attr) == 0
		&& pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM) == 0
		&& pthread_create(&pch_pid, &attr, make_pch, NULL) == 0) {
		int i;

		/* Use the detached threads not to fiddle with major code
		   processing SICHLD.  */
		pthread_detach(pch_pid);
		init_state = 4;
		for (i = 0; i < mjit_opts.threads; i++) {
		    if (pthread_create(&pid, &attr, worker, NULL) != 0)
			break;
		    worker_pids[workers_num++] = pid;
		    pthread_detach(pid);
		}
		if (i == mjit_opts.threads)
		    init_state = 5;
	    }
	}
      }
    }
    switch (init_state) {
    case 4:
	mjit_init_p = TRUE;
	/* Fall through: */
    case 3:
	pthread_cond_destroy(&mjit_worker_wakeup);
	/* Fall through: */
    case 2:
	pthread_cond_destroy(&mjit_client_wakeup);
	/* Fall through: */
    case 1:
	pthread_cond_destroy(&mjit_pch_wakeup);
	/* Fall through: */
    case 0:
	free(header_fname); header_fname = NULL;
	free(cc_path); cc_path = NULL;
	free(pch_fname); pch_fname = NULL;
	pthread_mutex_destroy(&mjit_engine_mutex);
	verbose("Failure in MJIT initialization");
	return;
    }
    mjit_init_p = TRUE;
    verbose("Successful MJIT initialization (workers = %d)", mjit_opts.threads);
}

/* Return number of done batch iseqs.  */
static int
get_done_batch_iseqs_num(void) {
    int n = 0;
    struct rb_mjit_batch *b;
    struct rb_mjit_batch_iseq *bi;
    
    for (b = done_batches.head; b != NULL; b = b->next)
	for (bi = b->first; bi != NULL; bi = bi->next)
	    n++;
    return n;
}

/* Used to sort batch iseqs to put most frequently called ones first.
   If the call numbers are equal put bigger iseqs first.  */ 
static int
batch_iseq_compare(const void *el1, const void *el2) {
    const struct rb_mjit_batch_iseq *bi1 = *(struct rb_mjit_batch_iseq * const *) el1;
    const struct rb_mjit_batch_iseq *bi2 = *(struct rb_mjit_batch_iseq * const *) el2;
    unsigned long overall_calls1 = (bi1->iseq == NULL ? bi1->overall_calls : bi1->iseq->body->overall_calls);
    unsigned long overall_calls2 = (bi2->iseq == NULL ? bi2->overall_calls : bi2->iseq->body->overall_calls);

    if (overall_calls2 < overall_calls1) return -1;
    if (overall_calls1 < overall_calls2) return 1;
    return (long) bi2->iseq_size - (long) bi1->iseq_size;
}

/* Allocate and return a new array of done batch iseqs sorted
   according criteria described in batch_iseq_compare.  The array has
   null end marker.  */
static struct rb_mjit_batch_iseq **
get_sorted_batch_iseqs(void) {
    int n, batch_iseqs_num = get_done_batch_iseqs_num();
    struct rb_mjit_batch_iseq **batch_iseqs = xmalloc(sizeof(struct rb_mjit_batch_iseq *) * (batch_iseqs_num + 1));
    struct rb_mjit_batch *b;
    struct rb_mjit_batch_iseq *bi;
    
    if (batch_iseqs == NULL)
	return NULL;
    n = 0;
    for (b = done_batches.head; b != NULL; b = b->next)
	for (bi = b->first; bi != NULL; bi = bi->next)
	    batch_iseqs[n++] = bi;
    batch_iseqs[n] = NULL;
    assert(n == batch_iseqs_num);
    qsort(batch_iseqs, batch_iseqs_num, sizeof(struct rb_mjit_batch_iseq *), batch_iseq_compare);
    return batch_iseqs;
}

/* Print statistics about ISEQ calls to stderr.  Statistics is printed
   only for JITed iseqs.  */
static void
print_statistics(void) {
    int n;
    struct rb_mjit_batch_iseq *bi, **batch_iseqs;

    if ((batch_iseqs = get_sorted_batch_iseqs()) == NULL)
	return;

    fprintf(stderr, "Name                                          Batch Iseq  Size     Calls/JIT Calls               Spec Fails\n");
    for (n = 0; (bi = batch_iseqs[n]) != NULL; n++) {
	int i;
	unsigned long overall_calls
	    = (bi->iseq == NULL ? bi->overall_calls : bi->iseq->body->overall_calls);
	unsigned long jit_calls
	    = (bi->iseq == NULL ? bi->jit_calls : bi->iseq->body->jit_calls);
	unsigned long failed_jit_calls
	    = (bi->iseq == NULL ? bi->jit_calls : bi->iseq->body->failed_jit_calls);
	
	fprintf(stderr, "%-45s %5d %4d %5lu %9lu/%-9lu (%-5.2f%%)   %9lu (%6.2f%%)",
		bi->label, bi->batch->num, bi->num, bi->iseq_size,
		overall_calls, jit_calls, overall_calls == 0 ? 0.0 : jit_calls * 100.0 / overall_calls,
		failed_jit_calls, jit_calls == 0 ? 0.0 : failed_jit_calls * 100.0 / jit_calls);
	/* Print insns whose failed spec variant caused mutations.  */
	for (i = 0; i < bi->jit_mutations_num; i++)
	    if (bi->mutation_insns[i].insn == BIN(nop))
		fprintf (stderr, " -");
	    else
		fprintf (stderr, " %s", insn_name((VALUE) bi->mutation_insns[i].insn));
	fprintf(stderr, "\n");
    }
    free(batch_iseqs);
}

/* Finish the threads processing batches and creating PCH, finalize
   and free MJIT data.  It should be called last during MJIT
   life.  */
void
mjit_finish(void) {
    if (!mjit_init_p)
	return;
    debug(1, "Initiate finishing MJIT");
    debug(2, "Canceling pch and worker threads");
    /* As our threads are detached, we could just cancel them.  But it
       is a bad idea because OS processes (C compiler) started by
       threads can produce temp files.  And even if the temp files are
       removed, the used C compiler still complaint about their
       absence.  So wait for a clean finish of the threads.  */
    CRITICAL_SECTION_START("in mjit_finish to wakeup from pch");
    while (pch_status == PCH_NOT_READY) {
	debug(3, "Waiting wakeup from make_pch");
	pthread_cond_wait(&mjit_pch_wakeup, &mjit_engine_mutex);
    }
    CRITICAL_SECTION_FINISH("in mjit_finish to wakeup from pch");
    finish_workers_p = TRUE;
    while (finished_workers < workers_num) {
	debug(3, "Sending cancel signal to workers");
	CRITICAL_SECTION_START("in mjit_finish");
	if (pthread_cond_broadcast(&mjit_worker_wakeup) != 0) {
	    fprintf(stderr, "Cannot send wakeup signal to workers in mjit_finish: time - %.3f ms\n",
		    relative_ms_time());
	}
	CRITICAL_SECTION_FINISH("in mjit_finish");
    }
    pthread_mutex_destroy(&mjit_engine_mutex);
    pthread_cond_destroy(&mjit_pch_wakeup);
    pthread_cond_destroy(&mjit_client_wakeup);
    pthread_cond_destroy(&mjit_worker_wakeup);
    if (! mjit_opts.save_temps)
	remove(pch_fname);
    if (mjit_opts.profile)
	print_statistics();
#if MJIT_INSN_STATISTICS
    fprintf(stderr, "Executed insns (all/jit): %8lu/%-8lu (%5.2f%%)\n",
	    byte_code_insns_num + jit_insns_num, jit_insns_num,
	    (byte_code_insns_num + jit_insns_num) == 0
	    ? 0.0 : jit_insns_num * 100.0 / (byte_code_insns_num + jit_insns_num));
#endif
    free(header_fname); header_fname = NULL;
    free(cc_path); cc_path = NULL;
    free(pch_fname); pch_fname = NULL;
    finish_workers();
    mjit_init_p = FALSE;
    verbose("Successful MJIT finish");
}
