#include "sleepy_penguin.h"
#include "sp_copy.h"
#ifdef HAVE_SPLICE
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/uio.h>
#include <limits.h>
#include <unistd.h>

static VALUE sym_EAGAIN;

#ifndef F_LINUX_SPECIFIC_BASE
#  define F_LINUX_SPECIFIC_BASE 1024
#endif

#ifndef F_GETPIPE_SZ
#  define F_SETPIPE_SZ    (F_LINUX_SPECIFIC_BASE + 7)
#  define F_GETPIPE_SZ    (F_LINUX_SPECIFIC_BASE + 8)
#endif

static int check_fileno(VALUE io)
{
	int saved_errno = errno;
	int fd = rb_sp_fileno(io);
	errno = saved_errno;
	return fd;
}

static void *nogvl_splice(void *ptr)
{
	struct copy_args *a = ptr;

	return (void *)splice(a->fd_in, a->off_in, a->fd_out, a->off_out,
	                     a->len, a->flags);
}

/* :nodoc: */
static VALUE my_splice(VALUE mod, VALUE io_in, VALUE off_in,
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
		a.fd_in = check_fileno(io_in);
		a.fd_out = check_fileno(io_out);
		bytes = (ssize_t)IO_RUN(nogvl_splice, &a);
		if (bytes == 0) return Qnil;
		if (bytes < 0) {
			switch (errno) {
			case EINTR: continue;
			case EAGAIN: return sym_EAGAIN;
			default: rb_sys_fail("splice");
			}
		}
		return SSIZET2NUM(bytes);
	}
}

struct tee_args {
	int fd_in;
	int fd_out;
	size_t len;
	unsigned flags;
};

/* runs without GVL */
static void *nogvl_tee(void *ptr)
{
	struct tee_args *a = ptr;

	return (void *)tee(a->fd_in, a->fd_out, a->len, a->flags);
}

/* :nodoc: */
static VALUE my_tee(VALUE mod, VALUE io_in, VALUE io_out,
			VALUE len, VALUE flags)
{
	struct tee_args a;
	ssize_t bytes;

	a.len = (size_t)NUM2SIZET(len);
	a.flags = NUM2UINT(flags);

	for (;;) {
		a.fd_in = check_fileno(io_in);
		a.fd_out = check_fileno(io_out);
		bytes = (ssize_t)IO_RUN(nogvl_tee, &a);
		if (bytes == 0) return Qnil;
		if (bytes < 0) {
			switch (errno) {
			case EINTR: continue;
			case EAGAIN: return sym_EAGAIN;
			default: rb_sys_fail("tee");
			}
		}
		return SSIZET2NUM(bytes);
	}
}

void sleepy_penguin_init_splice(void)
{
	VALUE mod = rb_define_module("SleepyPenguin");
	rb_define_singleton_method(mod, "__splice", my_splice, 6);
	rb_define_singleton_method(mod, "__tee", my_tee, 4);

	/*
	 * Attempt to move pages instead of copying.  This is only a hint
	 * and support for it was removed in Linux 2.6.21.  It will be
         * re-added for FUSE filesystems only in Linux 2.6.35.
	 */
	rb_define_const(mod, "F_MOVE", UINT2NUM(SPLICE_F_MOVE));

	/*
	 * Do not block on pipe I/O.  This flag only affects the pipe(s)
	 * being spliced from/to and has no effect on the non-pipe
	 * descriptor (which requires non-blocking operation to be set
	 * explicitly).
	 *
	 * The non-blocking flag (O_NONBLOCK) on the pipe descriptors
	 * themselves are ignored by this family of functions, and
	 * using this flag is the only way to get non-blocking operation
	 * out of them.
	 *
	 * It is highly recommended this flag be set
         * (or SleepyPenguin.trysplice used)
	 * whenever splicing from a socket into a pipe unless there is
	 * another (native) thread or process doing a blocking read on that
	 * pipe.  Otherwise it is possible to block a single-threaded process
	 * if the socket buffers are larger than the pipe buffers.
	 */
	rb_define_const(mod, "F_NONBLOCK", UINT2NUM(SPLICE_F_NONBLOCK));

	/*
	 * Indicate that there may be more data coming into the outbound
	 * descriptor.  This can allow the kernel to avoid sending partial
	 * frames from sockets.  Currently only used with splice.
	 */
	rb_define_const(mod, "F_MORE", UINT2NUM(SPLICE_F_MORE));

	/*
	 * The maximum size of an atomic write to a pipe
	 * POSIX requires this to be at least 512 bytes.
	 * Under Linux, this is 4096 bytes.
	 */
	rb_define_const(mod, "PIPE_BUF", UINT2NUM(PIPE_BUF));

	/*
	 * fcntl() command constant used to return the size of a pipe.
	 * This constant is only defined when running Linux 2.6.35
	 * or later.  For convenience, use IO#pipe_size instead.
	 */
	rb_define_const(mod, "F_GETPIPE_SZ", UINT2NUM(F_GETPIPE_SZ));

	/*
	 * fcntl() command constant used to set the size of a pipe.
	 * This constant is only defined when running Linux 2.6.35
	 * or later.  For convenience, use IO#pipe_size= instead.
	 */
	rb_define_const(mod, "F_SETPIPE_SZ", UINT2NUM(F_SETPIPE_SZ));

	sym_EAGAIN = ID2SYM(rb_intern("EAGAIN"));
}
#endif
