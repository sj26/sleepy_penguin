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

#if ! HAVE_RB_IO_T
#  define rb_io_t OpenFile
#endif

#ifdef GetReadFile
#  define FPTR_TO_FD(fptr) (fileno(GetReadFile(fptr)))
#else
#  if !HAVE_RB_IO_T || (RUBY_VERSION_MAJOR == 1 && RUBY_VERSION_MINOR == 8)
#    define FPTR_TO_FD(fptr) fileno(fptr->f)
#  else
#    define FPTR_TO_FD(fptr) fptr->fd
#  endif
#endif

static int my_fileno(VALUE io)
{
	rb_io_t *fptr;

	switch (TYPE(io)) {
	case T_FIXNUM: return NUM2INT(io);
	case T_FILE:
		GetOpenFile(io, fptr);
		return FPTR_TO_FD(fptr);
	}
	io = rb_convert_type(io, T_FILE, "IO", "to_io");
	GetOpenFile(io, fptr);
	return FPTR_TO_FD(fptr);
}

#endif /* SLEEPY_PENGUIN_H */
