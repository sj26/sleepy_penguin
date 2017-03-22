# -*- encoding: binary -*-
require 'sleepy_penguin_ext'

# We need to serialize Inotify#take for Rubinius since that has no GVL
# to protect the internal array
if defined?(SleepyPenguin::Inotify) &&
   defined?(Rubinius) && Rubinius.respond_to?(:synchronize)
  class SleepyPenguin::Inotify
    # :stopdoc:
    alias __take take
    undef_method :take
    def take(*args)
      Rubinius.synchronize(@inotify_tmp) { __take(*args) }
    end
    # :startdoc
  end
end

module SleepyPenguin
  require_relative 'sleepy_penguin/splice' if respond_to?(:__splice)
  require_relative 'sleepy_penguin/cfr' if respond_to?(:__cfr)
  require_relative 'sleepy_penguin/epoll' if const_defined?(:Epoll)
  require_relative 'sleepy_penguin/kqueue' if const_defined?(:Kqueue)

  # Copies +len+ bytes from +src+ to +dst+, where +src+ refers to
  # an open, mmap(2)-able File and +dst+ refers to a Socket.
  # An optional +offset+ keyword may be specified for the +src+ File.
  # Using +offset+ will not adjust the offset of the underlying file
  # handle itself; in other words: this allows concurrent threads to
  # use linux_sendfile to write data from one open file to multiple
  # sockets.
  #
  # Returns the number of bytes written on success, or :wait_writable
  # if the +dst+ Socket is non-blocking and the operation would block.
  # A return value of zero bytes indicates EOF is reached on the +src+
  # file.
  #
  # Newer OSes may be more flexible in whether or not +dst+ or +src+
  # is a regular file or socket, respectively.
  #
  # This method was added in sleepy_penguin 3.5.0.
  def self.linux_sendfile(dst, src, len, offset: nil)
    __lsf(dst, src, offset, len)
  end
end
