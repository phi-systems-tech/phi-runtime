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

## MQTT client

`phi/runtime/mqtt/client.h`, `phi::runtime::mqtt::Client`: libmosquitto on
the caller's loop - no network thread of libmosquitto's own - with TLS
(`Tls`, the chain always verified), a last will (`Will`), a connect deadline
and disconnect reasons a person can read. It is the one copy of what
phi-transport-voice, phi-adapter-voice, phi-adapter-z2m and phi-adapter-thinq
each carried until 2026-10-27.

A library of its own, `libphi_runtime_mqtt` (packages `libphi-runtime-mqtt0`
and `-dev`), linked to libmosquitto privately: the loop stays free of it, and
a consumer needs neither libmosquitto's headers nor its CMake.

    find_package(phi-runtime-mqtt CONFIG REQUIRED)
    target_link_libraries(my-adapter PRIVATE phi::runtime_mqtt)

`tests/mqtt_fakebroker_tests.cpp` runs it against a broker of its own that
refuses, says nothing or goes away (from phi-adapter-z2m);
`tests/mqtt_tests.cpp` runs against a mosquitto it starts on a free port
(publish, subscribe, retained, will, clean disconnect, broker gone) and is
skipped where `/usr/sbin/mosquitto` is missing, as in a package build.
