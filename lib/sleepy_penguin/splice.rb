# -*- encoding: binary -*-

module SleepyPenguin
  # call-seq:
  #    SleepyPenguin.splice(io_in, io_out, len[, flags [, keywords]) => Integer
  #
  # Splice +len+ bytes from/to a pipe.  Either +io_in+ or +io_out+
  # MUST be a pipe.  +io_in+ and +io_out+ may BOTH be pipes as of
  # Linux 2.6.31 or later.
  #
  # +flags+ defaults to zero if unspecified.
  # It may be an Integer bitmask, a Symbol, or Array of Symbols
  #
  # The integer bitmask may any combination of:
  #
  # * SleepyPenguin::F_MOVE - attempt to move pages instead of copying
  #
  # * SleepyPenguin::F_NONBLOCK - do not block on pipe I/O (only)
  #
  # * SleepyPenguin::F_MORE - indicates more data will be sent soon
  #
  # Symbols may be used as well to specify a single flag:
  #
  # * :move        - corresponds to F_MOVE
  # * :nonblock    - corresponds to F_NONBLOCK
  # * :more        - corresponds to F_MORE
  #
  # Or, an array of any combination of the above symbols.
  #
  # Keywords:
  #
  # :off_in and :off_out if non-nil may be used to
  #  specify an offset for the respective non-pipe file descriptor.
  #
  # :exception defaults to +true+.  Setting it to +false+
  # will return :EAGAIN symbol instead of raising Errno::EAGAIN.
  # This will also return +nil+ instead of raising EOFError
  # when +io_in+ is at the end.
  #
  # Raises EOFError when +io_in+ has reached end of file.
  # Raises Errno::EAGAIN if the SleepyPenguin::F_NONBLOCK flag is set
  # and the pipe has no data to read from or space to write to.  May
  # also raise Errno::EAGAIN if the non-pipe descriptor has no data
  # to read from or space to write to.
  #
  # As splice never exposes buffers to userspace, it will not take
  # into account userspace buffering done by Ruby or stdio.  It is
  # also not subject to encoding/decoding filters under Ruby 1.9+.
  #
  # Consider using `exception: false` if +io_out+ is a pipe or if you
  # are using non-blocking I/O on both descriptors as it avoids the
  # cost of raising common Errno::EAGAIN exceptions.
  #
  # See manpage for full documentation:
  # http://man7.org/linux/man-pages/man2/splice.2.html
  #
  # Support for this exists in sleepy_penguin 3.5.0+
  def self.splice(io_in, io_out, len, flags = 0,
                  off_in: nil, off_out: nil, exception: true)
    flags = __map_splice_flags(flags)
    ret = __splice(io_in, off_in, io_out, off_out, len, flags)
    exception ? __map_exc(ret) : ret
  end

  # call-seq:
  #   SleepyPenguin.tee(io_in, io_out, len[, flags[, keywords]) => Integer
  #
  # Copies up to +len+ bytes of data from +io_in+ to +io_out+.  +io_in+
  # and +io_out+ must both refer to pipe descriptors.  +io_in+ and +io_out+
  # may not be endpoints of the same pipe.
  #
  # +flags+ may be zero (the default) or a combination of:
  # * SleepyPenguin::F_NONBLOCK
  #
  # As a shortcut, the `:nonblock` symbol may be used instead.
  #
  # Other splice-related flags are currently unimplemented in the
  # kernel and have no effect.
  #
  # Returns the number of bytes duplicated if successful.
  # Raises EOFError when +io_in+ is closed and emptied.
  # Raises Errno::EAGAIN when +io_in+ is empty and/or +io_out+ is full
  # and +flags+ specifies non-blocking operation
  #
  # Keywords:
  #
  # :exception defaults to +true+.  Setting it to +false+
  # will return :EAGAIN symbol instead of raising Errno::EAGAIN.
  # This will also return +nil+ instead of raising EOFError
  # when +io_in+ is at the end.
  #
  # Consider using `exception: false` if +io_out+ is a pipe or if you
  # are using non-blocking I/O on both descriptors as it avoids the
  # cost of raising common Errno::EAGAIN exceptions.
  #
  # See manpage for full documentation:
  # http://man7.org/linux/man-pages/man2/tee.2.html
  #
  # Support for this exists in sleepy_penguin 3.5.0+
  def self.tee(io_in, io_out, len, flags = 0, exception: true)
    flags = __map_splice_flags(flags)
    ret = __tee(io_in, io_out, len, flags)
    exception ? __map_exc(ret) : ret
  end if respond_to?(:__tee)

  @__splice_f_map = { # :nodoc:
    :nonblock => F_NONBLOCK,
    :more => F_MORE,
    :move => F_MOVE
  }

  def self.__map_splice_flags(flags) # :nodoc:
    onef = @__splice_f_map[flags] and return onef
    flags.respond_to?(:inject) ?
        flags.inject(0) { |fl, sym| fl |= @__splice_f_map[sym] } : flags
  end

  def self.__map_exc(ret) # :nodoc:
    case ret
    when :EAGAIN then raise Errno::EAGAIN, 'Resource temporarily unavailable'
    when nil then raise EOFError, 'end of file reached'
    end
    ret
  end
end
