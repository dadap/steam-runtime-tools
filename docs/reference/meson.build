# Copyright © 2019 Collabora Ltd.
#
# SPDX-License-Identifier: MIT
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
# CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

version_conf = configuration_data()
version_conf.set('PACKAGE_VERSION', meson.project_version())
configure_file(
  input: 'version.xml.in',
  output: 'version.xml',
  configuration: version_conf
)

gnome.gtkdoc(
  'steam-runtime-tools-0',
  main_xml : 'steam-runtime-tools.xml',
  namespace : 'srt',
  mode : 'none',
  src_dir : ['steam-runtime-tools'],
  dependencies : libsteamrt_dep,
  install: true,
  scan_args : [
    '--rebuild-sections',
    '--rebuild-types',
  ],
)

# vim:set sw=2 sts=2 et:
