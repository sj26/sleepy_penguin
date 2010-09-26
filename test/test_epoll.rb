require 'test/unit'
require 'fcntl'
require 'socket'
$-w = true

require 'sleepy_penguin'

class TestEpoll < Test::Unit::TestCase
  include SleepyPenguin

  def setup
    @rd, @wr = IO.pipe
    @ep = Epoll.new
  end

  def test_fork_safe
    tmp = []
    @ep.add @rd, Epoll::IN
    pid = fork do
      @ep.wait(nil, 100) { |flags,obj| tmp << [ flags, obj ] }
      exit!(tmp.empty?)
    end
    @wr.syswrite "HI"
    _, status = Process.waitpid2(pid)
    assert status.success?
    @ep.wait(nil, 0) { |flags,obj| tmp << [ flags, obj ] }
    assert_equal [[Epoll::IN, @rd]], tmp
  end

  def test_after_fork_usability
    fork { @ep.add(@rd, Epoll::IN); exit!(0) }
    fork { @ep.set(@rd, Epoll::IN); exit!(0) }
    fork { @ep.to_io; exit!(0) }
    fork { @ep.dup; exit!(0) }
    fork { @ep.clone; exit!(0) }
    fork { @ep.close; exit!(0) }
    fork { @ep.closed?; exit!(0) }
    fork {
      begin
        @ep.del(@rd)
      rescue Errno::ENOENT
        exit!(0)
      end
      exit!(1)
    }
    res = Process.waitall
    res.each { |(pid,status)| assert status.success? }
  end

  def test_tcp_connect_nonblock_edge
    epflags = Epoll::OUT | Epoll::ET
    host = '127.0.0.1'
    srv = TCPServer.new(host, 0)
    port = srv.addr[1]
    addr = Socket.pack_sockaddr_in(port, host)
    sock = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM, 0)
    assert_raises(Errno::EINPROGRESS) { sock.connect_nonblock(addr) }
    IO.select(nil, [ sock ], [sock ])
    @ep.add(sock, epflags)
    tmp = []
    @ep.wait(1) { |flags, obj| tmp << [ flags, obj ] }
    assert_equal [ [Epoll::OUT,  sock] ], tmp
  end

  def test_tcp_connect_edge
    epflags = Epoll::OUT | Epoll::ET
    host = '127.0.0.1'
    srv = TCPServer.new(host, 0)
    port = srv.addr[1]
    sock = TCPSocket.new(host, port)
    @ep.add(sock, epflags)
    tmp = []
    @ep.wait(1) { |flags, obj| tmp << [ flags, obj ] }
    assert_equal [ [Epoll::OUT,  sock] ], tmp
  end

  def teardown
    assert_nothing_raised do
      @rd.close unless @rd.closed?
      @wr.close unless @wr.closed?
      @ep.close unless @ep.closed?
    end
  end

  def test_max_events_big
    @ep.add @rd, Epoll::IN
    tmp = []
    thr = Thread.new { @ep.wait(1024) { |flags, obj| tmp << [ flags, obj ] } }
    Thread.pass
    assert tmp.empty?
    @wr.write '.'
    thr.join
    assert_equal([[Epoll::IN, @rd]], tmp)
    tmp.clear
    thr = Thread.new { @ep.wait { |flags, obj| tmp << [ flags, obj ] } }
    thr.join
    assert_equal([[Epoll::IN, @rd]], tmp)
  end

  def test_max_events_small
    @ep.add @rd, Epoll::IN | Epoll::ET
    @ep.add @wr, Epoll::OUT | Epoll::ET
    @wr.write '.'
    tmp = []
    @ep.wait(1) { |flags, obj| tmp << [ flags, obj ] }
    assert_equal 1, tmp.size
    @ep.wait(1) { |flags, obj| tmp << [ flags, obj ] }
    assert_equal 2, tmp.size
  end

  def test_signal_safe
    time = {}
    trap(:USR1) { time[:USR1] = Time.now; sleep 0.1; @wr.write '.' }
    @ep.add @rd, Epoll::IN
    tmp = []
    pid = fork do
      sleep 0.1 # slightly racy :<
      Process.kill(:USR1, Process.ppid)
    end
    time[:START_WAIT] = Time.now
    @ep.wait { |flags, obj| tmp << [ flags, obj ]; time[:EP] = Time.now }
    assert_equal([[Epoll::IN, @rd]], tmp)
    _, status = Process.waitpid2(pid)
    assert status.success?
    assert((time[:USR1] - time[:START_WAIT]) >= 0.1)
    assert((time[:USR1] - time[:START_WAIT]) < 0.15)
    assert((time[:EP] - time[:USR1]) >= 0.1)
    assert((time[:EP] - time[:USR1]) < 0.15)
    ensure
      trap(:USR1, 'DEFAULT')
  end

  def test_close
    @ep.add @rd, Epoll::IN
    tmp = []
    thr = Thread.new { @ep.wait { |flags, obj| tmp << [ flags, obj ] } }
    @rd.close
    @wr.close
    assert_nil thr.join(0.01)
    assert thr.alive?
    thr.kill
    assert tmp.empty?
  end

  def test_rdhup
    rd, wr = UNIXSocket.pair
    @ep.add rd, Epoll::RDHUP
    tmp = []
    thr = Thread.new { @ep.wait { |flags, obj| tmp << [ flags, obj ] } }
    wr.shutdown Socket::SHUT_WR
    thr.join
    assert_equal([[ Epoll::RDHUP, rd ]], tmp)
  end

  def test_hup
    @ep.add @rd, Epoll::IN
    tmp = []
    thr = Thread.new { @ep.wait { |flags, obj| tmp << [ flags, obj ] } }
    @wr.close
    thr.join
    assert_equal([[ Epoll::HUP, @rd ]], tmp)
  end

  def test_multiple
    r, w = IO.pipe
    assert_nothing_raised do
      @ep.add r, Epoll::IN
      @ep.add @rd, Epoll::IN
      @ep.add w, Epoll::OUT
      @ep.add @wr, Epoll::OUT
    end
    tmp = []
    @ep.wait { |flags, obj| tmp << [ flags, obj ] }
    assert_equal 2, tmp.size
    assert_equal [ Epoll::OUT ], tmp.map { |flags, obj| flags }.uniq
    ios = tmp.map { |flags, obj| obj }
    assert ios.include?(@wr)
    assert ios.include?(w)
  end

  def test_gc
    assert_nothing_raised { 4096.times { Epoll.new } }
    assert ! @ep.closed?
  end

  def test_gc_to_io
    assert_nothing_raised do
      4096.times do
        ep = Epoll.new
        io = ep.to_io
      end
    end
    assert ! @ep.closed?
  end

  def test_clone
    tmp = []
    clone = @ep.clone
    assert @ep.to_io.fileno != clone.to_io.fileno
    clone.add @wr, Epoll::OUT
    @ep.wait(nil, 0) { |flags, obj| tmp << [ flags, obj ] }
    assert_equal([[Epoll::OUT, @wr]], tmp)
  end

  def test_dup
    tmp = []
    clone = @ep.dup
    assert @ep.to_io.fileno != clone.to_io.fileno
    clone.add @wr, Epoll::OUT
    @ep.wait(nil, 0) { |flags, obj| tmp << [ flags, obj ] }
    assert_equal([[Epoll::OUT, @wr]], tmp)
  end

  def test_set_idempotency
    assert_nothing_raised do
      @ep.set @rd, Epoll::IN
      @ep.set @rd, Epoll::IN
      @ep.set @wr, Epoll::OUT
      @ep.set @wr, Epoll::OUT
    end
  end

  def test_wait_timeout
    t0 = Time.now
    assert_equal 0, @ep.wait(nil, 100) { |flags,obj| assert false }
    diff = Time.now - t0
    assert(diff >= 0.075, "#{diff} < 0.100s")
  end

  def test_del
    assert_raises(Errno::ENOENT) { @ep.del(@rd) }
    assert_nothing_raised do
      @ep.add(@rd, Epoll::IN)
      @ep.del(@rd)
    end
  end

  def test_wait_read
    @ep.add(@rd, Epoll::IN)
    assert_equal 0, @ep.wait(nil, 0) { |flags,obj| assert false }
    @wr.syswrite '.'
    i = 0
    nr = @ep.wait(nil, 0) do |flags,obj|
      assert_equal Epoll::IN, flags
      assert_equal obj, @rd
      i += 1
    end
    assert_equal 1, i
    assert_equal 1, nr
  end

  def test_wait_write
    @ep.add(@wr, Epoll::OUT | Epoll::IN)
    i = 0
    nr = @ep.wait(nil, 0) do |flags, obj|
      assert_equal Epoll::OUT, flags
      assert_equal obj, @wr
      i += 1
    end
    assert_equal 1, nr
    assert_equal 1, i
  end

  def test_wait_write_blocked
    begin
      @wr.write_nonblock('.' * 65536)
    rescue Errno::EAGAIN
      break
    end while true
    @ep.add(@wr, Epoll::OUT | Epoll::IN)
    i = 0
    assert_equal 0, @ep.wait(nil, 0) { |flags,event| assert false }
  end

  def test_selectable
    tmp = nil
    @ep.add @rd, Epoll::IN
    thr = Thread.new { tmp = IO.select([ @ep ]) }
    thr.join 0.01
    assert_nil tmp
    @wr.write '.'
    thr.join
    assert_equal([[@ep],[],[]], tmp)
  end

  def test_new_no_cloexec
    @ep.close
    io = Epoll.new(0).to_io
    assert((io.fcntl(Fcntl::F_GETFD) & Fcntl::FD_CLOEXEC) == 0)
  end

  def test_new_cloexec
    @ep.close
    io = Epoll.new(Epoll::CLOEXEC).to_io
    assert((io.fcntl(Fcntl::F_GETFD) & Fcntl::FD_CLOEXEC) == Fcntl::FD_CLOEXEC)
  end

  def test_new
    @ep.close
    io = Epoll.new.to_io
    assert((io.fcntl(Fcntl::F_GETFD) & Fcntl::FD_CLOEXEC) == Fcntl::FD_CLOEXEC)
  end
end