$-w = $stdout.sync = $stderr.sync = Thread.abort_on_exception = true
gem 'minitest'
require 'minitest/autorun'
Testcase = begin
  Minitest::Test # minitest 5
rescue NameError
  Minitest::Unit::TestCase # minitest 4
end

def check_cloexec(io)
  pipe = IO.pipe
  rbimp = Fcntl::FD_CLOEXEC & pipe[0].fcntl(Fcntl::F_GETFD)
  ours = Fcntl::FD_CLOEXEC & io.fcntl(Fcntl::F_GETFD)
  assert_equal rbimp, ours, "CLOEXEC default does not match Ruby implementation"
ensure
  pipe.each { |io| io.close }
end
