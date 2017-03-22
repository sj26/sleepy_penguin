module SleepyPenguin

  # call-seq:
  #    SleepyPenguin.copy_file_range(src, dst, len[, keywords]) => # Integer
  #
  # Performs and in-kernel copy of +len+ bytes from +src+ to +dst+,
  # where +src+ and +dst+ are regular files on the same filesystem.
  # Returns the number of bytes copied, which may be less than
  # requested.
  #
  # +flags+ is currently unused, but may be specified in the future.
  #
  # Keywords:
  #
  # :off_in and :off_out if non-nil may be used to specify an Integer
  # offset for each respective descriptor.  If specified, the file
  # offsets of each file description will not be moved, providing
  # pread(2)/pwrite(2)-like semantics.
  #
  # See copy_file_range(2) manpage for full documentation:
  # http://man7.org/linux/man-pages/man2/copy_file_range.2.html
  #
  # This method only works in Linux 4.5+ with sleepy_penguin 3.5.0+,
  # and may require up-to-date kernel headers for non-x86/x86-64 systems.
  def self.copy_file_range(io_in, io_out, len, flags = 0,
                           off_in: nil, off_out: nil)
    __cfr(io_in, off_in, io_out, off_out, len, flags)
  end
end
