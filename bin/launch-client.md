---
title: steam-runtime-launch-client
section: 1
...

<!-- This document:
Copyright © 2020-2021 Collabora Ltd.
SPDX-License-Identifier: MIT
-->

# NAME

steam-runtime-launch-client - client to launch processes in a container

# SYNOPSIS

**steam-runtime-launch-client**
[**--clear-env**]
[**--directory** *DIR*]
[**--env** _VAR_**=**_VALUE_]
[**--forward-fd** *FD*]
[**--inherit-env** *VAR*]
[**--inherit-env-matching** *WILDCARD*]
[**--pass-env** *VAR*]
[**--pass-env-matching** *WILDCARD*]
[**--unset-env** *VAR*]
[**--terminate**]
[**--verbose**]
{**--bus-name** *NAME*|**--dbus-address** *ADDRESS*|**--socket** *SOCKET*|**--host**|**--inside-app** *ID*|**--alongside-steam**}
[**--**]
[*COMMAND* [*ARGUMENTS...*]]

**steam-runtime-launch-client**
*OPTIONS*
**-c** *SHELL_COMMAND*
[**--**]
[*$0* *ARGUMENTS...*]

**steam-runtime-launch-client**
[**--verbose**]
**--list**

# DESCRIPTION

**steam-runtime-launch-client** connects to an `AF_UNIX` socket established
by **steam-runtime-launcher-service**(1), and executes an arbitrary command
as a subprocess of **steam-runtime-launcher-service**.

If no *COMMAND* is specified, and the **--terminate** option is not given,
then the default is to run an interactive shell.
This uses **$SHELL** if available in the container, falling back to
**bash**(1) or **sh**(1) if necessary.

# OPTIONS

**--socket** *PATH*, **--socket** *@ABSTRACT*
:   Connect to the given `AF_UNIX` socket, which can either be an
    absolute path, or `@` followed by an arbitrary string.
    An absolute path indicates a filesystem-based socket, which is
    associated with the filesystem and can be shared between filesystem
    namespaces by bind-mounting.
    `@` indicates an an abstract socket, which is associated with a
    network namespace, is shared between all containers that are in
    the same network namespace, and cannot be shared across network
    namespace boundaries.
    This option is deprecated: using a bus name on the well-known D-Bus
    session bus is preferred.

**--dbus-address** *ADDRESS*
:   The same as **--socket**, but the socket is specified in the form
    of a D-Bus address.
    This option is deprecated: using a bus name on the well-known D-Bus
    session bus is preferred.

**--alongside-steam**
:   Attempt to launch the *COMMAND* on behalf of a Steam app or game
    that is running inside a Steam Linux Runtime container, so that
    the *COMMAND* is run outside the container in approximately the
    same execution environment as the script that launches the Steam client.
    The Steam Runtime 1 'scout' and Steam Runtime 1½ 'heavy' library
    stacks that are used to run major components of the Steam client will
    usually **not** be present in the **LD_LIBRARY_PATH** for commands
    run like this.

    This is currently equivalent to
    **--bus-name=**_$SRT_LAUNCHER_SERVICE_ALONGSIDE_STEAM_
    (if that variable is set), followed by
    **--bus-name=com.steampowered.PressureVessel.LaunchAlongsideSteam**
    (unconditionally) and
    **--bus-name=org.freedesktop.portal.Flatpak** (if running inside the
    Flatpak per-app container for the unofficial Steam Flatpak app).
    It can be combined with other options that select a bus name:
    in particular it is valid to use
    **--alongside-steam --host**
    to run a command in the context of the Steam client if possible or
    on the host system otherwise, or
    **--host --alongside-steam**
    to use the opposite fallback sequence.

    Unlike **--host**, if the Steam client was run as a Flatpak or Snap
    application, the *COMMAND* will be run as part of that same Flatpak
    or Snap application, but possibly in a Flatpak subsandbox environment.

    The *COMMAND* will run in an unspecified execution environment which
    will frequently change between different host operating systems and
    over time, so this should only be used to run diagnostic commands
    and similar things that are not part of the critical path for a game
    to be run successfully.
    Executables which are intended to be run like this should be compiled
    against a glibc version that is as old as possible, and should
    either use a relative **DT_RPATH** or **DT_RUNPATH** to find bundled
    dependencies relative to the special token **${ORIGIN}**, or use
    **dlopen**(3) to tolerate dependencies not being loaded successfully.

    Official binary releases of the **pressure-vessel** tools are a good
    example of what is required to run successfully on an arbitrary host
    system: they are compiled in the Steam Runtime 1 'scout'
    environment, all required dependencies except for glibc are bundled
    with the pressure-vessel tools and loaded from
    **${ORIGIN}/../lib/x86_64-linux-gnu** using a **DT_RPATH**, and the
    optional **sd-journal**(3) library is loaded using **dlopen**(3).

**--bus-name** *NAME*
:   Connect to the well-known D-Bus session bus and send commands to
    the given *NAME*, which is normally assumed to be owned by
    **steam-runtime-launcher-service**.

    If used more than once, each *NAME* is tried in sequence and the
    first that is owned on the session bus will be used.

    As a special case, if the *NAME* is
    **org.freedesktop.Flatpak**, then it is assumed to be
    owned by the **flatpak-session-helper** per-user service, and the
    *COMMAND* is launched on the host system, similar to the
    **--host** option of **flatpak-spawn**(1).
    The **--terminate** option is not allowed in this mode.

    As another special case, if the *NAME* is
    **org.freedesktop.portal.Flatpak**, then it is assumed to be
    owned by the **flatpak-portal** per-user service, and the
    *COMMAND* is launched in a Flatpak sub-sandbox, similar to
    **flatpak-spawn**(1) without the **--host** option.
    Most options that alter how the sub-sandbox is created, such as
    **--sandbox-flag**, are not currently supported.
    As with **org.freedesktop.Flatpak**, the
    **--terminate** option is not allowed in this mode.

**--host**
:   Attempt to launch the *COMMAND* on the host system, similar to the
    **--host** option of **flatpak-spawn**(1).
    This is currently equivalent to
    **--bus-name=org.freedesktop.Flatpak**, and can be combined with
    other options that select a bus name, such as **--alongside-steam**.

    Similar to **--alongside-steam**, the *COMMAND* will run in an unspecified
    execution environment which will frequently change between different
    host operating systems and over time.
    See the description of **--alongside-steam** for more details on how to
    compile suitable executables.

**--inside-app** *ID*
:   Attempt to launch the *COMMAND* inside a pressure-vessel container
    that is currently running Steam app ID *ID*.
    *ID* may be **0**, for containers that are not running a Steam app.
    This is currently equivalent to
    **--bus-name=com.steampowered.App**_ID_, and can be combined with
    other options that select a bus name, although this is unlikely to be
    practically useful.

**--app-path** *PATH*, **--app-path=**
:   When creating a Flatpak subsandbox, mount *PATH* as the `/app` in
    the new subsandbox. If *PATH* is specified as the empty string,
    place an empty directory at `/app`.

    This option is only valid when used with
    **--bus-name=org.freedesktop.portal.Flatpak**, and requires
    Flatpak 1.12 or later.

**--clear-env**
:   Run the *COMMAND* in an empty environment, apart from any environment
    variables set by **--env** and similar options, and environment
    variables such as `PWD` that are set programmatically (see
    **ENVIRONMENT** section, below).
    If this option is not used, instead it inherits environment variables
    from **steam-runtime-launcher-service**, with **--env** and
    similar options overriding or unsetting individual variables.

**-c** *SHELL_COMMAND*, **--shell-command** *SHELL_COMMAND*
:   Run the *SHELL_COMMAND* using **sh**(1).
    If additional arguments are given, the first is used to set **$0**
    in the resulting shell, and the remaining arguments are the
    shell's positional parameters **$@**.
    This is a shortcut for using
    **-- sh -c** *SHELL_COMMAND* [*$0* [*ARGUMENT*...]]
    as the command and arguments.

**--directory** *DIR*
:   Arrange for the *COMMAND* to run with *DIR* as its current working
    directory.

    An empty string (typically written as **--directory=**) results in
    inheriting the current working directory from the service that
    will run the *COMMAND*.

    When using **--alongside-steam**, **--host**,
    **--bus-name=org.freedesktop.portal.Flatpak** or
    **--bus-name=org.freedesktop.Flatpak**,
    the default is to attempt to use the same working directory from
    which **steam-runtime-launch-client**(1) was run, similar to the
    effect of using **--directory="$(pwd)"** in a shell.
    This matches the behaviour of **flatpak-spawn**(1).

    Otherwise, the default is to inherit the current working directory
    from the service, equivalent to **--directory=**.

**--forward-fd** *FD*
:   Arrange for the *COMMAND* to receive file descriptor number *FD*
    from the current execution environment.
    File descriptors 0, 1 and 2
    (standard input, standard output and standard error) are always
    forwarded.

    If *FD* is a terminal, **steam-runtime-launch-client** will allocate
    a pseudo-terminal (pty) and pass the terminal end of the pty to the
    *COMMAND*, forwarding input and output between *FD* and the ptmx
    end of the pty.

**--list**
:   Instead of running a *COMMAND*, list the services that
    **steam-runtime-launch-client** could connect to.
    Each line of output is an option that could be passed to
    **steam-runtime-launch-client** to select a service.
    This uses a heuristic to identify possible targets from their bus
    names, so it is possible that not all possible targets are listed.
    In the current implementation, it lists bus names starting with
    **com.steampowered.App**, plus
    **com.steampowered.PressureVessel.LaunchAlongsideSteam** and Flatpak
    if available.

**--share-pids**
:   If used with **--bus-name=org.freedesktop.portal.Flatpak**, use the
    same process ID namespace for the new subsandbox as for the calling
    process. This requires Flatpak 1.10 or later, running on a host
    operating system where **bwrap**(1) is not and does not need to be
    setuid root.

    When communicating with a different API, this option is ignored.

**--terminate**
:   Terminate the **steam-runtime-launcher-service** server after the
    *COMMAND* finishes running.
    If no *COMMAND* is specified, terminate the server immediately.
    This option is not available when communicating with Flatpak.

**--usr-path** *PATH*
:   When creating a Flatpak subsandbox, mount *PATH* as the `/usr` in
    the new subsandbox.

    This option is only valid when used with
    **--bus-name=org.freedesktop.portal.Flatpak**, and requires
    Flatpak 1.12 or later.

**--verbose**
:   Be more verbose.

# ENVIRONMENT OPTIONS

Options from this group are processed in order, with each option taking
precedence over any earlier options that affect the same environment variable.
For example,
**--pass-env-matching="FO&#x2a;" --env=FOO=bar --unset-env=FOCUS**
will set **FOO** to **bar**, unset **FOCUS** even if the caller has
it set, and pass through **FONTS** from the caller.

If standard input, standard output or standard error is a terminal, then
the **TERM** environment variable is passed through by default, as if
**--pass-env=TERM** had been used at the beginning of the command line.
This behaviour can be overridden by other options that affect **TERM**,
or disabled with **--inherit-env=TERM**.

**--env** _VAR=VALUE_
:   Set environment variable _VAR_ to _VALUE_.
    This is mostly equivalent to using
    **env** _VAR=VALUE_ *COMMAND* *ARGUMENTS...*
    as the command.

**--inherit-env** *VAR*
:   Undo the effect of a previous **--env**, **--unset-env**, **--pass-env**
    or similar, returning to the default behaviour of inheriting *VAR*
    from the execution environment of the service that is used to run
    the *COMMAND* (unless **--clear-env** was used).

**--inherit-env-matching** *WILDCARD*
:   Do the same as for **--inherit-env** for any environment variable
    whose name matches *WILDCARD*.
    If this command is run from a shell, the wildcard will usually need
    to be quoted, for example **--inherit-env-matching="FOO&#x2a;"**.

**--pass-env** *VAR*
:   If the environment variable *VAR* is set, pass its current value
    into the container as if via **--env**. Otherwise, unset it as if
    via **--unset-env**.

**--pass-env-matching** *WILDCARD*
:   For each environment variable that is set and has a name matching
    the **fnmatch**(3) pattern *WILDCARD*, pass its current value
    into the container as if via **--env**.
    For example, **--pass-env-matching=Steam&#x2a;** copies Steam-related
    environment variables.
    If this command is run from a shell, the wildcard will usually need
    to be quoted, for example **--pass-env-matching="Steam&#x2a;"**.

**--unset-env** *VAR*
:   Unset *VAR* when running the command.
    This is mostly equivalent to using
    **env -u** *VAR* *COMMAND* *ARGUMENTS...*
    as the command.

# ENVIRONMENT

## Variables set for the command

Some variables will be set programmatically by
**steam-runtime-launcher-service** when the *COMMAND* is launched:

`MAINPID`
:   If **steam-runtime-launcher-service**(1) was run as a wrapper around a
    command (for example as
    **steam-runtime-launcher-service --bus-name=... -- my-game**),
    and the initial process of the wrapped command is still running,
    then this variable is set to its process ID (for example, the process
    ID of **my-game**). Otherwise, this variable is cleared.
    The environment options shown above will override this behaviour.

`PWD`
:   **steam-runtime-launcher-service**(1) sets this to the current working
    directory (as specified by **--directory**, or inherited from the
    launcher) for each command executed inside the container,
    overriding the environment options shown above.

## Variables read by steam-runtime-launch-client

Some variables affect the behaviour of **steam-runtime-launch-client**:

`PRESSURE_VESSEL_LOG_INFO` (boolean)
:   If set to 1, same as `SRT_LOG=info`

`PRESSURE_VESSEL_LOG_WITH_TIMESTAMP` (boolean)
:   If set to 1, same as `SRT_LOG=timestamp`

`SHELL`
:   If set to a non-empty value, it is used as the default shell when
    no *COMMAND* is provided.

`SRT_LAUNCHER_SERVICE_ALONGSIDE_STEAM`
:   If set, it is assumed to be the well-known or unique bus name of a
    **steam-runtime-launcher-service**(1) outside the Steam Linux Runtime
    container, used when implementing **--alongside-steam**.

`SRT_LOG`
:   A sequence of tokens separated by colons, spaces or commas
    affecting how output is recorded. See source code for details.

`TERM`
:   If standard input, standard output or standard error is a terminal,
    then this environment variable is passed to the *COMMAND* by default.

# OUTPUT

The standard output from *COMMAND* is printed on standard output.

The standard error from *COMMAND* is printed on standard error.
Diagnostic messages from **steam-runtime-launch-client** may also be printed
on standard error.

# EXIT STATUS

The exit status is similar to **env**(1):

0
:   The *COMMAND* exited successfully with status 0,
    or there was no *COMMAND* and **--terminate** succeeded.

125
:   Invalid arguments were given, or **steam-runtime-launch-client** failed
    to start.

126
:   Reserved to indicate inability to launch the *COMMAND*.
    This is not currently distinguished from exit status 125.

127
:   Reserved to indicate that the *COMMAND* was not found.
    This is not currently distinguished from exit status 125.

128
:   The *COMMAND* was launched, but its exit status could not be
    determined. This happens if the wait-status was neither
    normal exit nor termination by a signal. It also happens if
    **steam-runtime-launch-client** was disconnected from the D-Bus
    session bus or the **--socket** before the exit status could be
    determined.

Any value less than 128
:   The *COMMAND* exited unsuccessfully with the status indicated.

128 + *n*
:   The *COMMAND* was killed by signal *n*.
    (This is the same encoding used by **bash**(1), **bwrap**(1) and
    **env**(1).)

# EXAMPLES

A copy of **steam-runtime-launch-client** can be found in
`~/.steam/root/ubuntu12_32/steam-runtime/amd64/usr/bin/`,
`.../steamapps/common/SteamLinuxRuntime_soldier/pressure-vessel/bin/`
or
`.../steamapps/common/SteamLinuxRuntime_sniper/pressure-vessel/bin/`.

For a Steam game that runs under Proton 7.0 or later, if you set its
Steam Launch Options to

    STEAM_COMPAT_LAUNCHER_SERVICE=proton %command%

then you can run commands in its execution environment with commands
like:

    $ steam-runtime-launch-client --list
    --bus-name=com.steampowered.App312990
    --bus-name=com.steampowered.App312990.Instance123

    $ steam-runtime-launch-client \
        --bus-name=com.steampowered.App312990 \
        -- \
        wine winedbg notepad.exe

Similarly, for a Steam game that runs under the
"Steam Linux Runtime 1.0 (scout)"
compatibility tool, if you set its Steam Launch Options to

    STEAM_COMPAT_LAUNCHER_SERVICE=scout-in-container %command%

then you can attach a debugger with commands like:

    $ steam-runtime-launch-client --list
    --bus-name=com.steampowered.App440
    --bus-name=com.steampowered.App440.Instance54321

    $ pgrep hl2_linux
    12345

    $ gdb ./hl2_linux
    (gdb) set sysroot /proc/12345/root
    (gdb) target remote | \
        steam-runtime-launch-client \
        --bus-name=com.steampowered.App440 \
        -- gdbserver --attach - 12345
    (gdb) thread apply all bt
    (gdb) detach

If a game is crashing on startup, this can be debugged by setting its
Steam Launch Options to

    SRT_LAUNCHER_SERVICE_STOP_ON_EXIT=0 STEAM_COMPAT_LAUNCHER_SERVICE=proton %command%

and then running debugging commands in its Proton environment with
commands like:

    steam-runtime-launch-client \
        --bus-name=com.steampowered.App312990 \
        -- \
        wine64 winedbg Expendabros.exe

To exit the command server when finished, use a command like:

    steam-runtime-launch-client \
        --bus-name=com.steampowered.App312990 \
        --terminate

<!-- vim:set sw=4 sts=4 et: -->
