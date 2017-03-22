# -*- encoding: binary -*-
require_relative 'helper'
require 'fcntl'

class TestPipesize < Test::Unit::TestCase
  def test_pipe_size
    return unless RUBY_PLATFORM =~ /linux/
    [ :F_GETPIPE_SZ, :F_SETPIPE_SZ ].each do |c|
      return unless SleepyPenguin.const_defined?(c)
    end
    r, w = pipe = IO.pipe
    nr = r.fcntl(SleepyPenguin::F_GETPIPE_SZ)
    assert_kind_of Integer, nr
    assert_operator nr, :>, 0

    set = 131072
    r.fcntl(SleepyPenguin::F_SETPIPE_SZ, set)
    assert_equal set, r.fcntl(SleepyPenguin::F_GETPIPE_SZ)
  ensure
    pipe.each(&:close) if pipe
  end
end
