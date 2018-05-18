pressure-vessel — tools to put Steam in containers
==================================================

pressure-vessel is a bit like a simplified version of Flatpak for Steam games.

Use cases
---------

### Protecting `$HOME`

If we change the meaning of `/home` for a game to be a fake
app-specific directory, then the game can't write random files in unknown
subdirectories of $HOME, and we can be reasonably sure that uninstalling
a game removes all of its files. (As a side benefit, this could make it
easier to be sure that all the necessary files have been selected for
Steam Cloud Sync, because we could prevent the game writing anywhere
that won't be synchronised, other than /tmp or similar.)

### Using an alternative runtime

We can get a more predictable library stack than the
`LD_LIBRARY_PATH`-based Steam Runtime by making an alternative runtime
available over `/usr`, `/lib*`, `/bin`, `/sbin`, analogous to Flatpak
runtimes, using some or all of the host system's graphics stack.

A future goal is to use [libcapsule][] to avoid the library dependencies
of the host system's graphics stack influencing the libraries loaded by
games, particularly libstdc++. This is not yet implemented.

[libcapsule]: https://gitlab.collabora.com/vivek/libcapsule/

Building a relocatable install
------------------------------

Build a Debian source package (`.dsc`, `.debian.tar.*`, `.orig.tar.*`
for libcapsule 0.20180430.0 or later, on a system with autoconf-archive
20160916-1~bpo8+1 or later. Put it in the top-level directory of
`pressure-vessel`, for example:

    dcmd cp ../build-area/libcapsule_0.20180430.0-0co1.dsc .

To make the built version compatible with older systems, you will need a
Debian 8 'jessie' environment with some extra packages. SteamOS 2 'brewmaster'
is not suitable, because its amd64 and i386 linux-libc-dev packages are
not co-installable.

The simplest way is to do the build in a Debian 8 'jessie' amd64 container
with selected i386 packages available, like `ci/Jenkinsfile` does:

    make sysroot=/

Alternatively, you can use `bubblewrap` to enter a sysroot prepared
using [debos][] and [qemu-system-x86_64][qemu]. If
you have all those, you can just run:

    make

Alternatively, prepare a Debian jessie sysroot with `deb`
and `deb-src` sources and an `i386` foreign architecture (see
`sysroot/configure-sources.sh`), and all the packages listed in
`sysroot/install-dependencies.sh`) and use:

    make sysroot=/path/to/sysroot

or compress a similar chroot into a gzipped tar file (containing `./etc`,
`./usr` and so on, as used by [lxc][] and [pbuilder][]) and use:

    make tarball=/path/to/tarball.tar.gz

Binaries and source code end up in `relocatable-install/` which can be
copied to wherever you want.

[debos]: https://github.com/go-debos/debos
[lxc]: https://github.com/lxc/lxc
[pbuilder]: https://pbuilder.alioth.debian.org/
[qemu]: https://www.qemu.org/

Instructions for testing
------------------------

* Install Steam and some games. Convenient choices for a reasonably
  small free-to-play game on various engines include:

    - Floating Point (game ID 302380)
        - very small/quick to install, works OK on weak hardware
        - UnityPlayer 4.3.4f1
    - Life is Strange, episode 1 (319630)
        - Unreal Engine 3
    - Team Fortress 2 (440)
        - Source engine
    - Unturned (304930)
        - UnityPlayer 5.5.3f1

* Unpack `pressure-vessel-`*VERSION*`-bin.tar.gz` (or `...-bin+src.tar.gz`)
  in some convenient place (I used `/opt/pressure-vessel`, but a
  production deployment in Steam would more realistically use somewhere
  under `~/.steam` or `~/.local/share/Steam`):

    - be a user who can sudo

        $ cd /opt
        $ sudo mkdir pressure-vessel
        $ sudo chown $(id -nu) pressure-vessel
        $ tar --strip-components=1 -C pressure-vessel -xzvf ~/pressure-vessel-*-bin.tar.gz
        $ chmod -R a+rX pressure-vessel

* Launch a game once without pressure-vessel

* Configure the launch options for the chosen game:

    - be the Steam user, possibly via sudo -u steam -s

        $ nano ~/.steam/steam/userdata/[0-9]*/config/localconfig.vdf

    - navigate to UserLocalConfigStore/Software/Valve/Steam/Apps/*game ID*
    - below LastPlayed, add:

        "LaunchOptions" "/opt/pressure-vessel/bin/pressure-vessel-wrap -- %command%"

    - restart Steam (on SteamOS use `sudo systemctl restart lightdm`)

        - TODO: Is there a scriptable way to make Steam edit `localconfig.vdf`
            itself, or to make it reload an edited `localconfig.vdf`?

* Run the game. If successful, `pstree -pa` will look something like this:

        |-SteamChildMonit,13109 -tenfoot -steamos -enableremotecontrol
        |   `-sh,13110 -c...
        |       `-pressure-vessel,13111 /opt/pressure-vessel/bin/pressure-vessel-wrap...
        |           `-bwrap,13132 --new-session --setenv LD_LIBRARY_PATH...
        |               `-Floating Point.,13133

* To use a runtime instead of the host system, use:

        /opt/pressure-vessel/bin/pressure-vessel-wrap --runtime=$HOME/some-runtime -- %command%

    The runtime can be either:

    - A merged `/usr` containing `bin/bash`, `lib/ld-linux.so.2`,
      `bin/env`, `share/locale`, `lib/python2.7` and so on

    - A Flatpak runtime such as
      `~/.local/share/flatpak/runtime/com.valvesoftware.SteamRuntime.Platform/x86_64/scout_beta/active/files`,
      for example produced by [flatdeb][] (this is a special case of a
      merged `/usr`). For example, as the user that will run Steam:

            $ flatpak --user remote-add --no-gpg-verify smcv-flatdeb https://people.collabora.com/~smcv/flatdeb/repo
            $ flatpak --user install smcv-flatdeb com.valvesoftware.SteamRuntime.Platform/x86_64/scout_beta
            $ ln -fns ~/.local/share/flatpak/runtime/com.valvesoftware.SteamRuntime.Platform/x86_64/scout_beta/active/files \
                ~/scout_beta-runtime

        and then set the launch options to:

            /opt/pressure-vessel/bin/pressure-vessel-wrap --runtime=$HOME/scout_beta-runtime -- %command%

    - A sysroot containing `bin/bash`, `lib/ld-linux.so.2`,
      `usr/bin/env`, `usr/share/locale`, `usr/lib/python2.7` and so on,
      optionally with `bin`, `lib` etc. being symlinks into `usr`

    In this mode, graphics drivers come from the host system, while
    their dependencies (notably glibc and libstdc++) come from either
    the host system or the runtime, whichever appears to be newer. This
    is currently hard-coded in `pressure-vessel-wrap` and cannot be
    configured.

    To see the filesystem in which the game is executing, get the process ID
    of some game process from `ps` and use:

        $ sudo ls -l /proc/$game_pid/root/

    You'll see that the graphics drivers and possibly their dependencies
    are available in `/overrides` inside that filesystem, while selected
    files from the host are visible in `/run/host`.

* To test something manually:

    - cd to the directory that you want to be the current working directory
      inside the container

    - Run one of:

            /opt/pressure-vessel/bin/pressure-vessel-wrap --fake-home=/some/path --interactive -- ./whatever-game
            /opt/pressure-vessel/bin/pressure-vessel-wrap --freedesktop-app-id=com.example.Anything --interactive -- ./whatever-game
            /opt/pressure-vessel/bin/pressure-vessel-wrap --steam-app-id=70 --interactive -- ./whatever-game

* For interactive testing, if your runtime (if used) or host system (if no
    runtime) contains an xterm binary, you can use something like:

        /opt/pressure-vessel/bin/pressure-vessel-wrap --xterm -- %command%

    to run an xterm containing an interactive shell. The interactive
    shell's current working directory matches the game's. Run `"$@"` in
    the interactive shell to run the game.

* For interactive testing, if you ran Steam from a shell (not normally
    valid on SteamOS!), you can use:

        /opt/pressure-vessel/bin/pressure-vessel-wrap --interactive -- %command%

    The interactive shell's current working directory matches the game's.
    Run `"$@"` in the interactive shell to run the game.

Design constraints
------------------

* Steam games assume that they are executed with the current working
  directory equal to the game's base directory, typically something
  like `~/.steam/steam/SteamApps/common/Half-Life`.

* Steam games are delivered via existing code to use the Steam CDN, not
  as Flatpak packages.

* Each game is assumed to have one subdirectory of `SteamApps`
  (for example `~/.steam/steam/SteamApps/common/Half-Life`) and one
  *AppID* (for example 70 for [Half-Life][]). These are not correlated
  in any obvious way.

  [Half-Life]: https://store.steampowered.com/app/70/HalfLife/

* Some games have distinct app IDs but share data: for example,
  [X3: Terran Conflict][] (ID 2820) and [X3: Albion Prelude][] (ID 201310)
  share `SteamApps/common/X3 Terran Conflict`.

  [X3: Terran Conflict]: https://store.steampowered.com/app/2820/X3_Terran_Conflict/
  [X3: Albion Prelude]: https://store.steampowered.com/app/201310/X3_Albion_Prelude/

* The Steam client and client library need to be able to communicate
  via shared memory and SysV semaphores.

    - ⇒ We must share `/dev/shm`
    - ⇒ We must not unshare the IPC namespace

Assumptions
-----------

* Games may write to their own subdirectories of `SteamApps/common`.

* Games should not see each other's saved games and other private data
  in `XDG_CONFIG_HOME` etc.

* X11, Wayland, PulseAudio, D-Bus etc. are assumed to use sockets in
  `/run` (including `$XDG_RUNTIME_DIR`), `/var` or `/tmp`.

    - X11 uses `/tmp`: OK
    - Wayland uses `$XDG_RUNTIME_DIR`: OK
    - The D-Bus system bus always uses `/var/run`: OK
    - The D-Bus session bus uses `$XDG_RUNTIME_DIR/bus` on systems with
      systemd, systemd-logind and the user bus: OK
    - The D-Bus session bus uses abstract Unix sockets on other systems: OK
    - PulseAudio uses `$XDG_RUNTIME_DIR` on modern systems with systemd: OK
    - PulseAudio uses `/var/run` on SteamOS: OK
    - PulseAudio might not work on non-systemd non-SteamOS machines?

* Games can't see `~/.local/share/fonts`, `~/.fonts`, `~/Music`,
  `~/Desktop`, etc. and this is assumed to be OK.

TODO
----

* Games can currently read each other's static data via `~/.steam/steam`
  and `~/.steam/root`. This could be prevented, if we can define which
  parts of `~/.steam` are considered to be API and which are to be
  masked.

* What paths did PulseAudio historically use? It might not work on
  non-systemd non-SteamOS machines.

* Games like AudioSurf that want to read `~/Music` aren't going to work.
  They'll need some new special-case option analogous to
  Flatpak's `--filesystem=xdg-music:ro`.

* To avoid [weird behaviour][] when part of a game respects
  `XDG_CONFIG_HOME` and part of it hard-codes `~/.config`, maybe we
  should make `$fake_home/config` a synonym (bind-mount) for
  `$fake_home/.config`, and the same for `$fake_home/.local/share` with
  `$fake_home/data` and `$fake_home/.cache` with `$fake_home/cache`?

  [weird behaviour]: https://www.ctrl.blog/entry/flatpak-steamcloud-xdg

* Team Fortress 2 reports an error because it is unable to set the
  `en_US.UTF-8` locale (but then starts successfully anyway).

Design
------

Each game gets a private home directory in `~/.var/app`, the same as
Flatpak apps do. The name of the private home directory follows the
[naming recommendations from the freedesktop.org Desktop Entry
Specification](https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html#file-naming).
For Steam apps, unless a better name has been specified, the app's
freedesktop.org app ID is based on its Steam AppID, for example
`com.steampowered.App70` for Half-Life.

For the game, `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and `XDG_CACHE_HOME`
are set to the `./config`, `./data` and `./cache` directories inside
its private home directory, so well-behaved freedesktop-style apps
will write there.

* Example: [X3: Terran Conflict][] writes to `$XDG_CONFIG_HOME/EgoSoft/X3TC`
* Example: Mesa writes to `$XDG_CACHE_HOME/mesa` (assuming
  `$MESA_GLSL_CACHE_DIR` is unset)

Anything that hard-codes a path relative to `$HOME` (including `.config`,
`.local/share` or `.cache`) will write to the corresponding directory in
`~/.var/app`.

* Example: [Avorion][] writes to `~/.avorion/` in the container, which is
  `~/.var/app/com.steampowered.App445220/.avorion/` in the real home
  directory

  [Avorion]: https://store.steampowered.com/app/445220/Avorion/

If more than one game is meant to share a private home directory, then
they need to be run with `--freedesktop-app-id=` or `--steam-app-id=`
to set that up.

* For example, if [X3: Terran Conflict][] and [X3: Albion Prelude][]
  are to share data, they would have to both be run with for example
  `--freedesktop-app-id=com.egosoft.X3TC` or `--steam-app-id=2820`.

The app is not currently made available read-only on `/app` (as it
would be in Flatpak). It could be, if desired.
