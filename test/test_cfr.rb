# -*- encoding: binary -*-
require 'test/unit'
require 'tempfile'
$-w = true
require 'sleepy_penguin'

class TestCfr < Test::Unit::TestCase
  def test_copy_file_range
    str = 'abcde'
    size = 5
    src = Tempfile.new('ruby_cfr_src')
    dst = Tempfile.new('ruby_cfr_dst')
    assert_equal 5, src.syswrite(str)
    src.sysseek(0)
    begin
      nr = SleepyPenguin.copy_file_range(src, nil, dst, nil, size, 0)
    rescue Errno::EINVAL
      warn 'copy_file_range not supported (requires Linux 4.5+)'
      warn "We have: #{`uname -a`}"
      return
    end
    assert_equal nr, 5
    dst.sysseek(0)
    assert_equal str, dst.sysread(5)
  ensure
    dst.close!
    src.close!
  end
end if SleepyPenguin.respond_to?(:copy_file_range)
