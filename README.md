# phi-runtime

The event loop the phi daemons and their plugins share (the "Leaving Qt"
migration, phase 6e): `phi::runtime::Loop` (epoll, fd watches, deadline
timers, cross-thread `post()`), `Thread` (a std::thread running a Loop of
its own), `postAndWait`, `Signal`, `OneShots` and `BoundedQueue`.

One dynamic library on purpose: a `Loop&` crosses plugin boundaries, and
that is only sound when every side sees the same loop state - a static
copy per plugin would duplicate `Loop::current()`'s thread-local and make
every affinity assert lie.

Planned to grow in 6e-b: `net::` (sockets, TLS, HTTP, ws framing) and
`log::` (the shared log vocabulary). See phi-core's docs/ROADMAP.md.

SONAME policy: deliberately unstable while the API settles. Every breaking
cut bumps `libphi_runtime.so.<N>`; consumers rebuild, forced by
dpkg-shlibdeps. No ABI promise is made that this phase cannot keep.
