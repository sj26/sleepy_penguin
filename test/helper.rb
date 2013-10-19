$-w = $stdout.sync = $stderr.sync = Thread.abort_on_exception = true
gem 'minitest'
require 'minitest/autorun'
Testcase = begin
  Minitest::Test # minitest 5
rescue NameError
  Minitest::Unit::TestCase # minitest 4
end
