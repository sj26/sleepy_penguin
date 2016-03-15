# -*- encoding: binary -*-
require 'test/unit'
require 'tempfile'
require 'socket'
require 'sleepy_penguin'
require 'timeout'
$-w = true
Thread.abort_on_exception = true

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
    nr = SleepyPenguin.splice rd, nil, tmp, nil, 666
    assert_equal 2, nr
    assert_equal 1, @usr1
  end
end if defined?(RUBY_ENGINE)
