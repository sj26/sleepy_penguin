#include "sleepy_penguin.h"
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

#ifndef SSIZET2NUM
#  define SSIZET2NUM(x) LONG2NUM(x)
#endif
#ifndef NUM2SIZET
#  define NUM2SIZET(x) NUM2ULONG(x)
#endif

static int check_fileno(VALUE io)
{
	int saved_errno = errno;
	int fd = rb_sp_fileno(io);
	errno = saved_errno;
	return fd;
}

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

struct splice_args {
	int fd_in;
	int fd_out;
	off_t *off_in;
	off_t *off_out;
	size_t len;
	unsigned flags;
};

static void *nogvl_splice(void *ptr)
{
	struct splice_args *a = ptr;

	return (void *)splice(a->fd_in, a->off_in, a->fd_out, a->off_out,
	                     a->len, a->flags);
}

static ssize_t do_splice(int argc, VALUE *argv, unsigned dflags)
{
	off_t i = 0, o = 0;
	VALUE io_in, off_in, io_out, off_out, len, flags;
	struct splice_args a;
	ssize_t bytes;
	ssize_t total = 0;

	rb_scan_args(argc, argv, "51",
	             &io_in, &off_in, &io_out, &off_out, &len, &flags);

	a.off_in = NIL_P(off_in) ? NULL : (i = NUM2OFFT(off_in), &i);
	a.off_out = NIL_P(off_out) ? NULL : (o = NUM2OFFT(off_out), &o);
	a.len = NUM2SIZET(len);
	a.flags = NIL_P(flags) ? dflags : NUM2UINT(flags) | dflags;

	for (;;) {
		a.fd_in = check_fileno(io_in);
		a.fd_out = check_fileno(io_out);
		bytes = (ssize_t)IO_RUN(nogvl_splice, &a);
		if (bytes < 0) {
			if (errno == EINTR)
				continue;
			if (total > 0)
				return total;
			return bytes;
		} else if (bytes == 0) {
			break;
		} else {
			return bytes;
		}
	}

	return total;
}

/*
 * call-seq:
 *    SleepyPenguin.splice(io_in, off_in, io_out, off_out, len) => integer
 *    SleepyPenguin.splice(io_in, off_in, io_out, off_out, len, flags) => integer
 *
 * Splice +len+ bytes from/to a pipe.  Either +io_in+ or +io_out+
 * MUST be a pipe.  +io_in+ and +io_out+ may BOTH be pipes as of
 * Linux 2.6.31 or later.
 *
 * +off_in+ and +off_out+ if non-nil may be used to
 * specify an offset for the non-pipe file descriptor.
 *
 * +flags+ defaults to zero if unspecified.
 * +flags+ may be a bitmask of the following flags:
 *
 * * SleepyPenguin::F_MOVE
 * * SleepyPenguin::F_NONBLOCK
 * * SleepyPenguin::F_MORE
 *
 * Returns the number of bytes spliced.
 * Raises EOFError when +io_in+ has reached end of file.
 * Raises Errno::EAGAIN if the SleepyPenguin::F_NONBLOCK flag is set
 * and the pipe has no data to read from or space to write to.  May
 * also raise Errno::EAGAIN if the non-pipe descriptor has no data
 * to read from or space to write to.
 *
 * As splice never exposes buffers to userspace, it will not take
 * into account userspace buffering done by Ruby or stdio.  It is
 * also not subject to encoding/decoding filters under Ruby 1.9.
 *
 * Consider using SleepyPenguin.trysplice if +io_out+ is a pipe or if you are using
 * non-blocking I/O on both descriptors as it avoids the cost of raising
 * common Errno::EAGAIN exceptions.
 *
 * See manpage for full documentation:
 * http://kernel.org/doc/man-pages/online/pages/man2/splice.2.html
 */
static VALUE my_splice(int argc, VALUE *argv, VALUE self)
{
	ssize_t n = do_splice(argc, argv, 0);

	if (n == 0)
		rb_eof_error();
	if (n < 0)
		rb_sys_fail("splice");
	return SSIZET2NUM(n);
}

/*
 * call-seq:
 *    SleepyPenguin.trysplice(io_in, off_in, io_out, off_out, len) => integer
 *    SleepyPenguin.trysplice(io_in, off_in, io_out, off_out, len, flags) => integer
 *
 * Exactly like SleepyPenguin.splice, except +:EAGAIN+ is returned when either
 * the read or write end would block instead of raising Errno::EAGAIN.
 *
 * SleepyPenguin::F_NONBLOCK is always passed for the pipe descriptor,
 * but this can still block if the non-pipe descriptor is blocking.
 *
 * See SleepyPenguin.splice documentation for more details.
 *
 * This method is recommended whenever +io_out+ is a pipe.
 */
static VALUE trysplice(int argc, VALUE *argv, VALUE self)
{
	ssize_t n = do_splice(argc, argv, SPLICE_F_NONBLOCK);

	if (n == 0)
		return Qnil;
	if (n < 0) {
		if (errno == EAGAIN)
			return sym_EAGAIN;
		rb_sys_fail("splice");
	}
	return SSIZET2NUM(n);
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

static ssize_t do_tee(int argc, VALUE *argv, unsigned dflags)
{
	VALUE io_in, io_out, len, flags;
	struct tee_args a;
	ssize_t bytes;
	ssize_t total = 0;

	rb_scan_args(argc, argv, "31", &io_in, &io_out, &len, &flags);
	a.len = (size_t)NUM2SIZET(len);
	a.flags = NIL_P(flags) ? dflags : NUM2UINT(flags) | dflags;

	for (;;) {
		a.fd_in = check_fileno(io_in);
		a.fd_out = check_fileno(io_out);
		bytes = (ssize_t)IO_RUN(nogvl_tee, &a);
		if (bytes < 0) {
			if (errno == EINTR)
				continue;
			if (total > 0)
				return total;
			return bytes;
		} else if (bytes == 0) {
			break;
		} else {
			return bytes;
		}
	}

	return total;
}

/*
 * call-seq:
 *   SleepyPenguin.tee(io_in, io_out, len) => integer
 *   SleepyPenguin.tee(io_in, io_out, len, flags) => integer
 *
 * Copies up to +len+ bytes of data from +io_in+ to +io_out+.  +io_in+
 * and +io_out+ must both refer to pipe descriptors.  +io_in+ and +io_out+
 * may not be endpoints of the same pipe.
 *
 * +flags+ may be zero (the default) or a combination of:
 * * SleepyPenguin::F_NONBLOCK
 *
 * Other splice-related flags are currently unimplemented or have no effect.
 *
 * Returns the number of bytes duplicated if successful.
 * Raises EOFError when +io_in+ is closed and emptied.
 * Raises Errno::EAGAIN when +io_in+ is empty and/or +io_out+ is full
 * and +flags+ contains SleepyPenguin::F_NONBLOCK
 *
 * Consider using SleepyPenguin.trytee if you are using
 * SleepyPenguin::F_NONBLOCK as it avoids the cost of raising
 * common Errno::EAGAIN exceptions.
 *
 * See manpage for full documentation:
 * http://kernel.org/doc/man-pages/online/pages/man2/tee.2.html
 */
static VALUE my_tee(int argc, VALUE *argv, VALUE self)
{
	ssize_t n = do_tee(argc, argv, 0);

	if (n == 0)
		rb_eof_error();
	if (n < 0)
		rb_sys_fail("tee");

	return SSIZET2NUM(n);
}

/*
 * call-seq:
 *    SleepyPenguin.trytee(io_in, io_out, len) => integer
 *    SleepyPenguin.trytee(io_in, io_out, len, flags) => integer
 *
 * Exactly like SleepyPenguin.tee, except +:EAGAIN+ is returned when either
 * the read or write end would block instead of raising Errno::EAGAIN.
 *
 * SleepyPenguin::F_NONBLOCK is always passed for the pipe descriptor,
 * but this can still block if the non-pipe descriptor is blocking.
 *
 * See SleepyPenguin.tee documentation for more details.
 */
static VALUE trytee(int argc, VALUE *argv, VALUE self)
{
	ssize_t n = do_tee(argc, argv, SPLICE_F_NONBLOCK);

	if (n == 0)
		return Qnil;
	if (n < 0) {
		if (errno == EAGAIN)
			return sym_EAGAIN;
		rb_sys_fail("tee");
	}

	return SSIZET2NUM(n);
}

void sleepy_penguin_init_splice(void)
{
	VALUE mod = rb_define_module("SleepyPenguin");
	rb_define_singleton_method(mod, "splice", my_splice, -1);
	rb_define_singleton_method(mod, "trysplice", trysplice, -1);
	rb_define_singleton_method(mod, "tee", my_tee, -1);
	rb_define_singleton_method(mod, "trytee", trytee, -1);

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
