/*
 * Common code for portal-like services
 *
 * Copyright © 2018 Red Hat, Inc.
 * Copyright © 2020 Collabora Ltd.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Based on xdg-desktop-portal, flatpak-portal and flatpak-spawn.
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#pragma once

#include <stdio.h>
#include <unistd.h>

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>

#include "steam-runtime-tools/glib-backports-internal.h"
#include "libglnx/libglnx.h"

typedef struct _PvPortalListener PvPortalListener;
typedef struct _PvPortalListenerClass PvPortalListenerClass;

#define PV_TYPE_PORTAL_LISTENER (pv_portal_listener_get_type ())
#define PV_PORTAL_LISTENER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_PORTAL_LISTENER, PvPortalListener))
#define PV_PORTAL_LISTENER_CLASS(cls) (G_TYPE_CHECK_CLASS_CAST ((cls), PV_TYPE_PORTAL_LISTENER, PvPortalListenerClass))
#define PV_IS_PORTAL_LISTENER(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_PORTAL_LISTENER))
#define PV_IS_PORTAL_LISTENER_CLASS(cls) (G_TYPE_CHECK_CLASS_TYPE ((cls), PV_TYPE_PORTAL_LISTENER))
#define PV_PORTAL_LISTENER_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), PV_TYPE_PORTAL_LISTENER, PvPortalListenerClass)
GType pv_portal_listener_get_type (void);

struct _PvPortalListener
{
  GObject parent;
};

PvPortalListener *pv_portal_listener_new (void);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PvPortalListener, g_object_unref)
