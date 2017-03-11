# -*- encoding: binary -*-
require 'test/unit'
require 'tempfile'
require 'socket'
$-w = true
require 'sleepy_penguin'

class TestSendfile < Test::Unit::TestCase
  def test_linux_sendfile
    rd, wr = UNIXSocket.pair
    size = 5
    src = Tempfile.new('ruby_sf_src')
    assert_equal 0, SleepyPenguin.linux_sendfile(wr, src, size)
    str = 'abcde'.freeze
    assert_equal str.bytesize, src.syswrite(str)
    assert_equal 0, SleepyPenguin.linux_sendfile(wr, src, size)
    src.sysseek(0, IO::SEEK_SET)
    assert_equal str.bytesize,
                 SleepyPenguin.linux_sendfile(wr, src, size, offset: 0)
    assert_equal str, rd.read(size)
    assert_equal 0, src.sysseek(0, IO::SEEK_CUR), 'handle offset not changed'
    assert_equal 3, SleepyPenguin.linux_sendfile(wr, src, 3)
    assert_equal 3, src.sysseek(0, IO::SEEK_CUR), 'handle offset changed'
  ensure
    [ rd, wr ].compact.each(&:close)
    src.close! if src
  end
end
