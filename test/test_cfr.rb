# -*- encoding: binary -*-
require_relative 'helper'
require 'tempfile'

class TestCfr < Test::Unit::TestCase
  def test_copy_file_range
    str = 'abcde'
    size = 5
    src = Tempfile.new('ruby_cfr_src')
    dst = Tempfile.new('ruby_cfr_dst')
    assert_equal 5, src.syswrite(str)
    src.sysseek(0)
    begin
      nr = SleepyPenguin.copy_file_range(src, dst, size)
    rescue Errno::EINVAL
      warn 'copy_file_range not supported (requires Linux 4.5+)'
      warn "We have: #{`uname -a`}"
      return
    end
    assert_equal nr, 5
    dst.sysseek(0)
    assert_equal str, dst.sysread(5)

    nr = SleepyPenguin.copy_file_range(src, dst, size, off_in: 1, off_out: 0)
    assert_equal 4, nr
    dst.sysseek(0)
    assert_equal 'bcde', dst.sysread(4)

    nr = SleepyPenguin.copy_file_range(src, dst, size, off_in: 9)
    assert_equal 0, nr, 'no EOFError'
  ensure
    dst.close!
    src.close!
  end
end if SleepyPenguin.respond_to?(:copy_file_range)
