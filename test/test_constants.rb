require 'sleepy_penguin/sp'
require_relative 'helper'

class TestConstants < Test::Unit::TestCase
  def test_constants
    assert_equal SleepyPenguin::SLEEPY_PENGUIN_VERSION,
                 SP::SLEEPY_PENGUIN_VERSION
    v = SP::SLEEPY_PENGUIN_VERSION.split('.')

    (0..2).each do |i|
      assert_equal v[i], v[i].to_i.to_s, v.inspect
    end
  end
end
