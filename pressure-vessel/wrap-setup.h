/*
 * Copyright © 2014-2019 Red Hat, Inc
 * Copyright © 2017-2021 Collabora Ltd.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <glib.h>

#include "environ.h"

#include "flatpak-bwrap-private.h"
#include "flatpak-exports-private.h"

void pv_wrap_share_sockets (FlatpakBwrap *bwrap,
                            PvEnviron *container_env,
                            gboolean using_a_runtime,
                            gboolean is_flatpak_env);

gboolean pv_wrap_use_host_os (FlatpakExports *exports,
                              FlatpakBwrap *bwrap,
                              GError **error);