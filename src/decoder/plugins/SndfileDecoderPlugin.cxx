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
#include "SndfileDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "input/InputStream.hxx"
#include "CheckAudioFormat.hxx"
#include "tag/TagHandler.hxx"
#include "fs/Path.hxx"
#include "util/Error.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <sndfile.h>

static constexpr Domain sndfile_domain("sndfile");

static bool
sndfile_init(gcc_unused const config_param &param)
{
       LogDebug(sndfile_domain, sf_version_string());
       return true;
}

struct SndfileInputStream {
	Decoder *const decoder;
	InputStream &is;

	size_t Read(void *buffer, size_t size) {
		/* libsndfile chokes on partial reads; therefore
		   always force full reads */
		return decoder_read_full(decoder, is, buffer, size)
			? size
			: 0;
	}
};

static sf_count_t
sndfile_vio_get_filelen(void *user_data)
{
	SndfileInputStream &sis = *(SndfileInputStream *)user_data;
	const InputStream &is = sis.is;

	if (!is.KnownSize())
		return -1;

	return is.GetSize();
}

static sf_count_t
sndfile_vio_seek(sf_count_t _offset, int whence, void *user_data)
{
	SndfileInputStream &sis = *(SndfileInputStream *)user_data;
	InputStream &is = sis.is;

	offset_type offset = _offset;
	switch (whence) {
	case SEEK_SET:
		break;

	case SEEK_CUR:
		offset += is.GetOffset();
		break;

	case SEEK_END:
		if (!is.KnownSize())
			return -1;

		offset += is.GetSize();
		break;

	default:
		return -1;
	}

	Error error;
	if (!is.LockSeek(offset, error)) {
		LogError(error, "Seek failed");
		return -1;
	}

	return is.GetOffset();
}

static sf_count_t
sndfile_vio_read(void *ptr, sf_count_t count, void *user_data)
{
	SndfileInputStream &sis = *(SndfileInputStream *)user_data;

	return sis.Read(ptr, count);
}

static sf_count_t
sndfile_vio_write(gcc_unused const void *ptr,
		  gcc_unused sf_count_t count,
		  gcc_unused void *user_data)
{
	/* no writing! */
	return -1;
}

static sf_count_t
sndfile_vio_tell(void *user_data)
{
	SndfileInputStream &sis = *(SndfileInputStream *)user_data;
	const InputStream &is = sis.is;

	return is.GetOffset();
}

/**
 * This SF_VIRTUAL_IO implementation wraps MPD's #input_stream to a
 * libsndfile stream.
 */
static SF_VIRTUAL_IO vio = {
	sndfile_vio_get_filelen,
	sndfile_vio_seek,
	sndfile_vio_read,
	sndfile_vio_write,
	sndfile_vio_tell,
};

/**
 * Converts a frame number to a timestamp (in seconds).
 */
static float
frame_to_time(sf_count_t frame, const AudioFormat *audio_format)
{
	return (float)frame / (float)audio_format->sample_rate;
}

static void
sndfile_stream_decode(Decoder &decoder, InputStream &is)
{
	SF_INFO info;

	info.format = 0;

	SndfileInputStream sis{&decoder, is};
	SNDFILE *const sf = sf_open_virtual(&vio, SFM_READ, &info, &sis);
	if (sf == nullptr) {
		LogWarning(sndfile_domain, "sf_open_virtual() failed");
		return;
	}

	/* for now, always read 32 bit samples.  Later, we could lower
	   MPD's CPU usage by reading 16 bit samples with
	   sf_readf_short() on low-quality source files. */
	Error error;
	AudioFormat audio_format;
	if (!audio_format_init_checked(audio_format, info.samplerate,
				       SampleFormat::S32,
				       info.channels, error)) {
		LogError(error);
		return;
	}

	decoder_initialized(decoder, audio_format, info.seekable,
			    frame_to_time(info.frames, &audio_format));

	int buffer[4096];

	const size_t frame_size = audio_format.GetFrameSize();
	const sf_count_t read_frames = sizeof(buffer) / frame_size;

	DecoderCommand cmd;
	do {
		sf_count_t num_frames = sf_readf_int(sf, buffer, read_frames);
		if (num_frames <= 0)
			break;

		cmd = decoder_data(decoder, is,
				   buffer, num_frames * frame_size,
				   0);
		if (cmd == DecoderCommand::SEEK) {
			sf_count_t c = decoder_seek_where_frame(decoder);
			c = sf_seek(sf, c, SEEK_SET);
			if (c < 0)
				decoder_seek_error(decoder);
			else
				decoder_command_finished(decoder);
			cmd = DecoderCommand::NONE;
		}
	} while (cmd == DecoderCommand::NONE);

	sf_close(sf);
}

static void
sndfile_handle_tag(SNDFILE *sf, int str, TagType tag,
		   const struct tag_handler *handler, void *handler_ctx)
{
	const char *value = sf_get_string(sf, str);
	if (value != nullptr)
		tag_handler_invoke_tag(handler, handler_ctx, tag, value);
}

static constexpr struct {
	int8_t str;
	TagType tag;
} sndfile_tags[] = {
	{ SF_STR_TITLE, TAG_TITLE },
	{ SF_STR_ARTIST, TAG_ARTIST },
	{ SF_STR_COMMENT, TAG_COMMENT },
	{ SF_STR_DATE, TAG_DATE },
	{ SF_STR_ALBUM, TAG_ALBUM },
	{ SF_STR_TRACKNUMBER, TAG_TRACK },
	{ SF_STR_GENRE, TAG_GENRE },
};

static bool
sndfile_scan_stream(InputStream &is,
		    const struct tag_handler *handler, void *handler_ctx)
{
	SF_INFO info;

	info.format = 0;

	SndfileInputStream sis{nullptr, is};
	SNDFILE *const sf = sf_open_virtual(&vio, SFM_READ, &info, &sis);
	if (sf == nullptr)
		return false;

	if (!audio_valid_sample_rate(info.samplerate)) {
		sf_close(sf);
		FormatWarning(sndfile_domain,
			      "Invalid sample rate in %s", is.GetURI());
		return false;
	}

	tag_handler_invoke_duration(handler, handler_ctx,
				    info.frames / info.samplerate);

	for (auto i : sndfile_tags)
		sndfile_handle_tag(sf, i.str, i.tag, handler, handler_ctx);

	sf_close(sf);

	return true;
}

static const char *const sndfile_suffixes[] = {
	"wav", "aiff", "aif", /* Microsoft / SGI / Apple */
	"au", "snd", /* Sun / DEC / NeXT */
	"paf", /* Paris Audio File */
	"iff", "svx", /* Commodore Amiga IFF / SVX */
	"sf", /* IRCAM */
	"voc", /* Creative */
	"w64", /* Soundforge */
	"pvf", /* Portable Voice Format */
	"xi", /* Fasttracker */
	"htk", /* HMM Tool Kit */
	"caf", /* Apple */
	"sd2", /* Sound Designer II */

	/* libsndfile also supports FLAC and Ogg Vorbis, but only by
	   linking with libFLAC and libvorbis - we can do better, we
	   have native plugins for these libraries */

	nullptr
};

static const char *const sndfile_mime_types[] = {
	"audio/x-wav",
	"audio/x-aiff",

	/* what are the MIME types of the other supported formats? */

	nullptr
};

const struct DecoderPlugin sndfile_decoder_plugin = {
	"sndfile",
	sndfile_init,
	nullptr,
	sndfile_stream_decode,
	nullptr,
	nullptr,
	sndfile_scan_stream,
	nullptr,
	sndfile_suffixes,
	sndfile_mime_types,
};
