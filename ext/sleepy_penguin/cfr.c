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

static VALUE rb_cfr(int argc, VALUE *argv, VALUE mod)
{
	off_t i, o;
	VALUE io_in, off_in, io_out, off_out, len, flags;
	ssize_t bytes;
	struct copy_args a;

	rb_scan_args(argc, argv, "51",
	             &io_in, &off_in, &io_out, &off_out, &len, &flags);

	a.off_in = NIL_P(off_in) ? NULL : (i = NUM2OFFT(off_in), &i);
	a.off_out = NIL_P(off_out) ? NULL : (o = NUM2OFFT(off_out), &o);
	a.len = NUM2SIZET(len);
	a.flags = NIL_P(flags) ? 0 : NUM2UINT(flags);

again:
	a.fd_in = rb_sp_fileno(io_in);
	a.fd_out = rb_sp_fileno(io_out);
	bytes = (ssize_t)IO_RUN(nogvl_cfr, &a);
	if (bytes < 0) {
		if (errno == EINTR)
			goto again;
		rb_sys_fail("copy_file_range");
	} else if (bytes == 0) {
		rb_eof_error();
	}
	return SSIZET2NUM(bytes);
}

void sleepy_penguin_init_cfr(void)
{
	VALUE mod = rb_define_module("SleepyPenguin");

	rb_define_singleton_method(mod, "copy_file_range", rb_cfr, -1);
}
