require_relative 'helper'

class TestKqueue < Test::Unit::TestCase
  include SleepyPenguin

  def test_kqueue
    kq = Kqueue.new
    assert_kind_of IO, kq.to_io
    assert_predicate kq.to_io, :close_on_exec?
    rd, wr = IO.pipe
    ev = Kevent[rd.fileno, EvFilt::READ, Ev::ADD|Ev::ONESHOT, 0, 0, rd]
    thr = Thread.new do
      kq.kevent(ev)
      wr.syswrite "."
    end

    events = []
    n = kq.kevent(nil, 1) do |kevent|
      assert_kind_of Kevent, kevent
      events << kevent
    end
    assert_equal 1, events.size
    assert_equal rd.fileno, events[0][0]
    assert_equal EvFilt::READ, events[0][1]
    assert_equal 1, n

    # we should be drained
    events = []
    n = kq.kevent(nil, 1, 0) do |kevent|
      assert_kind_of Kevent, kevent
      events << kevent
    end
    assert_equal 0, events.size
    assert_equal 0, n
    thr.join

    # synchronous add
    events = []
    ev = Kevent[wr.fileno, EvFilt::WRITE, Ev::ADD|Ev::ONESHOT, 0, 0, wr]
    kq.kevent(ev)
    n = kq.kevent(nil, 1, 0) do |kevent|
      assert_kind_of Kevent, kevent
      events << kevent
    end
    assert_equal 1, events.size
    assert_equal wr.fileno, events[0][0]
    assert_equal EvFilt::WRITE, events[0][1]
    assert_equal 1, n
  ensure
    kq.close
    rd.close if rd
    wr.close if wr
  end

  def test_usable_after_fork
    kq = Kqueue.new
    pid = fork do
      begin
        ok = false
        assert_equal(0, kq.kevent(nil, 1, 0.1) { exit!(false) })
        ok = true
      ensure
        exit!(ok)
      end
    end
    assert_equal(0, kq.kevent(nil, 1, 0.1) { exit!(false) })
    _, status = Process.waitpid2(pid)
    assert status.success?, status.inspect
  ensure
    kq.close
  end

  def test_epoll_nest
    kq1 = Kqueue.new
    kq2 = Kqueue.new
    r1, w1 = IO.pipe
    r2, w2 = IO.pipe
    w1.write '.'
    w2.write '.'
    kq1.kevent([
       Kevent[r1.fileno, EvFilt::READ, Ev::ADD, 0, 0, r1],
       Kevent[w1.fileno, EvFilt::WRITE, Ev::ADD, 0, 0, w1]
    ])
    kq2.kevent([
       Kevent[r2.fileno, EvFilt::READ, Ev::ADD, 0, 0, r2],
       Kevent[w2.fileno, EvFilt::WRITE, Ev::ADD, 0, 0, w2]
    ])
    outer = []
    inner = []
    nr = 0
    kq1.kevent(nil, 2) do |kev1|
      outer << kev1.udata
      kq2.kevent(nil, 2) do |kev2|
        (inner[nr] ||= []) << kev2.udata
      end
      nr += 1
    end
    assert_equal [ r1, w1 ].sort_by(&:fileno), outer.sort_by(&:fileno)
    exp = [ r2, w2 ].sort_by(&:fileno)
    assert_equal [ exp, exp ], inner.map { |x| x.sort_by(&:fileno) }
  ensure
    [ r1, w1, r2, w2, kq1, kq2 ].compact.each(&:close)
  end
end if defined?(SleepyPenguin::Kqueue)
