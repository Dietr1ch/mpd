/*
 * Copyright (C) 2003-2014 The Music Player Daemon Project
 * http://www.musicpd.org
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPD_PCM_DSD_USB_HXX
#define MPD_PCM_DSD_USB_HXX

#include "check.h"

#include <stdint.h>
#include <stddef.h>

class PcmBuffer;
template<typename T> struct ConstBuffer;

/**
 * Pack DSD 1 bit samples into (padded) 24 bit PCM samples for
 * playback over USB, according to the proposed standard by 
 * dCS and others:
 * http://www.sonore.us/DoP_openStandard_1v1.pdf
 */
ConstBuffer<uint32_t>
pcm_dsd_to_usb(PcmBuffer &buffer, unsigned channels,
	       ConstBuffer<uint8_t> src);

#endif
