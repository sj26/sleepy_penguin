#ifndef SLEEPY_PENGUIN_H
#define SLEEPY_PENGUIN_H

#include <ruby.h>
#ifdef HAVE_RUBY_IO_H
#  include <ruby/io.h>
#else
#  include <rubyio.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>

extern size_t rb_sp_l1_cache_line_size;
unsigned rb_sp_get_uflags(VALUE klass, VALUE flags);
int rb_sp_get_flags(VALUE klass, VALUE flags, int default_flags);
int rb_sp_io_closed(VALUE io);
int rb_sp_fileno(VALUE io);
void rb_sp_set_nonblock(int fd);

#ifdef HAVE_RB_THREAD_IO_BLOCKING_REGION
/* Ruby 1.9.3 and 2.0.0 */
VALUE rb_thread_io_blocking_region(rb_blocking_function_t *, void *, int);
#  define rb_sp_fd_region(fn,data,fd) \
	rb_thread_io_blocking_region((fn),(data),(fd))
#elif defined(HAVE_RB_THREAD_CALL_WITHOUT_GVL) && \
	defined(HAVE_RUBY_THREAD_H) && HAVE_RUBY_THREAD_H
/* in case Ruby 2.0+ ever drops rb_thread_io_blocking_region: */
#  include <ruby/thread.h>
#  define COMPAT_FN (void *(*)(void *))
#  define rb_sp_fd_region(fn,data,fd) \
	rb_thread_call_without_gvl(COMPAT_FN(fn),(data),RUBY_UBF_IO,NULL)
#elif defined(HAVE_RB_THREAD_BLOCKING_REGION)
/* Ruby 1.9.2 */
#  define rb_sp_fd_region(fn,data,fd) \
	rb_thread_blocking_region((fn),(data),RUBY_UBF_IO,NULL)
#else
#  error Ruby <= 1.8 not supported
#endif

#define NODOC_CONST(klass,name,value) \
  rb_define_const((klass),(name),(value))

#ifdef HAVE_RB_FD_FIX_CLOEXEC
#  define RB_SP_CLOEXEC(flag) (flag)
#else
#  define RB_SP_CLOEXEC(flag) (0)
#endif

typedef int rb_sp_waitfn(int fd);
int rb_sp_wait(rb_sp_waitfn waiter, VALUE obj, int *fd);
void *rb_sp_gettlsbuf(size_t *size);
VALUE rb_sp_puttlsbuf(VALUE);

/* Flexible array elements are standard in C99 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
# define FLEX_ARRAY
#elif defined(__GNUC__)
# if (__GNUC__ >= 3)
#  define FLEX_ARRAY
# else
#  define FLEX_ARRAY 0
# endif
#endif

int rb_sp_gc_for_fd(int err);

#ifndef HAVE_COPY_FILE_RANGE
#  include <sys/syscall.h>
#  if !defined(__NR_copy_file_range) && defined(__linux__)
#    if defined(__x86_64__)
#      define __NR_copy_file_range 326
#    elif defined(__i386__)
#      define __NR_copy_file_range 377
#    endif /* supported arches */
#  endif /* __NR_copy_file_range */
#endif

#endif /* SLEEPY_PENGUIN_H */
