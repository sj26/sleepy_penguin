#include "sleepy_penguin.h"
#ifdef HAVE_SYS_EVENT_H
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include "missing_clock_gettime.h"
#include "missing_rb_thread_fd_close.h"
#include "missing_rb_update_max_fd.h"
#include "value2timespec.h"

#ifdef HAVE_SYS_MOUNT_H /* for VQ_* flags on FreeBSD */
#  include <sys/mount.h>
#endif

/* not bothering with overflow checking for backwards compat */
#ifndef RARRAY_LENINT
#  define RARRAY_LENINT(ary) (int)RARRAY_LEN(ary)
#endif
#ifndef RARRAY_CONST_PTR
#  define RARRAY_CONST_PTR(ary) RARRAY_PTR(ary)
#endif
#ifndef NUM2SHORT
#  define NUM2SHORT(n) (short)NUM2INT(n)
#endif
#ifndef NUM2USHORT
#  define NUM2USHORT(n) (short)NUM2UINT(n)
#endif

/*
 * Rubinius does not support RSTRUCT_* in the C API:
 * ref: https://github.com/rubinius/rubinius/issues/494
 */
#if defined(RUBINIUS)
#  define RBX_STRUCT (1)
#  define RSTRUCT_LEN(s) 0, rb_bug("RSTRUCT_LEN attempted in Rubinius")
#  define RSTRUCT_PTR(s) NULL, rb_bug("RSTRUCT_PTR attempted in Rubinius")
#else
#  define RBX_STRUCT (0)
#endif

static const long NANO_PER_SEC = 1000000000;
static ID id_for_fd;
static VALUE mEv, mEvFilt, mNote, mVQ;

struct kq_per_thread {
	VALUE io;
	VALUE changelist;
	int fd;
	int nchanges;
	int nevents;
	int capa;
	struct timespec *ts;
	struct kevent events[FLEX_ARRAY];
};

static void tssub(struct timespec *a, struct timespec *b, struct timespec *res)
{
	res->tv_sec = a->tv_sec - b->tv_sec;
	res->tv_nsec = a->tv_nsec - b->tv_nsec;
	if (res->tv_nsec < 0) {
		res->tv_sec--;
		res->tv_nsec += NANO_PER_SEC;
	}
}

/* this will raise if the IO is closed */
static int kq_fd_check(struct kq_per_thread *kpt)
{
	int save_errno = errno;

	kpt->fd = rb_sp_fileno(kpt->io);
	errno = save_errno;

	return 1;
}

static struct kq_per_thread *kpt_get(int nchanges, int nevents)
{
	struct kq_per_thread *kpt;
	size_t size;
	int max = nchanges > nevents ? nchanges : nevents;

	/* error check here to prevent OOM from posix_memalign */
	if (max < 0) {
		errno = EINVAL;
		rb_sys_fail("kevent got negative events < 0");
	}

	size = sizeof(struct kq_per_thread) + sizeof(struct kevent) * max;
	kpt = rb_sp_gettlsbuf(&size);
	kpt->capa = max;
	kpt->nchanges = nchanges;
	kpt->nevents = nevents;

	return kpt;
}

/*
 * call-seq:
 *	SleepyPenguin::Kqueue::IO.new	-> Kqueue::IO object
 *
 * Creates a new Kqueue::IO object.  This is a wrapper around the kqueue(2)
 * system call which creates a Ruby IO object around the kqueue descriptor.
 *
 * kqueue descriptors are automatically invalidated by the OS across fork,
 * so care must be taken when forking.
 * Setting IO#autoclose=false is recommended for applications which fork
 * after kqueue creation.
 */
static VALUE s_new(VALUE klass)
{
	VALUE rv;
	int fd = kqueue();
	int flags;

	if (fd < 0) {
		/*
		 * ENOMEM/EMFILE/ENFILE are the only documented errors
		 * for kqueue(), hope GC can give us some space to retry:
		 */
		rb_gc();
		fd = kqueue();
		if (fd < 0)
			rb_sys_fail("kqueue");
	}

	flags = fcntl(fd, F_GETFD);
	if (flags != -1)
		fcntl(fd, F_SETFD, flags | FD_CLOEXEC);

	rv = INT2FIX(fd);

	return rb_call_super(1, &rv);
}

static void yield_kevent(struct kevent *event)
{
	VALUE ident = ULONG2NUM((unsigned long)event->ident); /* uintptr_t */
	VALUE filter = INT2NUM((int)event->filter); /* short */
	VALUE flags = UINT2NUM((unsigned)event->flags); /* u_short */
	VALUE fflags = UINT2NUM((unsigned)event->fflags); /* u_int */
	VALUE data = LONG2NUM((long)event->data); /* intptr_t */
	VALUE udata = (VALUE)event->udata; /* void * */

	rb_yield_values(6, ident, filter, flags, fflags, data, udata);
}

static VALUE kevent_result(struct kq_per_thread *kpt, int nevents)
{
	int i;
	struct kevent *event = kpt->events;

	if (nevents < 0) {
		if (errno == EINTR)
			nevents = 0;
		else
			rb_sys_fail("kevent");
	}

	for (i = nevents; --i >= 0; event++)
		yield_kevent(event);

	return INT2NUM(nevents);
}

/*
 * returns true if we were interrupted by a signal and resumable,
 * updating the timeout timespec with the remaining time if needed.
 */
static int
kevent_resume_p(struct timespec *expire_at, struct kq_per_thread *kpt)
{
	struct timespec now;

	kq_fd_check(kpt); /* may raise IOError */

	if (errno != EINTR)
		return 0;

	/*
	 * kevent is not interruptible until changes are sent,
	 * so if we got here, we already got our changes in
	 */
	kpt->nchanges = 0;

	/* we're waiting forever */
	if (kpt->ts == NULL)
		return 1;

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (now.tv_sec > expire_at->tv_sec)
		return 0;
	if (now.tv_sec == expire_at->tv_sec && now.tv_nsec > expire_at->tv_nsec)
		return 0;

	tssub(expire_at, &now, kpt->ts);
	return 1;
}

static VALUE nogvl_kevent(void *args)
{
	struct kq_per_thread *kpt = args;
	int nevents = kevent(kpt->fd, kpt->events, kpt->nchanges,
			 kpt->events, kpt->nevents, kpt->ts);

	return (VALUE)nevents;
}

static void changelist_prepare(struct kevent *, VALUE);

static VALUE do_kevent(struct kq_per_thread *kpt)
{
	long nevents;
	struct timespec expire_at;

	if (kpt->nchanges)
		changelist_prepare(kpt->events, kpt->changelist);

	if (kpt->ts) {
		clock_gettime(CLOCK_MONOTONIC, &expire_at);

		expire_at.tv_sec += kpt->ts->tv_sec;
		expire_at.tv_nsec += kpt->ts->tv_nsec;
		if (expire_at.tv_nsec > NANO_PER_SEC) {
			expire_at.tv_sec++;
			expire_at.tv_nsec -= NANO_PER_SEC;
		}
	}

	do {
		nevents = (long)rb_sp_fd_region(nogvl_kevent, kpt, kpt->fd);
	} while (nevents < 0 && kevent_resume_p(&expire_at, kpt));

	return kevent_result(kpt, (int)nevents);
}

#if defined(HAVE_RB_STRUCT_SIZE) && defined(RSTRUCT_GET)
static void ev_set_struct(struct kevent *ev, VALUE event)
{
	if (rb_struct_size(event) == INT2NUM(6)) {
		uintptr_t ident = (uintptr_t)NUM2ULONG(RSTRUCT_GET(event, 0));
		short filter = NUM2SHORT(RSTRUCT_GET(event, 1));
		unsigned short flags = NUM2USHORT(RSTRUCT_GET(event, 2));
		unsigned fflags = (unsigned)NUM2UINT(RSTRUCT_GET(event, 3));
		intptr_t data = (intptr_t)NUM2LONG(RSTRUCT_GET(event, 4));
		void *udata = (void *)RSTRUCT_GET(event, 5);

		EV_SET(ev, ident, filter, flags, fflags, data, udata);
	} else {
		rb_raise(rb_eTypeError, "unsupported struct in changelist");
	}
}
#elif RBX_STRUCT == 0 && defined(RSTRUCT_LEN) && defined(RSTRUCT_PTR)
/* legacy MRI */
static void ev_set_struct(struct kevent *ev, VALUE event)
{
	long len = RSTRUCT_LEN(*event);
	if (len == 6) {
		const VALUE *ptr = RSTRUCT_PTR(*event);
		uintptr_t ident = (uintptr_t)NUM2ULONG(ptr[0]);
		short filter = NUM2SHORT(ptr[1]);
		unsigned short flags = NUM2USHORT(ptr[2]);
		unsigned fflags = (unsigned)NUM2UINT(ptr[3]);
		intptr_t data = (intptr_t)NUM2LONG(ptr[4]);
		void *udata = (void *)ptr[5];

		EV_SET(event, ident, filter, flags, fflags, data, udata);
	} else {
		rb_raise(rb_eTypeError, "unsupported struct in changelist");
	}
}
#else
static void ev_set_struct(struct kevent *ev, VALUE event)
{
	rb_raise(rb_eTypeError, "unsupported struct in changelist");
}
#endif

static void ev_set_ary(struct kevent *ev, VALUE event)
{
	long len = RARRAY_LEN(event);
	const VALUE *ptr = RARRAY_CONST_PTR(event);

	if (len == 6) {
		uintptr_t ident = (uintptr_t)NUM2ULONG(ptr[0]);
		short filter = NUM2SHORT(ptr[1]);
		unsigned short flags = NUM2USHORT(ptr[2]);
		unsigned fflags = (unsigned)NUM2UINT(ptr[3]);
		intptr_t data = (intptr_t)NUM2LONG(ptr[4]);
		void *udata = (void *)ptr[5];

		EV_SET(ev, ident, filter, flags, fflags, data, udata);
		return;
	}
	rb_raise(rb_eTypeError,
		"changelist must be an array of 6-element arrays or structs");
}

/* sets ptr and len */
static void unpack_event(struct kevent *ev, VALUE event)
{
	switch (TYPE(event)) {
	case T_STRUCT:
		if (RBX_STRUCT) {
			event = rb_funcall(event, rb_intern("to_a"), 0, 0);
			/* fall-through to T_ARRAY */
		} else {
			ev_set_struct(ev, event);
			return;
		}
	case T_ARRAY:
		ev_set_ary(ev, event);
	default:
		rb_raise(rb_eTypeError, "unsupported type in changelist");
	}
}

static void ary2eventlist(struct kevent *events, VALUE changelist)
{
	const VALUE *chg = RARRAY_CONST_PTR(changelist);
	long i = RARRAY_LEN(changelist);

	for (; --i >= 0; chg++)
		unpack_event(events++, *chg);
}

/*
 * Convert an Ruby representation of the changelist to "struct kevent"
 */
static void changelist_prepare(struct kevent *events, VALUE changelist)
{
	switch (TYPE(changelist)) {
	case T_ARRAY:
		ary2eventlist(events, changelist);
		return;
	case T_STRUCT: /* single event */
		unpack_event(events, changelist);
		return;
	default:
		rb_bug("changelist_prepare not type filtered by sp_kevent");
	}
}

/*
 * call-seq:
 *	kq_io.kevent([changelist[, nevents[, timeout]]]) { |ident,filter,flags,fflags,data,udata| ... }
 *
 * This is a wrapper around the kevent(2) system call to change and/or
 * retrieve events from the underlying kqueue descriptor.
 *
 * +changelist+ may be nil, a single Kevent struct or an array of Kevent
 * structs.  If +changelist+ is nil, no changes will be made to the
 * underlying kqueue object.
 *
 * +nevents+ may be non-negative integer or nil.  If +nevents+ is zero or
 * nil, no events are retrieved.  If +nevents+ is positive, a block must
 * be passed to kevent for each event.
 *
 * +timeout+ is the numeric timeout in seconds to wait for +nevents+.
 * If nil and +nevents+ is positive, kevent will sleep forever.
 * +timeout+ may be in a floating point number if subsecond resolution
 * is required.  If +nevents+ is nil or zero and +timeout+ is not specified,
 * +timeout+ is implied to be zero.
 *
 * If event retrieval is desired, a block taking 6-elements (one for each
 * field of the kevent struct) must be passed.
 */
static VALUE sp_kevent(int argc, VALUE *argv, VALUE self)
{
	struct timespec ts, *t;
	VALUE changelist, events, timeout;
	struct kq_per_thread *kpt;
	int nchanges, nevents;

	rb_scan_args(argc, argv, "03", &changelist, &events, &timeout);

	switch (TYPE(changelist)) {
	case T_NIL: nchanges = 0; break;
	case T_STRUCT: nchanges = 1; break;
	case T_ARRAY: nchanges = RARRAY_LENINT(changelist); break;
	default:
		rb_raise(rb_eTypeError, "unhandled type for kevent changelist");
	}

	if (rb_block_given_p()) {
		if (NIL_P(events))
			rb_raise(rb_eArgError,
				"block given but nevents not specified");
		nevents = NUM2INT(events);
		if (nevents < 0)
			rb_raise(rb_eArgError, "nevents must be non-negative");
	} else {
		if (!NIL_P(events))
			rb_raise(rb_eArgError,
				"nevents specified but block not given");
		nevents = 0;
	}

	t = NIL_P(timeout) ? NULL : value2timespec(&ts, timeout);
	kpt = kpt_get(nchanges, nevents);
	kpt->ts = t;
	kpt->changelist = changelist;
	kpt->io = self;
	kpt->fd = rb_sp_fileno(kpt->io);

	return rb_ensure(do_kevent, (VALUE)kpt, rb_sp_puttlsbuf, (VALUE)kpt);
}

/* initialize constants in the SleepyPenguin::Ev namespace */
static void init_ev(VALUE mSleepyPenguin)
{
	/*
	 * Document-module: SleepyPenguin::Ev
	 *
	 * Constants in the SleepyPenguin::Ev namespace are for the +flags+
	 * field in Kevent structs.
	 */
	mEv = rb_define_module_under(mSleepyPenguin, "Ev");

	/* See EV_ADD in the kevent(2) man page */
	rb_define_const(mEv, "ADD", UINT2NUM(EV_ADD));

	/* See EV_ENABLE in the kevent(2) man page */
	rb_define_const(mEv, "ENABLE", UINT2NUM(EV_ENABLE));

	/* See EV_DISABLE in the kevent(2) man page */
	rb_define_const(mEv, "DISABLE", UINT2NUM(EV_DISABLE));

	/* See EV_DISPATCH in the kevent(2) man page */
	rb_define_const(mEv, "DISPATCH", UINT2NUM(EV_DISPATCH));

	/* See EV_DELETE in the kevent(2) man page */
	rb_define_const(mEv, "DELETE", UINT2NUM(EV_DELETE));

	/* See EV_RECEIPT in the kevent(2) man page */
	rb_define_const(mEv, "RECEIPT", UINT2NUM(EV_RECEIPT));

	/* See EV_ONESHOT in the kevent(2) man page */
	rb_define_const(mEv, "ONESHOT", UINT2NUM(EV_ONESHOT));

	/* See EV_CLEAR in the kevent(2) man page */
	rb_define_const(mEv, "CLEAR", UINT2NUM(EV_CLEAR));

	/* See EV_EOF in the kevent(2) man page */
	rb_define_const(mEv, "EOF", UINT2NUM(EV_EOF));

	/* This is a return value in the proc passed to kevent */
	rb_define_const(mEv, "ERROR", UINT2NUM(EV_ERROR));
}

/* initialize constants in the SleepyPenguin::EvFilt namespace */
static void init_evfilt(VALUE mSleepyPenguin)
{
	/*
	 * Document-module: SleepyPenguin::EvFilt
	 *
	 * Pre-defined system filters for Kqueue events.  Not all filters
	 * are supported on all platforms.  Consult the kevent(2) man page
	 * and source code for your operating system for more information.
	 */
	mEvFilt = rb_define_module_under(mSleepyPenguin, "EvFilt");

	/* See EVFILT_READ in the kevent(2) man page */
	rb_define_const(mEvFilt, "READ", INT2NUM(EVFILT_READ));

	/* See EVFILT_WRITE in the kevent(2) man page */
	rb_define_const(mEvFilt, "WRITE", INT2NUM(EVFILT_WRITE));

	/*
	 * See EVFILT_AIO in the kevent(2) man page, not supported by libkqueue
	 */
	rb_define_const(mEvFilt, "AIO", INT2NUM(EVFILT_AIO));

	/* See EVFILT_VNODE in the kevent(2) man page */
	rb_define_const(mEvFilt, "VNODE", INT2NUM(EVFILT_VNODE));

#ifdef EVFILT_PROC
	/* Monitor process IDs, not supported by libkqueue */
	rb_define_const(mEvFilt, "PROC", INT2NUM(EVFILT_PROC));
#endif

	/*
	 * Note: the use of EvFilt::SIGNAL is NOT supported in Ruby
	 * Ruby runtimes already manage all signal handling in the process,
	 * so attempting to manage them with a kqueue causes conflicts.
	 * We disable the Linux SignalFD interface for the same reason.
	 */
	rb_define_const(mEvFilt, "SIGNAL", INT2NUM(EVFILT_SIGNAL));

	/* See EVFILT_TIMER in the kevent(2) man page */
	rb_define_const(mEvFilt, "TIMER", INT2NUM(EVFILT_TIMER));

#ifdef EVFILT_NETDEV
	/* network devices, no longer supported */
	rb_define_const(mEvFilt, "NETDEV", INT2NUM(EVFILT_NETDEV));
#endif

#ifdef EVFILT_FS
	/*
	 * See EVFILT_FS in the kevent(2) man page,
	 * not supported by libkqueue
	 */
	rb_define_const(mEvFilt, "FS", INT2NUM(EVFILT_FS));
#endif

#ifdef EVFILT_LIO
	/* attached to lio requests, not supported by libkqueue */
	rb_define_const(mEvFilt, "LIO", INT2NUM(EVFILT_LIO));
#endif

	/* see EVFILT_USER in the kevent(2) man page */
	rb_define_const(mEvFilt, "USER", INT2NUM(EVFILT_USER));
}

/* initialize constants in the SleepyPenguin::Note namespace */
static void init_note(VALUE mSleepyPenguin)
{
	/*
	 * Document-module: SleepyPenguin::Note
	 *
	 * Data/hint flags/masks for EVFILT_USER and friends in Kqueue
	 * On input, the top two bits of fflags specifies how the lower
	 * twenty four bits should be applied to the stored value of fflags.
	 *
	 * On output, the top two bits will always be set to Note::FFNOP
	 * and the remaining twenty four bits will contain the stored
	 * fflags value.
	 */
	mNote = rb_define_module_under(mSleepyPenguin, "Note");

	/* ignore input fflags */
	rb_define_const(mNote, "FFNOP", UINT2NUM(NOTE_FFNOP));

	/* bitwise AND fflags */
	rb_define_const(mNote, "FFAND", UINT2NUM(NOTE_FFAND));

	/* bitwise OR fflags */
	rb_define_const(mNote, "FFOR", UINT2NUM(NOTE_FFOR));

	/* copy fflags */
	rb_define_const(mNote, "FFCOPY", UINT2NUM(NOTE_FFCOPY));

	/* control mask for fflags */
	rb_define_const(mNote, "FFCTRLMASK", UINT2NUM(NOTE_FFCTRLMASK));

	/* user-defined flag mask for fflags */
	rb_define_const(mNote, "FFLAGSMASK", UINT2NUM(NOTE_FFLAGSMASK));

	/* Cause the event to be triggered for output */
	rb_define_const(mNote, "TRIGGER", UINT2NUM(NOTE_TRIGGER));

#ifdef NOTE_LOWAT
	/*
	 * data/hint flags for EVFILT_{READ|WRITE}, shared with userspace
	 * Not supported by libkqueue in Linux
	 */
	rb_define_const(mNote, "LOWAT", UINT2NUM(NOTE_LOWAT));
#endif

#ifdef EVFILT_VNODE
	/* vnode was removed */
	rb_define_const(mNote, "DELETE", UINT2NUM(NOTE_DELETE));

	/* vnode data contents changed */
	rb_define_const(mNote, "WRITE", UINT2NUM(NOTE_WRITE));

	/* vnode size increased */
	rb_define_const(mNote, "EXTEND", UINT2NUM(NOTE_EXTEND));

	/* vnode attributes changes */
	rb_define_const(mNote, "ATTRIB", UINT2NUM(NOTE_ATTRIB));

	/* vnode link count changed */
	rb_define_const(mNote, "LINK", UINT2NUM(NOTE_LINK));

	/* vnode was renamed */
	rb_define_const(mNote, "RENAME", UINT2NUM(NOTE_RENAME));

#  ifdef NOTE_REVOKE
	/* vnode access was revoked, not supported on Linux */
	rb_define_const(mNote, "REVOKE", UINT2NUM(NOTE_REVOKE));
#  endif
#endif /* EVFILT_VNODE */

#ifdef EVFILT_PROC
	/* process exited */
	rb_define_const(mNote, "EXIT", UINT2NUM(NOTE_EXIT));

	/* process forked */
	rb_define_const(mNote, "FORK", UINT2NUM(NOTE_FORK));

	/* process exec'd */
	rb_define_const(mNote, "EXEC", UINT2NUM(NOTE_EXEC));

	/* mask for hint bits */
	rb_define_const(mNote, "PCTRLMASK", UINT2NUM(NOTE_PCTRLMASK));

	/* mask for pid */
	rb_define_const(mNote, "PDATAMASK", UINT2NUM(NOTE_PDATAMASK));

	/* follow across forks */
	rb_define_const(mNote, "TRACK", UINT2NUM(NOTE_TRACK));

	/* could not track child */
	rb_define_const(mNote, "TRACKERR", UINT2NUM(NOTE_TRACKERR));

	/* am a child process */
	rb_define_const(mNote, "CHILD", UINT2NUM(NOTE_CHILD));
#endif /* EVFILT_PROC */

#ifdef EVFILT_NETDEV
	/* link is up */
	rb_define_const(mNote, "LINKUP", UINT2NUM(NOTE_LINKUP));

	/* link is down */
	rb_define_const(mNote, "LINKDOWN", UINT2NUM(NOTE_LINKDOWN));

	/* link state is valid */
	rb_define_const(mNote, "LINKINV", UINT2NUM(NOTE_LINKINV));
#endif /* EVFILT_NETDEV */
}

static void init_vq(VALUE mSleepyPenguin)
{
#ifdef VQ_NOTRESP
	/*
	 * Document-module: SleepyPenguin::VQ
	 *
	 * Constants used by the EvFilt::FS filter in the Kqueue interfaces
	 */
	mVQ = rb_define_module_under(mSleepyPenguin, "VQ");

	/* server down */
	rb_define_const(mVQ, "NOTRESP", UINT2NUM(VQ_NOTRESP));

	/* server bad auth */
	rb_define_const(mVQ, "NEEDAUTH", UINT2NUM(VQ_NEEDAUTH));

	/* low on space */
	rb_define_const(mVQ, "LOWDISK", UINT2NUM(VQ_LOWDISK));

	/* new filesystem mounted */
	rb_define_const(mVQ, "MOUNT", UINT2NUM(VQ_MOUNT));

	/* filesystem unmounted */
	rb_define_const(mVQ, "UNMOUNT", UINT2NUM(VQ_UNMOUNT));

	/* filesystem dead, needs force unmount */
	rb_define_const(mVQ, "DEAD", UINT2NUM(VQ_DEAD));

	/* filesystem needs assistance from external program */
	rb_define_const(mVQ, "ASSIST", UINT2NUM(VQ_ASSIST));

	/* server lockd down */
	rb_define_const(mVQ, "NOTRESPLOCK", UINT2NUM(VQ_NOTRESPLOCK));
#endif /* VQ_NOTRESP */
}

void sleepy_penguin_init_kqueue(void)
{
	VALUE mSleepyPenguin, cKqueue, cKqueue_IO;

	mSleepyPenguin = rb_define_module("SleepyPenguin");
	init_ev(mSleepyPenguin);
	init_evfilt(mSleepyPenguin);
	init_note(mSleepyPenguin);
	init_vq(mSleepyPenguin);

	cKqueue = rb_define_class_under(mSleepyPenguin, "Kqueue", rb_cObject);

	/*
	 * Document-class: SleepyPenguin::Kqueue::IO
	 *
	 * Kqueue::IO is a low-level class.  It does not provide fork nor
	 * GC-safety, so Ruby IO objects added via kevent must be retained
	 * by the application until IO#close is called.
	 *
	 * Warning: this class is easy to misuse, be careful as failure
	 * to preserve references objects passed as Kevent#udata may lead
	 * to crashes in Ruby.  The high-level Kqueue class prevents these
	 * crashes (but may still return invalid objects).
	 */
	cKqueue_IO = rb_define_class_under(cKqueue, "IO", rb_cIO);
	rb_define_singleton_method(cKqueue_IO, "new", s_new, 0);

	rb_define_method(cKqueue_IO, "kevent", sp_kevent, -1);

	id_for_fd = rb_intern("for_fd");

	/*
         * the high-level interface is implemented in Ruby
         * see lib/sleepy_penguin/kevent.rb
         */
}
#endif /* HAVE_SYS_EVENT_H */
