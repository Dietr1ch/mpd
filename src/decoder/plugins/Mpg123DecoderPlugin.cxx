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

#include "config.h" /* must be first for large file support */
#include "Mpg123DecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "CheckAudioFormat.hxx"
#include "tag/TagHandler.hxx"
#include "fs/Path.hxx"
#include "util/Error.hxx"
#include "util/Domain.hxx"
#include "Log.hxx"

#include <mpg123.h>

#include <stdio.h>

static constexpr Domain mpg123_domain("mpg123");

static bool
mpd_mpg123_init(gcc_unused const config_param &param)
{
	mpg123_init();

	return true;
}

static void
mpd_mpg123_finish(void)
{
	mpg123_exit();
}

/**
 * Opens a file with an existing #mpg123_handle.
 *
 * @param handle a handle which was created before; on error, this
 * function will not free it
 * @param audio_format this parameter is filled after successful
 * return
 * @return true on success
 */
static bool
mpd_mpg123_open(mpg123_handle *handle, const char *path_fs,
		AudioFormat &audio_format)
{
	int error;
	int channels, encoding;
	long rate;

	/* mpg123_open() wants a writable string :-( */
	char *const path2 = const_cast<char *>(path_fs);

	error = mpg123_open(handle, path2);
	if (error != MPG123_OK) {
		FormatWarning(mpg123_domain,
			      "libmpg123 failed to open %s: %s",
			      path_fs, mpg123_plain_strerror(error));
		return false;
	}

	/* obtain the audio format */

	error = mpg123_getformat(handle, &rate, &channels, &encoding);
	if (error != MPG123_OK) {
		FormatWarning(mpg123_domain,
			      "mpg123_getformat() failed: %s",
			      mpg123_plain_strerror(error));
		return false;
	}

	if (encoding != MPG123_ENC_SIGNED_16) {
		/* other formats not yet implemented */
		FormatWarning(mpg123_domain,
			      "expected MPG123_ENC_SIGNED_16, got %d",
			      encoding);
		return false;
	}

	Error error2;
	if (!audio_format_init_checked(audio_format, rate, SampleFormat::S16,
				       channels, error2)) {
		LogError(error2);
		return false;
	}

	return true;
}

static void
mpd_mpg123_file_decode(Decoder &decoder, Path path_fs)
{
	mpg123_handle *handle;
	int error;
	off_t num_samples;
	struct mpg123_frameinfo info;

	/* open the file */

	handle = mpg123_new(nullptr, &error);
	if (handle == nullptr) {
		FormatError(mpg123_domain,
			    "mpg123_new() failed: %s",
			    mpg123_plain_strerror(error));
		return;
	}

	AudioFormat audio_format;
	if (!mpd_mpg123_open(handle, path_fs.c_str(), audio_format)) {
		mpg123_delete(handle);
		return;
	}

	num_samples = mpg123_length(handle);

	/* tell MPD core we're ready */

	decoder_initialized(decoder, audio_format, true,
			    (float)num_samples /
			    (float)audio_format.sample_rate);

	if (mpg123_info(handle, &info) != MPG123_OK) {
		info.vbr = MPG123_CBR;
		info.bitrate = 0;
	}

	switch (info.vbr) {
	case MPG123_ABR:
		info.bitrate = info.abr_rate;
		break;
	case MPG123_CBR:
		break;
	default:
		info.bitrate = 0;
	}

	/* the decoder main loop */

	DecoderCommand cmd;
	do {
		unsigned char buffer[8192];
		size_t nbytes;

		/* decode */

		error = mpg123_read(handle, buffer, sizeof(buffer), &nbytes);
		if (error != MPG123_OK) {
			if (error != MPG123_DONE)
				FormatWarning(mpg123_domain,
					      "mpg123_read() failed: %s",
					      mpg123_plain_strerror(error));
			break;
		}

		/* update bitrate for ABR/VBR */
		if (info.vbr != MPG123_CBR) {
			/* FIXME: maybe skip, as too expensive? */
			/* FIXME: maybe, (info.vbr == MPG123_VBR) ? */
			if (mpg123_info (handle, &info) != MPG123_OK)
				info.bitrate = 0;
		}

		/* send to MPD */

		cmd = decoder_data(decoder, nullptr, buffer, nbytes, info.bitrate);

		if (cmd == DecoderCommand::SEEK) {
			off_t c = decoder_seek_where_frame(decoder);
			c = mpg123_seek(handle, c, SEEK_SET);
			if (c < 0)
				decoder_seek_error(decoder);
			else {
				decoder_command_finished(decoder);
				decoder_timestamp(decoder, c/(double)audio_format.sample_rate);
			}

			cmd = DecoderCommand::NONE;
		}
	} while (cmd == DecoderCommand::NONE);

	/* cleanup */

	mpg123_delete(handle);
}

static bool
mpd_mpg123_scan_file(Path path_fs,
		     const struct tag_handler *handler, void *handler_ctx)
{
	mpg123_handle *handle;
	int error;
	off_t num_samples;

	handle = mpg123_new(nullptr, &error);
	if (handle == nullptr) {
		FormatError(mpg123_domain,
			    "mpg123_new() failed: %s",
			    mpg123_plain_strerror(error));
		return false;
	}

	AudioFormat audio_format;
	if (!mpd_mpg123_open(handle, path_fs.c_str(), audio_format)) {
		mpg123_delete(handle);
		return false;
	}

	num_samples = mpg123_length(handle);
	if (num_samples <= 0) {
		mpg123_delete(handle);
		return false;
	}

	/* ID3 tag support not yet implemented */

	mpg123_delete(handle);

	tag_handler_invoke_duration(handler, handler_ctx,
				    num_samples / audio_format.sample_rate);
	return true;
}

static const char *const mpg123_suffixes[] = {
	"mp3",
	nullptr
};

const struct DecoderPlugin mpg123_decoder_plugin = {
	"mpg123",
	mpd_mpg123_init,
	mpd_mpg123_finish,
	/* streaming not yet implemented */
	nullptr,
	mpd_mpg123_file_decode,
	mpd_mpg123_scan_file,
	nullptr,
	nullptr,
	mpg123_suffixes,
	nullptr,
};
