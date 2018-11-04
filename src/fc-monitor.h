/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2017 Jan Alexander Steffens (heftig) <jan.steffens@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */
#ifndef FC_MONITOR_H
#define FC_MONITOR_H

#include <glib-object.h>

G_BEGIN_DECLS

#define FC_TYPE_MONITOR (fc_monitor_get_type ())
G_DECLARE_FINAL_TYPE (FcMonitor, fc_monitor, FC, MONITOR, GObject)

FcMonitor *fc_monitor_new (void);

void fc_monitor_start (FcMonitor *monitor);
void fc_monitor_stop  (FcMonitor *monitor);

G_END_DECLS

#endif /* FC_MONITOR_H */
