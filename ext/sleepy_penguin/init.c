void sleepy_penguin_init_epoll(void);

#ifdef HAVE_SYS_TIMERFD_H
void sleepy_penguin_init_timerfd(void);
#else
#  define sleepy_penguin_init_timerfd() for(;0;)
#endif

#ifdef HAVE_SYS_EVENTFD_H
void sleepy_penguin_init_eventfd(void);
#else
#  define sleepy_penguin_init_eventfd() for(;0;)
#endif

void Init_sleepy_penguin_ext(void)
{
	sleepy_penguin_init_epoll();
	sleepy_penguin_init_timerfd();
	sleepy_penguin_init_eventfd();
}
