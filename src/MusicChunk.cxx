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

#include "config.h"
#include "MusicChunk.hxx"
#include "AudioFormat.hxx"
#include "tag/Tag.hxx"

#include <assert.h>

MusicChunk::~MusicChunk()
{
	delete tag;
}

#ifndef NDEBUG
bool
MusicChunk::CheckFormat(const AudioFormat other_format) const
{
	assert(other_format.IsValid());

	return length == 0 || audio_format == other_format;
}
#endif

WritableBuffer<void>
MusicChunk::Write(const AudioFormat af,
		  float data_time, uint16_t _bit_rate)
{
	assert(CheckFormat(af));
	assert(length == 0 || audio_format.IsValid());

	if (length == 0) {
		/* if the chunk is empty, nobody has set bitRate and
		   times yet */

		bit_rate = _bit_rate;
		times = data_time;
	}

	const size_t frame_size = af.GetFrameSize();
	size_t num_frames = (sizeof(data) - length) / frame_size;
	if (num_frames == 0)
		return WritableBuffer<void>::Null();

#ifndef NDEBUG
	audio_format = af;
#endif

	return { data + length, num_frames * frame_size };
}

bool
MusicChunk::Expand(const AudioFormat af, size_t _length)
{
	const size_t frame_size = af.GetFrameSize();

	assert(length + _length <= sizeof(data));
	assert(audio_format == af);

	length += _length;

	return length + frame_size > sizeof(data);
}
