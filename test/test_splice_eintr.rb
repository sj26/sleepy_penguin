# -*- encoding: binary -*-
require_relative 'helper'
require 'tempfile'
require 'socket'
require 'timeout'

class Test_Splice_EINTR < Test::Unit::TestCase
  def setup
    @usr1 = 0
    trap(:USR1) { @usr1 += 1 }
  end

  def teardown
    trap(:USR1, "DEFAULT")
  end

  def test_EINTR_splice_read
    rd, wr = IO.pipe
    tmp = Tempfile.new 'splice-read'
    main = Thread.current
    Thread.new do
      sleep 0.01
      Process.kill(:USR1, $$)
      sleep 0.01
      wr.write "HI"
    end
    nr = SleepyPenguin.splice rd, tmp, 666
    assert_equal 2, nr
    assert_equal 1, @usr1
  end
end if SleepyPenguin.respond_to?(:splice)
