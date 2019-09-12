---
title: steam-runtime-system-info
section: 1
...

# NAME

steam-runtime-system-info - examine the Steam runtime environment and diagnose potential issues

# SYNOPSIS

**steam-runtime-system-info**
[**--expectations** *PATH*]
[**--verbose**]

# DESCRIPTION

# OPTIONS

**--expectations** *PATH*
:   Path to a directory containing details of the libraries that are
    expected to be available. By default, *$STEAM_RUNTIME***/usr/lib/steamrt**
    or **/usr/lib/steamrt** is used.

**--verbose**
:   Show additional information. This currently adds details of all the
    expected libraries that loaded successfully.

# OUTPUT

The output is a JSON *object* (mapping, dictionary) with the following
keys:

**can-write-uinput**
:   A boolean value indicating whether **/dev/uinput** can be opened for
    writing. If this is **false**, the Steam Controller will not be able
    to emulate a keyboard, a mouse or a different game controller.

**steam-installation**
:   An object describing the Steam installation. The keys are strings:

    **path**
    :   A string: the absolute path to the Steam installation.

    **issues**
    :   An array of strings representing problems with the Steam
        installation. If empty, no problems were found.

        **internal-error**
        :   An error occurred while checking the Steam installation.

        **cannot-find**
        :   The Steam installation could not be found.

        **dot-steam-steam-not-symlink**
        :   **~/.steam/steam** is not a symbolic link, for example
            due to [Debian bug #916303](https://bugs.debian.org/916303).

**runtime**
:   An object describing the `LD_LIBRARY_PATH`-based Steam Runtime.
    The keys are strings:

    **path**
    :   A string: the absolute path to the Steam Runtime.

    **version**
    :   The version number of the Steam Runtime, for example
        `0.20190711.3`.

    **issues**
    :   An array of strings representing problems with the Steam
        Runtime. If empty, no problems were found.

        **internal-error**
        :   An error occurred while checking the Steam Runtime.

        **disabled**
        :   The Steam Runtime has been explicitly disabled. This is
            intended for use by game and Steam Runtime developers, and
            is not supported.

        **not-runtime**
        :   The **STEAM_RUNTIME** environment variable does not point
            to a directory that looks like a Steam Runtime.

        **unofficial**
        :   The **STEAM_RUNTIME** environment variable points to an
            unofficial Steam Runtime build.

        **unexpected-location**
        :   The **STEAM_RUNTIME** environment variable points to a Steam
            Runtime that is not in the expected location in the Steam
            installation.

        **unexpected-version**
        :   The **STEAM_RUNTIME** environment variable points to a Steam
            Runtime that was not the expected version.

        **not-in-ld-path**
        :   The Steam Runtime does not appear in the **LD_LIBRARY_PATH**.
            Libraries from the Steam Runtime will not be found correctly.

        **not-in-path**
        :   The Steam Runtime does not appear in the **PATH**.
            Executables from the Steam Runtime will not be found correctly.

        **not-in-environment**
        :   The **STEAM_RUNTIME** environment variable is not set.

        **not-using-newer-host-libraries**
        :   "Pinned" shared libraries from the host system are not in
            their intended places in the **LD_LIBRARY_PATH**. This is
            likely to break the use of libraries from the host system,
            in particular the Mesa graphics drivers.

**architectures**
:   An object with architecture-specific information. The keys are
    Debian-style *multiarch tuples* describing ABIs, as returned by
    `gcc -print-multiarch`: the multiarch tuples normally used on x86 PCs
    are `x86_64-linux-gnu` for 64-bit binaries and `i386-linux-gnu` for
    32-bit binaries (the multiarch tuple contains i386 even if they were
    built for an i486, i586 or i686 baseline instruction set).

    The values are objects with the following keys:

    **can-run**
    :   A boolean value indicating whether a simple executable for this
        architecture was run successfully.

    **library-issues-summary**
    :   A summary of issues found when loading the expected libraries,
        as an array of strings. If empty, no problems were found.

        **internal-error**
        :   There was an internal error while checking libraries.

        **cannot-load**
        :   At least one of the expected libraries could not be loaded.

        **missing-symbols**
        :   At least one of the expected libraries did not contain all
            the symbols that were expected, indicating use of an
            incompatible version.

        **misversioned-symbols**
        :   At least one of the expected libraries did not contain all
            the symbol versioning that was expected, indicating use of an
            incompatible version.

        **unknown-expectations**
        :   Details of the expected libraries were not found.

    **library-details**
    :   An object representing shared libraries. If the **--verbose**
        option was used, all known libraries are listed here; if not,
        only the libraries with problems are listed.

        The keys are library SONAMEs such as **libc.so.6**. The values
        are objects with these string keys:

        **path**
        :   The absolute path to the library.

        **issues**
        :   Problems with the library, as arrays of the same strings
            described in **library-issues-summary** above.

        **missing-symbols**
        :   If the library has missing symbols they are listed here.

        **misversioned-symbols**
        :   If the library has missing versioned symbols, but the same
            symbol exists with an unexpected version or no version, they
            are listed here.

    **graphics-details**
    :   An object representing graphics stacks. The keys are strings
        such as **glx/gl**, **egl_x11/glesv2** or **x11/vulkan**
        representing the combination of a window system interface and a
        rendering interface.

        Known window system interfaces:

        **x11**
        :   The X11 windowing system for Vulkan

        **glx**
        :   GLX, the traditional OpenGL API for X11

        **egl_x11**
        :   EGL, a windowing system interface for either OpenGL or OpenGL ES,
            rendering to an X11 window

        Known rendering interfaces:

        **gl**
        :   "Desktop" OpenGL

        **glesv2**
        :   OpenGL ES v2

        **vulkan**
        :   Vulkan

        The values are objects with the following string keys:

        **renderer**
        :   The renderer string describing the graphics device and/or driver,
            or **null** if it could not be determined

        **version**
        :   The version string describing the graphics API and/or driver,
            or **null** if it could not be determined

        **issues**
        :   Problems with this graphics stack, represented as an array
            of strings. The array is empty if no problems were detected.

            **internal-error**
            :   There was an internal error while checking graphics support.

            **cannot-load**
            :   The necessary libraries could not be loaded, or a rendering
                context could not be created

            **software-rendering**
            :   This graphics stack appears to be using unaccelerated
                software rendering.

**locale-issues**
:   An array of strings indicating locale-related issues.
    The array is empty if no problems were detected.

    **internal-error**
    :   There was an internal error while checking locale support.

    **default-missing**
    :   The system's configured locale is not available.

    **default-not-utf8**
    :   The system's configured locale is not a UTF-8 locale such as
        `en_US.UTF-8` or `de_DE.UTF-8`. This may cause problems in games
        and frameworks that assume that all strings are UTF-8, or that
        assume that all Unicode characters can be output.

    **c-utf8-missing**
    :   The special `C.UTF-8` locale provided by Debian and Fedora
        derivatives is not available. This may cause problems in games
        and frameworks that assume it is always present.

    **en-us-utf8-missing**
    :   The `en_US.UTF-8` locale is not available. This may cause
        problems in games and frameworks that assume it is always present.

    **i18n-supported-missing**
    :   The **SUPPORTED** file listing supported locales was not found
        in the expected location.
        This indicates that either locale data is not installed, or this
        operating system does not put it in the expected location.
        The Steam Runtime might be unable to generate extra locales if needed.

    **i18n-locales-en-us-missing**
    :   The **locales/en_US** file describing the USA English locale was
        not found in the expected location. This indicates that either
        locale data is not installed, or this operating system does not
        put it in the expected location, or only a partial set of locale
        source data is available. The Steam Runtime will be unable to
        generate extra locales if needed.

**locales**
:   An object describing locales. The keys are either **<default>** or
    a locale name. The values are objects containing either details of
    a locale:

    **resulting-name**
    :   The canonical name of the locale, which might differ from the
        name that was requested.

    **charset**
    :   The character set used by the locale, for example **UTF-8**.

    **is_utf8**
    :   Whether the locale appears to be UTF-8.

    or details of the error encountered when attempting to set a locale:

    **error-domain**
    :   A machine-readable string indicating a category of errors.

    **error-code**
    :   A small integer indicating a specific error. Its meaning depends
        on the **error-domain**.

    **error**
    :   A human-readable error message.

# EXIT STATUS

0
:   **steam-runtime-system-info** ran successfully. Note that this does
    not mean that Steam games will run successfully: inspect the JSON
    output to see whether there are problems.

Nonzero
:   An error occurred.

# EXAMPLES

To examine the Steam Runtime:

    $ ~/.local/share/Steam/ubuntu12_32/steam-runtime/setup.sh
    Pins up-to-date!
    $ env \
    PATH="$(~/.local/share/Steam/ubuntu12_32/steam-runtime/setup.sh --print-bin-path):$PATH" \
    ~/.local/share/Steam/ubuntu12_32/steam-runtime/run.sh \
    steam-runtime-system-info

Or change the launch options for a Steam game to:

    xterm & %command%

then launch the game, and in the resulting **xterm**, run

    $ steam-runtime-system-info

(Note that Steam will not consider the game to have exited until the
**xterm** is closed.)

<!-- vim:set sw=4 sts=4 et: -->