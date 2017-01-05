#include "sleepy_penguin.h"
#include "sp_copy.h"
#include <unistd.h>

#ifndef HAVE_COPY_FILE_RANGE
#  include <sys/syscall.h>
#  if !defined(__NR_copy_file_range)
#    if defined(__x86_64__)
#      define __NR_copy_file_range 326
#    elif defined(__i386__)
#      define __NR_copy_file_range 377
#    endif /* supported arches */
#  endif /* __NR_copy_file_range */
#endif

#ifdef __NR_copy_file_range
static ssize_t my_cfr(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
		       size_t len, unsigned int flags)
{
	long n = syscall(__NR_copy_file_range,
			fd_in, off_in, fd_out, off_out, len, flags);

	return (ssize_t)n;
}
#  define copy_file_range(fd_in,off_in,fd_out,off_out,len,flags) \
		my_cfr((fd_in),(off_in),(fd_out),(off_out),(len),(flags))
#endif

static void *nogvl_cfr(void *ptr)
{
	struct copy_args *a = ptr;

	return (void *)copy_file_range(a->fd_in, a->off_in,
				a->fd_out, a->off_out, a->len, a->flags);
}

/* :nodoc: */
static VALUE rb_sp_cfr(VALUE mod, VALUE io_in, VALUE off_in,
			VALUE io_out, VALUE off_out,
			VALUE len, VALUE flags)
{
	off_t i = 0, o = 0;
	struct copy_args a;
	ssize_t bytes;

	a.off_in = NIL_P(off_in) ? NULL : (i = NUM2OFFT(off_in), &i);
	a.off_out = NIL_P(off_out) ? NULL : (o = NUM2OFFT(off_out), &o);
	a.len = NUM2SIZET(len);
	a.flags = NUM2UINT(flags);

	for (;;) {
		a.fd_in = rb_sp_fileno(io_in);
		a.fd_out = rb_sp_fileno(io_out);
		bytes = (ssize_t)IO_RUN(nogvl_cfr, &a);
		if (bytes < 0) {
			switch (errno) {
			case EINTR: continue;
			default: rb_sys_fail("copy_file_range");
			}
		}
		return SSIZET2NUM(bytes);
	}
}

void sleepy_penguin_init_cfr(void)
{
	VALUE mod = rb_define_module("SleepyPenguin");

	rb_define_singleton_method(mod, "__cfr", rb_sp_cfr, 6);
}
