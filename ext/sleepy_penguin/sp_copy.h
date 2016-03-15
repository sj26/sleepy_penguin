/* common splice and copy_file_range-related definitions */

#ifndef SSIZET2NUM
#  define SSIZET2NUM(x) LONG2NUM(x)
#endif
#ifndef NUM2SIZET
#  define NUM2SIZET(x) NUM2ULONG(x)
#endif

#if defined(HAVE_RB_THREAD_CALL_WITHOUT_GVL) && defined(HAVE_RUBY_THREAD_H)
/* Ruby 2.0+ */
#  include <ruby/thread.h>
#  define WITHOUT_GVL(fn,a,ubf,b) \
        rb_thread_call_without_gvl((fn),(a),(ubf),(b))
#elif defined(HAVE_RB_THREAD_BLOCKING_REGION)
typedef VALUE (*my_blocking_fn_t)(void*);
#  define WITHOUT_GVL(fn,a,ubf,b) \
	rb_thread_blocking_region((my_blocking_fn_t)(fn),(a),(ubf),(b))

#else /* Ruby 1.8 */
/* partial emulation of the 1.9 rb_thread_blocking_region under 1.8 */
#  include <rubysig.h>
#  define RUBY_UBF_IO ((rb_unblock_function_t *)-1)
typedef void rb_unblock_function_t(void *);
typedef void * rb_blocking_function_t(void *);
static void * WITHOUT_GVL(rb_blocking_function_t *func, void *data1,
			rb_unblock_function_t *ubf, void *data2)
{
	void *rv;

	assert(RUBY_UBF_IO == ubf && "RUBY_UBF_IO required for emulation");

	TRAP_BEG;
	rv = func(data1);
	TRAP_END;

	return rv;
}
#endif /* ! HAVE_RB_THREAD_BLOCKING_REGION */

#define IO_RUN(fn,data) WITHOUT_GVL((fn),(data),RUBY_UBF_IO,0)

struct copy_args {
	int fd_in;
	int fd_out;
	off_t *off_in;
	off_t *off_out;
	size_t len;
	unsigned flags;
};
