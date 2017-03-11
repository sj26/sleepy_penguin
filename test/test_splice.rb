# -*- encoding: binary -*-
require 'test/unit'
require 'tempfile'
require 'socket'
require 'io/nonblock'
require 'timeout'
$-w = true
require 'sleepy_penguin'

class TestSplice < Test::Unit::TestCase

  def test_splice
    str = 'abcde'
    size = 5
    rd, wr = IO.pipe
    tmp = Tempfile.new('ruby_splice')

    assert_equal 5, tmp.syswrite(str)
    tmp.sysseek(0)

    nr = SleepyPenguin.splice(tmp.fileno, wr.fileno, size)
    assert_equal size, nr
    assert_equal str, rd.sysread(size)
  end

  def test_splice_io
    str = 'abcde'
    size = 5
    rd, wr = IO.pipe
    tmp = Tempfile.new('ruby_splice')

    assert_equal 5, tmp.syswrite(str)
    tmp.sysseek(0)

    nr = SleepyPenguin.splice(tmp, wr, size)
    assert_equal size, nr
    assert_equal str, rd.sysread(size)
  end

  def test_splice_io_noflags
    str = 'abcde'
    size = 5
    rd, wr = IO.pipe
    tmp = Tempfile.new('ruby_splice')

    assert_equal 5, tmp.syswrite(str)
    tmp.sysseek(0)

    nr = SleepyPenguin.splice(tmp, wr, size)
    assert_equal size, nr
    assert_equal str, rd.sysread(size)
  end

  def test_trysplice_io_noflags
    str = 'abcde'
    size = 5
    rd, wr = IO.pipe
    tmp = Tempfile.new('ruby_splice')

    assert_equal 5, tmp.syswrite(str)
    tmp.sysseek(0)

    nr = SleepyPenguin.splice(tmp, wr, size, :nonblock, exception: false)
    assert_equal size, nr
    assert_equal str, rd.sysread(size)
  end

  def test_splice_io_ish
    str = 'abcde'
    size = 5
    rd, wr = IO.pipe
    tmp = Tempfile.new('ruby_splice')
    io_ish = [ tmp ]
    def io_ish.to_io
      first.to_io
    end

    assert_equal 5, tmp.syswrite(str)
    tmp.sysseek(0)

    nr = SleepyPenguin.splice(io_ish, wr, size)
    assert_equal size, nr
    assert_equal str, rd.sysread(size)
  end

  def test_splice_in_offset
    str = 'abcde'
    off = 3
    len = 2
    rd, wr = IO.pipe
    tmp = Tempfile.new('ruby_splice')

    assert_equal 5, tmp.syswrite(str)
    tmp.sysseek(0)

    nr = SleepyPenguin.splice(tmp.fileno, wr.fileno, len, off_in: off)
    assert_equal len, nr
    assert_equal 'de', rd.sysread(len)
  end

  def test_splice_out_offset
    str = 'abcde'
    rd, wr = IO.pipe
    tmp = Tempfile.new('ruby_splice')

    assert_equal 5, wr.syswrite(str)
    nr = SleepyPenguin.splice(rd.fileno, tmp.fileno, str.size, off_out: 3)
    assert_equal 5, nr
    tmp.sysseek(0)
    assert_equal "\0\0\0abcde", tmp.sysread(9)
  end

  def test_splice_nonblock
    rd, wr = IO.pipe
    tmp = Tempfile.new('ruby_splice')

    assert_raises(Errno::EAGAIN) {
      SleepyPenguin.splice(rd.fileno, tmp.fileno, 5, :nonblock, off_out: 0)
    }
  end

  def test_trysplice_nonblock
    rd, wr = IO.pipe
    tmp = Tempfile.new('ruby_splice')
    assert_equal :EAGAIN, SleepyPenguin.splice(rd, tmp, 5, :nonblock,
                                               off_out: 0, exception: false)
  end

  def test_trysplice_nonblock_noargs
    rd, wr = IO.pipe
    tmp = Tempfile.new('ruby_splice')
    assert_equal :EAGAIN, SleepyPenguin.splice(rd, tmp, 5, :nonblock,
                                               off_out: 0, exception: false)
    assert_equal :EAGAIN, SleepyPenguin.splice(rd, tmp, 5, [:more,:nonblock],
                                               off_out: 0,
                                               exception: false)
  end

  def test_splice_eof
    rd, wr = IO.pipe
    tmp = Tempfile.new('ruby_splice')
    wr.syswrite 'abc'
    wr.close

    nr = SleepyPenguin.splice(rd.fileno, tmp.fileno, 5, :nonblock, off_out: 0)
    assert_equal 3, nr
    assert_raises(EOFError) {
      SleepyPenguin.splice(rd.fileno, tmp.fileno, 5, :nonblock, off_out: 0)
    }
  end

  def test_trysplice_eof
    rd, wr = IO.pipe
    tmp = Tempfile.new('ruby_splice')
    wr.syswrite 'abc'
    wr.close

    nr = SleepyPenguin.splice(rd, tmp, 5, off_out: 0, exception: false)
    assert_equal 3, nr
    assert_nil SleepyPenguin.splice(rd, tmp, 5, :nonblock,
                                    off_out: 0, exception: false)
  end

  def test_splice_nonblock_socket
    server = TCPServer.new('127.0.0.1', 0)
    port = server.addr[1]
    rp, wp = IO.pipe
    rs = TCPSocket.new('127.0.0.1', port)
    rs.nonblock = true
    assert_raises(Errno::EAGAIN) {
      SleepyPenguin.splice(rs, wp, 1024)
    }
    rs.close
    server.close
  end

  def test_tee
    str = 'abcde'
    size = 5
    rda, wra = IO.pipe
    rdb, wrb = IO.pipe

    assert_equal 5, wra.syswrite(str)
    nr = SleepyPenguin.tee(rda.fileno, wrb.fileno, size)
    assert_equal 5, nr
    assert_equal str, rdb.sysread(5)
    assert_equal str, rda.sysread(5)
  end

  def test_trytee
    str = 'abcde'
    size = 5
    rda, wra = IO.pipe
    rdb, wrb = IO.pipe

    assert_equal 5, wra.syswrite(str)
    nr = SleepyPenguin.tee(rda, wrb, size, :nonblock, exception: false)
    assert_equal 5, nr
    assert_equal str, rdb.sysread(5)
    assert_equal str, rda.sysread(5)
  end

  def test_tee_eof
    rda, wra = IO.pipe
    rdb, wrb = IO.pipe
    wra.close
    assert_raises(EOFError) {
      SleepyPenguin.tee(rda.fileno, wrb.fileno, 4096)
    }
  end

  def test_trytee_eof
    rda, wra = IO.pipe
    rdb, wrb = IO.pipe
    wra.close
    assert_nil SleepyPenguin.tee(rda, wrb, 4096, :nonblock, exception: false)
  end

  def test_tee_nonblock
    rda, wra = IO.pipe
    rdb, wrb = IO.pipe
    assert_raises(Errno::EAGAIN) {
      SleepyPenguin.tee(rda.fileno, wrb.fileno, 4096, SleepyPenguin::F_NONBLOCK)
    }
  end

  def test_trytee_nonblock
    rda, wra = IO.pipe
    rdb, wrb = IO.pipe
    assert_equal :EAGAIN, SleepyPenguin.tee(rda, wrb, 4096, :nonblock,
                                            exception: false)
  end

  def test_tee_io
    str = 'abcde'
    size = 5
    rda, wra = IO.pipe
    rdb, wrb = IO.pipe

    assert_equal 5, wra.syswrite(str)
    nr = SleepyPenguin.tee(rda, wrb, size)
    assert_equal 5, nr
    assert_equal str, rdb.sysread(5)
    assert_equal str, rda.sysread(5)
  end

  def test_constants
    %w(move nonblock more).each { |x|
      assert Integer === SleepyPenguin.const_get("F_#{x.upcase}")
    }
  end
end if SleepyPenguin.respond_to?(:splice)
