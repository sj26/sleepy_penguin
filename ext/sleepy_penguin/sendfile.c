#include "sleepy_penguin.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

#if defined(HAVE_SYS_SENDFILE_H) && !defined(HAVE_BSD_SENDFILE)
#  include <sys/sendfile.h>
#endif

#if defined(__linux__) && defined(HAVE_SENDFILE)
#  define linux_sendfile(in_fd, out_fd, offset, count) \
		sendfile((in_fd),(out_fd),(offset),(count))

/* all good */
#elif defined(HAVE_SENDFILE) && \
      (defined(__FreeBSD__) || defined(__DragonFly__))
/*
 * make BSD sendfile look like Linux for now...
 * we can support SF_NODISKIO later
 */
static ssize_t linux_sendfile(int sockfd, int filefd, off_t *off, size_t count)
{
	off_t sbytes = 0;
        off_t offset = off ? *off : lseek(filefd, 0, SEEK_CUR);

	int rc = sendfile(filefd, sockfd, offset, count, NULL, &sbytes, 0);
	if (sbytes > 0) {
		if (off)
			*off += sbytes;
		else
			lseek(filefd, sbytes, SEEK_CUR);
		return (ssize_t)sbytes;
	}

	return (ssize_t)rc;
}
#else /* emulate sendfile using (read|pread) + write */
static ssize_t pread_sendfile(int sockfd, int filefd, off_t *off, size_t count)
{
	size_t max_read = 16384;
	void *buf;
	ssize_t r;
	ssize_t w;

	max_read = count > max_read ? max_read : count;
	buf = xmalloc(max_read);

	do {
		r = off ? pread(filefd, buf, max_read, *off) :
			  read(filefd, buf, max_read);
	} while (r < 0 && errno == EINTR);

	if (r <= 0) {
		int err = errno;
		xfree(buf);
		errno = err;
		return r;
	}
	w = write(sockfd, buf, r);
	if (w > 0 && off)
		*off += w;
	xfree(buf);
	return w;
}
#    define linux_sendfile(out_fd, in_fd, offset, count) \
            pread_sendfile((out_fd),(in_fd),(offset),(count))
#endif

struct sf_args {
	int dst_fd;
	int src_fd;
	off_t *off;
	size_t count;
};

static VALUE sym_wait_writable;

static VALUE nogvl_sf(void *ptr)
{
	struct sf_args *a = ptr;

	return (VALUE)linux_sendfile(a->dst_fd, a->src_fd, a->off, a->count);
}

static VALUE lsf(VALUE mod, VALUE dst, VALUE src, VALUE src_off, VALUE count)
{
	off_t off = 0;
	struct sf_args a;
	ssize_t bytes;
	int retried = 0;

	a.off = NIL_P(src_off) ? NULL : (off = NUM2OFFT(src_off), &off);
	a.count = NUM2SIZET(count);
again:
	a.src_fd = rb_sp_fileno(src);
	a.dst_fd = rb_sp_fileno(dst);
	bytes = (ssize_t)rb_sp_fd_region(nogvl_sf, &a, a.dst_fd);
	if (bytes < 0) {
		switch (errno) {
		case EAGAIN:
			return sym_wait_writable;
		case ENOMEM:
		case ENOBUFS:
			if (!retried) {
				rb_gc();
				retried = 1;
				goto again;
			}
		}
		rb_sys_fail("sendfile");
	}
	return SSIZET2NUM(bytes);
}

void sleepy_penguin_init_sendfile(void)
{
	VALUE m = rb_define_module("SleepyPenguin");
	rb_define_singleton_method(m, "__lsf", lsf, 4);
	sym_wait_writable = ID2SYM(rb_intern("wait_writable"));
}
