ENV["VERSION"] or abort "VERSION= must be specified"
manifest = File.readlines('.manifest').map! { |x| x.chomp! }
require 'olddoc'
extend Olddoc::Gemspec
name, summary, title = readme_metadata

Gem::Specification.new do |s|
  s.name = %q{sleepy_penguin}
  s.version = ENV["VERSION"].dup
  s.homepage = Olddoc.config['rdoc_url']
  s.authors = ["#{name} hackers"]
  s.description = readme_description
  s.email = %q{sleepy-penguin@bogomips.org}
  s.extra_rdoc_files = extra_rdoc_files(manifest)
  s.files = manifest
  s.rdoc_options = rdoc_options
  s.summary = summary
  s.test_files = Dir['test/test_*.rb']
  s.extensions = %w(ext/sleepy_penguin/extconf.rb)
  s.add_development_dependency('olddoc', '~> 1.0')
  s.add_development_dependency('strace_me', '~> 1.0')

  s.licenses = %w(LGPLv2.1+)
end
