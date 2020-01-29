manifest = File.exist?('.manifest') ?
  IO.readlines('.manifest').map!(&:chomp!) : `git ls-files`.split("\n")

Gem::Specification.new do |s|
  s.name = %q{sleepy_penguin}
  s.version = (ENV['VERSION'] || '3.5.1').dup
  s.homepage = 'https://yhbt.net/sleepy_penguin/'
  s.authors = ['sleepy_penguin hackers']
  s.description = File.read('README').split("\n\n")[1]
  s.email = %q{sleepy-penguin@yhbt.net}
  s.files = manifest
  s.summary = 'Linux I/O events for Ruby'
  s.test_files = Dir['test/test_*.rb']
  s.extensions = %w(ext/sleepy_penguin/extconf.rb)
  s.extra_rdoc_files = IO.readlines('.document').map!(&:chomp!).keep_if do |f|
    File.exist?(f)
  end
  s.add_development_dependency('test-unit', '~> 3.0')
  s.add_development_dependency('strace_me', '~> 1.0')
  s.required_ruby_version = '>= 2.0'
  s.licenses = %w(LGPL-2.1+)
end
