require 'mkmf'
have_header('sys/epoll.h')
dir_config('kqueue')
if have_header('sys/event.h')
  have_library('kqueue')
end
have_header('sys/mount.h')
have_header('sys/eventfd.h')
have_header('sys/sendfile.h')

# it's impossible to use signalfd reliably with Ruby since Ruby currently
# manages # (and overrides) all signal handling
# have_header('sys/signalfd.h')

have_header('sys/timerfd.h')
have_header('sys/inotify.h')
have_header('ruby/io.h') and have_struct_member('rb_io_t', 'fd', 'ruby/io.h')
unless have_macro('CLOCK_MONOTONIC', 'time.h')
  have_func('CLOCK_MONOTONIC', 'time.h')
end
have_type('clockid_t', 'time.h')
have_func('clock_gettime', 'time.h')
have_func('copy_file_range')
have_func('epoll_create1', %w(sys/epoll.h))
have_func('inotify_init1', %w(sys/inotify.h))
have_func('splice', %w(fcntl.h))
have_func('tee', %w(fcntl.h))
have_macro('F_GETPIPE_SZ', %w(fcntl.h))
have_macro('F_SETPIPE_SZ', %w(fcntl.h))
have_func('rb_thread_call_without_gvl')
have_func('rb_thread_blocking_region')
have_func('rb_thread_io_blocking_region')
have_func('rb_thread_fd_close')
have_func('rb_update_max_fd')
have_func('rb_fd_fix_cloexec')
have_func('rb_io_get_io')
create_makefile('sleepy_penguin_ext')
