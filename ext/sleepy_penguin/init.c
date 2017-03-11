#include <ruby.h>
#ifndef _GNU_SOURCE
#  define _GNU_SOURCE /* TODO: confirm this is needed */
#endif

#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include "git_version.h"
#include "sleepy_penguin.h"
#define L1_CACHE_LINE_MAX 128 /* largest I've seen (Pentium 4) */
size_t rb_sp_l1_cache_line_size;
static pthread_key_t rb_sp_key;
struct rb_sp_tlsbuf {
	size_t capa;
	unsigned char ptr[FLEX_ARRAY];
};

#ifdef HAVE_SYS_EVENT_H
void sleepy_penguin_init_kqueue(void);
#else
#  define sleepy_penguin_init_kqueue() for(;0;)
#endif

#ifdef HAVE_SYS_EPOLL_H
void sleepy_penguin_init_epoll(void);
#else
#  define sleepy_penguin_init_epoll() for(;0;)
#endif

#ifdef HAVE_SYS_TIMERFD_H
void sleepy_penguin_init_timerfd(void);
#else
#  define sleepy_penguin_init_timerfd() for(;0;)
#endif

#ifdef HAVE_SYS_EVENTFD_H
void sleepy_penguin_init_eventfd(void);
#else
#  define sleepy_penguin_init_eventfd() for(;0;)
#endif

#ifdef HAVE_SYS_INOTIFY_H
void sleepy_penguin_init_inotify(void);
#else
#  define sleepy_penguin_init_inotify() for(;0;)
#endif

#ifdef HAVE_SYS_SIGNALFD_H
void sleepy_penguin_init_signalfd(void);
#else
#  define sleepy_penguin_init_signalfd() for(;0;)
#endif

#ifdef HAVE_SPLICE
void sleepy_penguin_init_splice(void);
#else
#  define sleepy_penguin_init_splice() for(;0;)
#endif

#if defined(HAVE_COPY_FILE_RANGE) || \
    (defined(__linux__) && defined(__NR_copy_file_range))
void sleepy_penguin_init_cfr(void);
#else
#  define sleepy_penguin_init_cfr() for (;0;)
#endif

/* everyone */
void sleepy_penguin_init_sendfile(void);

static size_t l1_cache_line_size_detect(void)
{
#ifdef _SC_LEVEL1_DCACHE_LINESIZE
	long tmp = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);

	if (tmp > 0 && tmp <= L1_CACHE_LINE_MAX)
		return (size_t)tmp;
#endif /* _SC_LEVEL1_DCACHE_LINESIZE */
	return L1_CACHE_LINE_MAX;
}

static void sp_once(void)
{
	int err = pthread_key_create(&rb_sp_key, free);

	if (err) {
		errno = err;
		rb_sys_fail( "pthread_key_create");
	}
}

void *rb_sp_gettlsbuf(size_t *size)
{
	struct rb_sp_tlsbuf *buf = pthread_getspecific(rb_sp_key);
	void *ptr;
	int err;
	size_t bytes;

	if (buf && buf->capa >= *size) {
		*size = buf->capa;
		goto out;
	}

	free(buf);
	bytes = *size + sizeof(struct rb_sp_tlsbuf);
	err = posix_memalign(&ptr, rb_sp_l1_cache_line_size, bytes);
	if (err) {
		errno = err;
		rb_memerror(); /* fatal */
	}

	buf = ptr;
	buf->capa = *size;
	err = pthread_setspecific(rb_sp_key, buf);
	if (err != 0) {
		errno = err;
		rb_sys_fail("BUG: pthread_setspecific");
	}
out:
	return buf->ptr;
}

void Init_sleepy_penguin_ext(void)
{
	VALUE mSleepyPenguin;
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	int err = pthread_once(&once, sp_once);

	if (err) {
		errno = err;
		rb_sys_fail("pthread_once");
	}

	rb_sp_l1_cache_line_size = l1_cache_line_size_detect();

	mSleepyPenguin = rb_define_module("SleepyPenguin");
	rb_define_const(mSleepyPenguin, "SLEEPY_PENGUIN_VERSION",
			rb_str_new2(MY_GIT_VERSION));

	sleepy_penguin_init_kqueue();
	sleepy_penguin_init_epoll();
	sleepy_penguin_init_timerfd();
	sleepy_penguin_init_eventfd();
	sleepy_penguin_init_inotify();
	sleepy_penguin_init_signalfd();
	sleepy_penguin_init_splice();
	sleepy_penguin_init_cfr();
	sleepy_penguin_init_sendfile();
}
