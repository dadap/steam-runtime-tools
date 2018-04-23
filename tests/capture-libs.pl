#!/usr/bin/perl

# Copyright © 2017 Collabora Ltd
#
# This file is part of libcapsule.
#
# libcapsule is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as
# published by the Free Software Foundation; either version 2.1 of the
# License, or (at your option) any later version.
#
# libcapsule is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with libcapsule.  If not, see <http://www.gnu.org/licenses/>.

use autodie;
use warnings;
use strict;

use Cwd qw(abs_path);
use File::Temp qw();
use IPC::Run qw(run);
use Test::More;

use FindBin;
use lib $FindBin::Bin;

use CapsuleTest;

skip_all_unless_bwrap;

my $LIBDIR = qr{/(?:usr/)?lib(?:32|64|x32)?(?:/\w+-linux-gnu\w*)?};

my $test_tempdir = File::Temp->newdir();
diag "Working directory: $test_tempdir";
my $host = "${test_tempdir}/host";
mkdir($host);
my $libdir = "${test_tempdir}/libdir";

ok(! system('rm', '-fr', $libdir));
mkdir($libdir);
run_ok([$CAPSULE_CAPTURE_LIBS_TOOL, 'soname:libc.so.6'], '>&2',
    init => sub { chdir $libdir or die $!; });
ok(-e "$libdir/libc.so.6", 'libc.so.6 was captured');
like(readlink "$libdir/libc.so.6", qr{^$LIBDIR/libc\.so\.6$},
    '$libdir/libc.so.6 is a symlink to the real libc.so.6');
my $libc = abs_path("$libdir/libc.so.6");
ok(-e "$libdir/libBrokenLocale.so.1", 'related libraries were captured');
ok(-e "$libdir/libm.so.6", 'related libraries were captured');
ok(-e "$libdir/libpthread.so.0", 'related libraries were captured');
ok(-e "$libdir/libmemusage.so", 'related libraries were captured');
like(readlink "$libdir/libm.so.6", qr{^$LIBDIR/libm\.so\.6$},
    'libm is a symlink to the right place');
my @libc_family;
{
    opendir(my $dir_iter, $libdir);
    @libc_family = sort grep { $_ ne '.' and $_ ne '..' } readdir $dir_iter;
    closedir $dir_iter;
    foreach my $symlink (@libc_family) {
        diag "- $symlink -> ".readlink("$libdir/$symlink");
    }
}

ok(! system('rm', '-fr', $libdir));
mkdir($libdir);
run_ok([$CAPSULE_CAPTURE_LIBS_TOOL, '--link-target=/run/host',
        "--dest=$libdir", 'soname-match:libc.so.6'], '>&2');
{
    opendir(my $dir_iter, $libdir);
    my @links = sort grep { $_ ne '.' and $_ ne '..' } readdir $dir_iter;
    foreach my $symlink (@links) {
        diag "- $symlink -> ".readlink("$libdir/$symlink");
    }
    closedir $dir_iter;
    is_deeply(\@links, \@libc_family,
        'the same libraries are captured when using --link-target');
}
like(readlink "$libdir/libc.so.6", qr{^/run/host\Q$libc\E$},
     '$libdir/libc.so.6 is a symlink to /run/host + realpath of libc.so.6');

ok(! system('rm', '-fr', $libdir));
mkdir($libdir);
run_ok([qw(bwrap --ro-bind / / --ro-bind /), $host,
        '--bind', $libdir, $libdir,
        qw(--dev-bind /dev /dev),
        $CAPSULE_CAPTURE_LIBS_TOOL,
        "--dest=$libdir",
        "--provider=$host", 'soname:libc.so.6'], '>&2');
{
    opendir(my $dir_iter, $libdir);
    my @links = sort grep { $_ ne '.' and $_ ne '..' } readdir $dir_iter;
    foreach my $symlink (@links) {
        diag "- $symlink -> ".readlink("$libdir/$symlink");
    }
    closedir $dir_iter;
    is_deeply(\@links, \@libc_family,
        'the same libraries are captured when using $host');
}
like(readlink "$libdir/libc.so.6", qr{^\Q$host\E\Q$libc\E$},
     '$libdir/libc.so.6 is a symlink to $host + realpath of libc.so.6');

ok(! system('rm', '-fr', $libdir));
mkdir($libdir);
run_ok([qw(bwrap --ro-bind / / --ro-bind /), $host,
        '--bind', $libdir, $libdir,
        qw(--dev-bind /dev /dev),
        $CAPSULE_CAPTURE_LIBS_TOOL, '--link-target=/run/host',
        "--dest=$libdir",
        "--provider=$host", 'soname:libc.so.6'], '>&2');
{
    opendir(my $dir_iter, $libdir);
    my @links = sort grep { $_ ne '.' and $_ ne '..' } readdir $dir_iter;
    foreach my $symlink (@links) {
        diag "- $symlink -> ".readlink("$libdir/$symlink");
    }
    closedir $dir_iter;
    is_deeply(\@links, \@libc_family,
        'the same libraries are captured when using $host and --link-target');
}
like(readlink "$libdir/libc.so.6", qr{^/run/host\Q$libc\E$},
     '$libdir/libc.so.6 is a symlink to /run/host + realpath of libc.so.6');

ok(! system('rm', '-fr', $libdir));
mkdir($libdir);
run_ok([qw(bwrap --ro-bind / / --ro-bind /), $host,
        '--bind', $libdir, $libdir,
        qw(--dev-bind /dev /dev),
        $CAPSULE_CAPTURE_LIBS_TOOL, '--link-target=/',
        "--dest=$libdir", "--provider=$host",
        'soname-match:libjp*g.so.6*'], '>&2');
{
    opendir(my $dir_iter, $libdir);
    foreach my $symlink (readdir $dir_iter) {
        next if $symlink eq '.' || $symlink eq '..';
        diag "- $symlink -> ".readlink("$libdir/$symlink");
    }
    closedir $dir_iter;
}
like(readlink "$libdir/libjpeg.so.62", qr{^$LIBDIR/libjpeg\.so\.62(?:[0-9.]+)$},
     '$libdir/libjpeg.so.62 is a symlink to /run/host + realpath of libjpeg-6b');

ok(! system('rm', '-fr', $libdir));
mkdir($libdir);
my $stderr;
my $result = run_verbose([qw(bwrap --ro-bind / / --ro-bind /), $host,
                          '--bind', $libdir, $libdir,
                          qw(--dev-bind /dev /dev),
                          $CAPSULE_CAPTURE_LIBS_TOOL, '--link-target=/',
                          "--dest=$libdir", "--provider=$host",
                          'soname-match:this*library*does?not?exist'],
                         '2>', \$stderr, '>&2');
ok(! $result, 'a non-matching wildcard should fail');
like($stderr, qr{"this\*library\*does\?not\?exist"});

ok(! system('rm', '-fr', $libdir));
mkdir($libdir);
run_ok([qw(bwrap --ro-bind / / --ro-bind /), $host,
        '--bind', $libdir, $libdir,
        qw(--dev-bind /dev /dev),
        $CAPSULE_CAPTURE_LIBS_TOOL, '--link-target=/',
        "--dest=$libdir", "--provider=$host",
        'if-exists:soname-match:this*library*does?not?exist']);

# libgobject-2.0.so.0 depends on libglib-2.0.so.0 so normally,
# capturing GObject captures GLib
ok(! system('rm', '-fr', $libdir));
mkdir($libdir);
run_ok([qw(bwrap --ro-bind / / --ro-bind /), $host,
        '--bind', $libdir, $libdir,
        qw(--dev-bind /dev /dev),
        $CAPSULE_CAPTURE_LIBS_TOOL, '--link-target=/',
        "--dest=$libdir", "--provider=$host",
        'soname:libgobject-2.0.so.0']);
{
    opendir(my $dir_iter, $libdir);
    foreach my $symlink (readdir $dir_iter) {
        next if $symlink eq '.' || $symlink eq '..';
        diag "- $symlink -> ".readlink("$libdir/$symlink");
    }
    closedir $dir_iter;
}
like(readlink "$libdir/libgobject-2.0.so.0",
     qr{^$LIBDIR/libgobject-2\.0\.so\.0(?:[0-9.]+)$},
     '$libdir/libgobject-2.0.so.0 is captured');
like(readlink "$libdir/libglib-2.0.so.0",
     qr{^$LIBDIR/libglib-2\.0\.so\.0(?:[0-9.]+)$},
     '$libdir/libglib-2.0.so.0 is captured as a dependency');

# only-dependencies: captures the dependency, GLib but not the
# dependent library, GObject
ok(! system('rm', '-fr', $libdir));
mkdir($libdir);
run_ok([qw(bwrap --ro-bind / / --ro-bind /), $host,
        '--bind', $libdir, $libdir,
        qw(--dev-bind /dev /dev),
        $CAPSULE_CAPTURE_LIBS_TOOL, '--link-target=/',
        "--dest=$libdir", "--provider=$host",
        'only-dependencies:soname:libgobject-2.0.so.0']);
{
    opendir(my $dir_iter, $libdir);
    foreach my $symlink (readdir $dir_iter) {
        next if $symlink eq '.' || $symlink eq '..';
        diag "- $symlink -> ".readlink("$libdir/$symlink");
    }
    closedir $dir_iter;
}
ok(! -e "$libdir/libgobject-2.0.so.0",
   'only-dependencies: does not capture the library itself');
like(readlink "$libdir/libglib-2.0.so.0",
     qr{^$LIBDIR/libglib-2\.0\.so\.0(?:[0-9.]+)$},
     '$libdir/libglib-2.0.so.0 is captured as a dependency');

# no-dependencies: captures the dependent library, GObject but not the
# dependency, GLib (not amazingly useful)
ok(! system('rm', '-fr', $libdir));
mkdir($libdir);
run_ok([qw(bwrap --ro-bind / / --ro-bind /), $host,
        '--bind', $libdir, $libdir,
        qw(--dev-bind /dev /dev),
        $CAPSULE_CAPTURE_LIBS_TOOL, '--link-target=/',
        "--dest=$libdir", "--provider=$host",
        'no-dependencies:soname:libgobject-2.0.so.0']);
{
    opendir(my $dir_iter, $libdir);
    foreach my $symlink (readdir $dir_iter) {
        next if $symlink eq '.' || $symlink eq '..';
        diag "- $symlink -> ".readlink("$libdir/$symlink");
    }
    closedir $dir_iter;
}
like(readlink "$libdir/libgobject-2.0.so.0",
     qr{^$LIBDIR/libgobject-2\.0\.so\.0(?:[0-9.]+)$},
     '$libdir/libgobject-2.0.so.0 is captured');
ok(! -e "$libdir/libglib-2.0.so.0",
   'no-dependencies: does not capture libglib-2.0.so.0');

# only-dependencies:no-dependencies: (either way round) is completely useless
ok(! system('rm', '-fr', $libdir));
mkdir($libdir);
my $result = run_verbose([qw(bwrap --ro-bind / / --ro-bind /), $host,
                          '--bind', $libdir, $libdir,
                          qw(--dev-bind /dev /dev),
                          $CAPSULE_CAPTURE_LIBS_TOOL, '--link-target=/',
                          "--dest=$libdir", "--provider=$host",
                          'only-dependencies:no-dependencies:libglib-2.0.so.0'],
                         '>&2');
ok(! $result, 'only-dependencies:no-dependencies: is useless');
ok(! system('rm', '-fr', $libdir));
mkdir($libdir);
my $result = run_verbose([qw(bwrap --ro-bind / / --ro-bind /), $host,
                          '--bind', $libdir, $libdir,
                          qw(--dev-bind /dev /dev),
                          $CAPSULE_CAPTURE_LIBS_TOOL, '--link-target=/',
                          "--dest=$libdir", "--provider=$host",
                          'no-dependencies:only-dependencies:libglib-2.0.so.0'],
                         '>&2');
ok(! $result, 'no-dependencies:only-dependencies: is useless');

SKIP: {
    skip "not on Linux? Good luck!", 1 unless $^O eq 'linux';
    my $stdout;
    my $ld_so;

    run_ok([$CAPSULE_CAPTURE_LIBS_TOOL, '--print-ld.so'], '>', \$ld_so);
    chomp $ld_so;
    diag "ld.so is $ld_so";

    run_ok([$CAPSULE_CAPTURE_LIBS_TOOL, '--resolve-ld.so=/'], '>', \$stdout);

    is($stdout, abs_path($ld_so)."\n",
        '--resolve-ld.so=/ should print '.abs_path($ld_so));
    run_ok([qw(bwrap --ro-bind / / --ro-bind /), $host,
            $CAPSULE_CAPTURE_LIBS_TOOL, "--resolve-ld.so=$host"],
        '>', \$stdout);
    is($stdout, abs_path($ld_so)."\n",
        "--resolve-ld.so=$host should print ".abs_path($ld_so));
};

done_testing;

# vim:set sw=4 sts=4 et:
