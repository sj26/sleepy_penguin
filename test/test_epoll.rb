require_relative 'helper'
require 'fcntl'
require 'socket'
require 'thread'

class TestEpoll < Test::Unit::TestCase
  include SleepyPenguin

  def setup
    @rd, @wr = IO.pipe
    @ep = Epoll.new
  end

  def test_constants
    Epoll.constants.each do |const|
      next if const.to_sym == :IO
      nr = Epoll.const_get(const)
      assert nr <= 0xffffffff, "#{const}=#{nr}"
    end
  end

  def test_cross_thread
    tmp = []
    t0 = Time.now
    Thread.new { sleep 0.100; @ep.add(@wr, Epoll::OUT) }
    @ep.wait { |flags,obj| tmp << [ flags, obj ] }
    elapsed = Time.now - t0
    assert elapsed >= 0.100
    assert_equal [[Epoll::OUT, @wr]], tmp, tmp.inspect
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

  def test_dup_and_fork
    epdup = @ep.dup
    @ep.close
    assert ! epdup.closed?
    pid = fork do
      exit!(!epdup.closed? && @ep.closed?)
    end
    _, status = Process.waitpid2(pid)
    assert status.success?, status.inspect
  ensure
    epdup.close
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
    res.each { |(_,status)| assert status.success? }
  end

  def test_tcp_connect_nonblock_edge
    epflags = Epoll::OUT | Epoll::ET
    host = '127.0.0.1'
    srv = TCPServer.new(host, 0)
    port = srv.addr[1]
    addr = Socket.pack_sockaddr_in(port, host)
    sock = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM, 0)
    begin
      sock.connect_nonblock(addr)
      exc = nil
    rescue Errno::EINPROGRESS => exc
    end
    assert_kind_of Errno::EINPROGRESS, exc
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

  def test_edge_accept
    host = '127.0.0.1'
    srv = TCPServer.new(host, 0)
    port = srv.addr[1]
    sock = TCPSocket.new(host, port)
    asock = srv.accept
    assert_equal 3, asock.syswrite("HI\n")
    @ep.add(asock, Epoll::OUT| Epoll::ET | Epoll::ONESHOT)
    tmp = []
    @ep.wait(1) { |flags, obj| tmp << [ flags, obj ] }
    assert_equal [ [Epoll::OUT,  asock] ], tmp
  end

  def teardown
    @rd.close unless @rd.closed?
    @wr.close unless @wr.closed?
    @ep.close unless @ep.closed?
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

  def test_signal_safe_wait_forever
    sigpipe = IO.pipe
    time = {}
    thr = Thread.new do
      IO.select([sigpipe[0]]) # wait for USR1
      sigpipe[0].read(1)
      sleep 0.5
      @wr.syswrite '.'
    end
    trap(:USR1) do
      time[:USR1] = Time.now
      sigpipe[1].syswrite('.') # wake up thr
    end
    @ep.add @rd, Epoll::IN
    tmp = []
    pid = fork do
      sleep 0.5 # slightly racy :<
      Process.kill(:USR1, Process.ppid)
      exit!(0)
    end
    time[:START_WAIT] = Time.now
    @ep.wait do |flags, obj|
      tmp << [ flags, obj ]
      time[:EP] = Time.now
    end
    assert_equal([[Epoll::IN, @rd]], tmp)
    _, status = Process.waitpid2(pid)
    assert status.success?, status.inspect
    usr1_delay = time[:USR1] - time[:START_WAIT]
    assert_in_delta(0.5, usr1_delay, 0.1, "usr1_delay=#{usr1_delay}")
    ep_delay = time[:EP] - time[:USR1]
    assert_in_delta(0.5, ep_delay, 0.1, "ep1_delay=#{ep_delay}")
    assert_kind_of Thread, thr
    thr.join
    ensure
      sigpipe.each { |io| io.close }
      trap(:USR1, 'DEFAULT')
  end

  def test_close
    @ep.add @rd, Epoll::IN
    tmp = []
    thr = Thread.new { @ep.wait { |flags, obj| tmp << [ flags, obj ] } }
    @rd.close
    @wr.close
    Thread.pass
    assert_nil thr.join(0.25)
    assert thr.alive?
    thr.kill
    assert tmp.empty?
    thr.join
  end

  def test_rdhup
    defined?(Epoll::RDHUP) or
      return warn "skipping test, EPOLLRDHUP not available"
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
    @ep.add r, Epoll::IN
    @ep.add @rd, Epoll::IN
    @ep.add w, Epoll::OUT
    @ep.add @wr, Epoll::OUT
    tmp = []
    @ep.wait { |flags, obj| tmp << [ flags, obj ] }
    assert_equal 2, tmp.size
    assert_equal [ Epoll::OUT ], tmp.map { |flags, obj| flags }.uniq
    ios = tmp.map { |flags, obj| obj }
    assert ios.include?(@wr)
    assert ios.include?(w)
  end

  def test_clone
    tmp = []
    clone = @ep.clone
    assert @ep.to_io.fileno != clone.to_io.fileno
    clone.add @wr, Epoll::OUT
    @ep.wait(nil, 0) { |flags, obj| tmp << [ flags, obj ] }
    assert_equal([[Epoll::OUT, @wr]], tmp)
    clone.close
  end

  def test_dup
    tmp = []
    clone = @ep.dup
    assert @ep.to_io.fileno != clone.to_io.fileno
    clone.add @wr, Epoll::OUT
    @ep.wait(nil, 0) { |flags, obj| tmp << [ flags, obj ] }
    assert_equal([[Epoll::OUT, @wr]], tmp)
    clone.close
  end

  def test_set_idempotency
    @ep.set @rd, Epoll::IN
    @ep.set @rd, Epoll::IN
    @ep.set @wr, Epoll::OUT
    @ep.set @wr, Epoll::OUT
  end

  def test_wait_timeout
    t0 = Time.now
    assert_equal 0, @ep.wait(nil, 100) { |flags,obj| assert false }
    diff = Time.now - t0
    assert(diff >= 0.075, "#{diff} < 0.100s")
  end

  def test_del
    assert_raises(Errno::ENOENT) { @ep.del(@rd) }
    @ep.add(@rd, Epoll::IN)
    @ep.del(@rd)
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
    io.close

    # prettier, slower, but more memory efficient due to lack of caching
    # due to the constant cache:
    io = Epoll.new(:CLOEXEC).to_io

    assert((io.fcntl(Fcntl::F_GETFD) & Fcntl::FD_CLOEXEC) == Fcntl::FD_CLOEXEC)
  end

  def test_new
    @ep.close
    io = Epoll.new.to_io
    check_cloexec(io)
  end

  def test_delete
    assert_nil @ep.delete(@rd)
    assert_nil @ep.delete(@wr)
    @ep.add @rd, Epoll::IN
    assert_equal @rd, @ep.delete(@rd)
    assert_nil @ep.delete(@rd)
  end

  def test_io_for
    @ep.add @rd, Epoll::IN
    assert_equal @rd, @ep.io_for(@rd.fileno)
    assert_equal @rd, @ep.io_for(@rd)
    @ep.del @rd
    assert_nil @ep.io_for(@rd.fileno)
    assert_nil @ep.io_for(@rd)
  end

  def test_flags_for
    @ep.add @rd, Epoll::IN
    assert_equal Epoll::IN, @ep.flags_for(@rd.fileno)
    assert_equal Epoll::IN, @ep.flags_for(@rd)

    @ep.del @rd
    assert_nil @ep.flags_for(@rd.fileno)
    assert_nil @ep.flags_for(@rd)
  end

  def test_flags_for_sym
    @ep.add @rd, :IN
    assert_equal Epoll::IN, @ep.flags_for(@rd.fileno)
    assert_equal Epoll::IN, @ep.flags_for(@rd)

    @ep.del @rd
    assert_nil @ep.flags_for(@rd.fileno)
    assert_nil @ep.flags_for(@rd)
  end

  def test_flags_for_sym_ary
    @ep.add @rd, [:IN, :ET]
    expect = Epoll::IN | Epoll::ET
    assert_equal expect, @ep.flags_for(@rd.fileno)
    assert_equal expect, @ep.flags_for(@rd)

    @ep.del @rd
    assert_nil @ep.flags_for(@rd.fileno)
    assert_nil @ep.flags_for(@rd)
  end

  def test_include?
    assert ! @ep.include?(@rd)
    @ep.add @rd, Epoll::IN
    assert @ep.include?(@rd), @ep.instance_variable_get(:@marks).inspect
    assert @ep.include?(@rd.fileno)
    assert ! @ep.include?(@wr)
    assert ! @ep.include?(@wr.fileno)
  end

  def test_cross_thread_close
    tmp = []
    thr = Thread.new { sleep(1); @ep.close }
    assert_raises(IOError) do
      @ep.wait { |flags, obj| tmp << [ flags, obj ] }
    end
    assert_nil thr.value
  end if RUBY_VERSION == "1.9.3"

  def test_epoll_level_trigger
    @ep.add(@wr, Epoll::OUT)

    tmp = nil
    @ep.wait { |flags, obj| tmp = obj }
    assert_equal @wr, tmp

    tmp = nil
    @ep.wait { |flags, obj| tmp = obj }
    assert_equal @wr, tmp

    buf = '.' * 16384
    begin
      @wr.write_nonblock(buf)
    rescue Errno::EAGAIN
      break
    end while true
    @rd.read(16384)

    tmp = nil
    @ep.wait { |flags, obj| tmp = obj }
    assert_equal @wr, tmp
  end

  def test_epoll_wait_signal_torture
    usr1 = 0
    empty = 0
    nr = 100
    @ep.add(@rd, Epoll::IN)
    tmp = []
    trap(:USR1) { usr1 += 1 }
    pid = fork do
      trap(:USR1, "DEFAULT")
      sleep 0.1
      ppid = Process.ppid
      nr.times { Process.kill(:USR1, ppid); sleep 0.05 }
      @wr.syswrite('.')
      exit!(0)
    end
    while tmp.empty?
      @ep.wait(nil, 100) { |flags,obj| tmp << obj }
      empty += 1
    end
    _, status = Process.waitpid2(pid)
    assert status.success?, status.inspect
    assert usr1 > 0, "usr1: #{usr1}"
    ensure
      trap(:USR1, "DEFAULT")
  end if ENV["STRESS"].to_i != 0

  def test_wait_one_event_per_thread
    thr = []
    pipes = {}
    lock = Mutex.new
    maxevents = 1
    ok = []
    nr = 10
    nr.times do
      r, w = IO.pipe
      lock.synchronize { pipes[r] = w }
      @ep.add(r, Epoll::IN | Epoll::ET | Epoll::ONESHOT)

      t = Thread.new do
        sleep 2
        events = 0
        @ep.wait(maxevents) do |_,obj|
          lock.synchronize do
            assert pipes.include?(obj), "#{obj.inspect} is unknown"
            ok << obj
          end
          events += 1
        end
        events
      end
      thr << t
    end
    lock.synchronize do
      pipes.each_value { |w| w.syswrite '.' }
    end
    thr.each do |t|
      begin
        t.run
      rescue ThreadError
      end
    end

    thr.each { |t| assert_equal 1, t.value }
    assert_equal nr, ok.size, ok.inspect
    assert_equal ok.size, ok.uniq.size, ok.inspect
    assert_equal ok.map { |io| io.fileno }.sort,
                 pipes.keys.map { |io| io.fileno }.sort
  ensure
    pipes.each do |r,w|
      r.close
      w.close
    end
  end

  def test_epoll_as_queue
    fl = Epoll::OUT | Epoll::ET
    first = nil
    to_close = []
    500.times do
      r, w = ary = IO.pipe
      to_close.concat(ary)
      @ep.add(w, fl)
      first ||= begin
        @ep.add(r, Epoll::IN | Epoll::ET)
        [ r, w ]
      end
    end
    500.times do |i|
      @ep.wait(1) { |flags, io| first[1].write('.') if i == 0 }
    end
    @ep.wait(1) { |flags, io| assert_equal(first[0], io) }
    to_close.each(&:close)
  end

  def test_epoll_nest
    ep2 = Epoll.new
    r, w = IO.pipe
    @ep.add(@rd, :IN)
    @ep.add(@wr, :OUT)
    ep2.add(r, :IN)
    ep2.add(w, :OUT)
    w.write('.')
    @wr.write('.')
    outer = []
    inner = []
    nr = 0
    @ep.wait(2) do |_, io|
      outer << io
      ep2.wait(2) do |_, io2|
        (inner[nr] ||= []) << io2
      end
      nr += 1
    end
    assert_equal [ @rd, @wr ].sort_by(&:fileno), outer.sort_by(&:fileno)
    exp = [ r, w ].sort_by(&:fileno)
    assert_equal [ exp, exp ], inner.map { |x| x.sort_by(&:fileno) }
  ensure
    [ r, w, ep2 ].compact.each(&:close)
  end
end if defined?(SleepyPenguin::Epoll)
