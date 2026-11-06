/*
 * Categorized, redacted and rate-limited driver diagnostics.
 *
 * Every driver message is a record with a stable category (see
 * docs/DIAGNOSTICS.md), the operation that failed, an errno name and the
 * decoder context it belongs to. The default text output keeps the historic
 * "libva-v4l2request: <message>" lines; LIBVA_V4L2_DIAG=json switches to one
 * JSON object per line with the structured fields and debug-level records.
 *
 * Messages never carry bitstream bytes. As a second line of defence against a
 * future call site leaking user data, absolute paths outside /dev and /sys
 * and URLs are replaced before output, control and non-ASCII bytes are
 * masked, and messages are truncated to V4L2R_DIAG_MSG_MAX bytes.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#include <va/va_str.h>

#include "v4l2_request.h"

/* Rate limit: at most BURST records per category in each WINDOW. */
#define V4L2R_DIAG_WINDOW_NS	(10ull * 1000000000ull)
#define V4L2R_DIAG_BURST	20
#define V4L2R_DIAG_RECORD_MAX	1024

static const char *const category_names[V4L2R_DIAG_NB_CATEGORIES] = {
	[V4L2R_DIAG_INFO] = "info",
	[V4L2R_DIAG_UNSUPPORTED] = "unsupported",
	[V4L2R_DIAG_CLIENT] = "client",
	[V4L2R_DIAG_BITSTREAM] = "bitstream",
	[V4L2R_DIAG_REFERENCE] = "reference",
	[V4L2R_DIAG_ALLOCATION] = "allocation",
	[V4L2R_DIAG_TIMEOUT] = "timeout",
	[V4L2R_DIAG_KERNEL] = "kernel",
	[V4L2R_DIAG_DECODER] = "decoder",
	[V4L2R_DIAG_DEVICE] = "device",
};

static const char *const level_names[] = {
	[V4L2R_DIAG_LEVEL_ERROR] = "error",
	[V4L2R_DIAG_LEVEL_WARNING] = "warning",
	[V4L2R_DIAG_LEVEL_INFO] = "info",
	[V4L2R_DIAG_LEVEL_DEBUG] = "debug",
};

static struct {
	pthread_mutex_t mutex;
	bool initialized;
	enum v4l2r_diag_mode mode;
	FILE *sink;			/* NULL = stderr */
	uint64_t (*clock_ns)(void);
	char run[17];
	uint64_t seq;
	uint64_t start_ns;
	/* Per category; [0] error and warning, [1] debug. Info records mark
	 * lifecycle events (a few per context) and are never limited, so the
	 * text lines scripts look for cannot be suppressed. */
	struct {
		uint64_t window_start;
		unsigned int emitted;
		unsigned int suppressed;
	} limit[V4L2R_DIAG_NB_CATEGORIES][2];
} diag = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
};

/* Process-wide context serials, never reused (VAContextIDs are). */
static uint32_t next_context_serial = 1;
static pthread_mutex_t serial_mutex = PTHREAD_MUTEX_INITIALIZER;

const char *v4l2r_diag_category_name(enum v4l2r_diag_category category)
{
	if ((unsigned int)category >= V4L2R_DIAG_NB_CATEGORIES)
		return "invalid";
	return category_names[category];
}

enum v4l2r_diag_category v4l2r_diag_errno_category(int err)
{
	switch (err < 0 ? -err : err) {
	case ETIMEDOUT:
		return V4L2R_DIAG_TIMEOUT;
	case ENOMEM:
	case ENOSPC:
	case ENOBUFS:
		return V4L2R_DIAG_ALLOCATION;
	case ENODEV:
	case EPIPE:
	case ENXIO:
		return V4L2R_DIAG_DEVICE;
	default:
		return V4L2R_DIAG_KERNEL;
	}
}

static const char *errno_name(int err, char *buf, size_t size)
{
	static const struct { int value; const char *name; } names[] = {
		{ EPERM, "EPERM" }, { ENOENT, "ENOENT" }, { EINTR, "EINTR" },
		{ EIO, "EIO" }, { ENXIO, "ENXIO" }, { EBADF, "EBADF" },
		{ EAGAIN, "EAGAIN" }, { ENOMEM, "ENOMEM" }, { EACCES, "EACCES" },
		{ EFAULT, "EFAULT" }, { EBUSY, "EBUSY" }, { EEXIST, "EEXIST" },
		{ ENODEV, "ENODEV" }, { EINVAL, "EINVAL" }, { ENOSPC, "ENOSPC" },
		{ EPIPE, "EPIPE" }, { ERANGE, "ERANGE" }, { ENOSYS, "ENOSYS" },
		{ ENOTTY, "ENOTTY" }, { ETIMEDOUT, "ETIMEDOUT" },
		{ EOVERFLOW, "EOVERFLOW" }, { ENOBUFS, "ENOBUFS" },
		{ ECANCELED, "ECANCELED" }, { EPROTO, "EPROTO" },
		{ EOPNOTSUPP, "EOPNOTSUPP" }, { EMSGSIZE, "EMSGSIZE" },
	};

	err = err < 0 ? -err : err;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (names[i].value == err)
			return names[i].name;
	snprintf(buf, size, "errno-%d", err);
	return buf;
}

static uint64_t default_clock_ns(void)
{
	return v4l2r_now_ns();
}

/* Called with diag.mutex held. */
static void diag_init_locked(void)
{
	uint64_t run;
	const char *mode;

	if (diag.initialized)
		return;
	diag.initialized = true;

	if (!diag.clock_ns)
		diag.clock_ns = default_clock_ns;
	diag.start_ns = diag.clock_ns();

	/* A random run identifier tells apart records from several processes
	 * (and several libva displays in one) interleaved in one log. */
	if (getrandom(&run, sizeof(run), GRND_NONBLOCK) != sizeof(run))
		run = diag.start_ns ^ ((uint64_t)getpid() << 32);
	snprintf(diag.run, sizeof(diag.run), "%016" PRIx64, run);

	mode = getenv("LIBVA_V4L2_DIAG");
	if (mode && !strcmp(mode, "json"))
		diag.mode = V4L2R_DIAG_MODE_JSON;
	else
		diag.mode = V4L2R_DIAG_MODE_TEXT;
	if (mode && strcmp(mode, "json") && strcmp(mode, "text") && *mode) {
		/* Do not echo the value: it is arbitrary environment input. */
		FILE *out = diag.sink ? diag.sink : stderr;
		fputs("libva-v4l2request: unknown LIBVA_V4L2_DIAG value; using text\n",
		      out);
	}
}

void v4l2r_diag_configure(const struct v4l2r_diag_options *options)
{
	pthread_mutex_lock(&diag.mutex);
	memset(diag.limit, 0, sizeof(diag.limit));
	diag.seq = 0;
	diag.initialized = false;
	diag.sink = options ? options->sink : NULL;
	diag.clock_ns = options ? options->clock_ns : NULL;
	diag_init_locked();
	if (options && options->mode)
		diag.mode = options->mode;
	pthread_mutex_unlock(&diag.mutex);
}

void v4l2r_diag_run_id(char out[17])
{
	pthread_mutex_lock(&diag.mutex);
	diag_init_locked();
	memcpy(out, diag.run, 17);
	pthread_mutex_unlock(&diag.mutex);
}

uint32_t v4l2r_diag_context_serial(void)
{
	uint32_t serial;

	pthread_mutex_lock(&serial_mutex);
	serial = next_context_serial++;
	if (!next_context_serial)
		next_context_serial = 1;
	pthread_mutex_unlock(&serial_mutex);

	return serial;
}

static bool token_end(char c)
{
	return !c || c == ' ' || c == '\t' || c == '\n' || c == ')' ||
	       c == ']' || c == '"' || c == '\'' || c == ',' || c == ';' ||
	       c == '>';
}

static bool word_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
	       c == '~';
}

/* Device nodes and sysfs attributes identify hardware, not the user. */
static bool path_allowed(const char *path, size_t len)
{
	if (len < 6 || (strncmp(path, "/dev/", 5) && strncmp(path, "/sys/", 5)))
		return false;
	return !memmem(path, len, "/..", 3);
}

/*
 * Copy src into dst, replacing private paths and URLs, masking control bytes
 * and truncating to V4L2R_DIAG_MSG_MAX. A path is any '/' not preceded by a
 * word character ("384x288/2" is not, "open:/home/x" is), up to a separator;
 * a URL is a word containing "://" or starting with "file:". With ascii_only,
 * bytes above 0x7e are masked too, keeping JSON output valid UTF-8; text mode
 * passes them through so localized strerror() text stays readable. A trailing
 * newline is dropped; the output formats add their own.
 */
size_t v4l2r_diag_redact(char *dst, size_t size, const char *src,
			 bool ascii_only)
{
	size_t out = 0, limit = size ? size - 1 : 0;
	bool truncated = false;

	if (limit > V4L2R_DIAG_MSG_MAX)
		limit = V4L2R_DIAG_MSG_MAX;

	for (size_t i = 0; src[i]; ) {
		const char *replacement = NULL;
		bool word_start = i == 0 || !word_char(src[i - 1]);
		size_t len = 0;

		if (word_start) {
			while (!token_end(src[i + len]))
				len++;
			if (src[i] == '/' && len > 1 &&
			    !path_allowed(src + i, len))
				replacement = "<path>";
			else if (word_char(src[i]) &&
				 (memmem(src + i, len, "://", 3) ||
				  (len > 5 && !strncmp(src + i, "file:", 5))))
				replacement = "<url>";
		}

		if (replacement) {
			size_t rlen = strlen(replacement);

			if (out + rlen > limit) {
				truncated = true;
				break;
			}
			memcpy(dst + out, replacement, rlen);
			out += rlen;
			i += len;
			continue;
		}

		if (src[i] == '\n' && !src[i + 1])
			break;
		if (out >= limit) {
			truncated = true;
			break;
		}
		unsigned char c = (unsigned char)src[i];
		dst[out++] = (c < 0x20 || c == 0x7f || (ascii_only && c > 0x7f)) ?
			     '?' : (char)c;
		i++;
	}

	if (truncated && limit >= 3) {
		out = out > limit - 3 ? limit - 3 : out;
		memcpy(dst + out, "...", 3);
		out += 3;
	}
	if (size)
		dst[out] = '\0';

	return out;
}

/* Append a JSON string literal of an already-sanitized ASCII string. */
static void json_string(char *buf, size_t size, size_t *pos, const char *s)
{
	size_t p = *pos;

	if (p + 1 < size)
		buf[p++] = '"';
	for (; *s && p + 7 < size; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			buf[p++] = '\\';
			buf[p++] = c;
		} else if (c < 0x20 || c >= 0x7f) {
			p += snprintf(buf + p, size - p, "\\u%04x", c);
		} else {
			buf[p++] = c;
		}
	}
	if (p + 1 < size)
		buf[p++] = '"';
	buf[p] = '\0';
	*pos = p;
}

static void json_append(char *buf, size_t size, size_t *pos,
			const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void json_append(char *buf, size_t size, size_t *pos,
			const char *fmt, ...)
{
	va_list args;
	int n;

	if (*pos >= size)
		return;
	va_start(args, fmt);
	n = vsnprintf(buf + *pos, size - *pos, fmt, args);
	va_end(args);
	if (n > 0)
		*pos = (size_t)n < size - *pos ? *pos + n : size - 1;
}

static void json_key_string(char *buf, size_t size, size_t *pos,
			    const char *key, const char *value)
{
	char clean[V4L2R_DIAG_MSG_MAX + 1];

	v4l2r_diag_redact(clean, sizeof(clean), value, true);
	json_append(buf, size, pos, ",\"%s\":", key);
	json_string(buf, size, pos, clean);
}

/* Header common to every JSON record. Called with diag.mutex held. */
static void json_header(char *buf, size_t size, size_t *pos, uint64_t now,
			enum v4l2r_diag_level level,
			enum v4l2r_diag_category category, const char *op)
{
	uint64_t ms = (now - diag.start_ns) / 1000000;

	json_append(buf, size, pos,
		    "{\"schema\":\"%s\",\"run\":\"%s\",\"seq\":%" PRIu64
		    ",\"t_ms\":%" PRIu64 ",\"level\":\"%s\",\"category\":\"%s\"",
		    V4L2R_DIAG_SCHEMA, diag.run, ++diag.seq, ms,
		    level_names[level], category_names[category]);
	if (op)
		json_append(buf, size, pos, ",\"op\":\"%s\"", op);
}

static void emit_locked(const char *line, size_t len)
{
	FILE *out = diag.sink ? diag.sink : stderr;

	/* One write per record keeps lines from concurrent threads intact. */
	fwrite(line, 1, len, out);
	fflush(out);
}

/* Report records a limiter dropped in its last window. diag.mutex held. */
static void flush_suppressed_locked(enum v4l2r_diag_category category,
				    unsigned int class, uint64_t now)
{
	char line[V4L2R_DIAG_RECORD_MAX];
	unsigned int count = diag.limit[category][class].suppressed;
	size_t pos = 0;

	if (!count)
		return;
	diag.limit[category][class].suppressed = 0;

	if (diag.mode == V4L2R_DIAG_MODE_JSON) {
		json_header(line, sizeof(line), &pos, now,
			    V4L2R_DIAG_LEVEL_WARNING, category, "suppressed");
		json_append(line, sizeof(line), &pos,
			    ",\"suppressed\":%u,\"msg\":\"rate limit dropped "
			    "%u %s%s records\"}\n", count, count,
			    category_names[category], class ? " debug" : "");
	} else {
		json_append(line, sizeof(line), &pos,
			    "libva-v4l2request: suppressed %u similar %s "
			    "messages\n", count, category_names[category]);
	}
	emit_locked(line, pos);
}

/* Returns whether a record may be written now. diag.mutex held. */
static bool rate_allow_locked(enum v4l2r_diag_level level,
			      enum v4l2r_diag_category category, uint64_t now)
{
	unsigned int class = level == V4L2R_DIAG_LEVEL_DEBUG;
	typeof(diag.limit[0][0]) *limit = &diag.limit[category][class];

	if (level == V4L2R_DIAG_LEVEL_INFO)
		return true;

	if (!limit->window_start || now - limit->window_start >= V4L2R_DIAG_WINDOW_NS) {
		flush_suppressed_locked(category, class, now);
		limit->window_start = now ? now : 1;
		limit->emitted = 0;
	}

	if (limit->emitted >= V4L2R_DIAG_BURST) {
		limit->suppressed++;
		return false;
	}
	limit->emitted++;
	return true;
}

void v4l2r_diag(const struct v4l2r_context *ctx, enum v4l2r_diag_level level,
		enum v4l2r_diag_category category, const char *op, int err,
		const char *fmt, ...)
{
	char raw[V4L2R_DIAG_MSG_MAX * 2];
	char msg[V4L2R_DIAG_MSG_MAX + 1];
	char line[V4L2R_DIAG_RECORD_MAX];
	char errbuf[24];
	size_t pos = 0;
	uint64_t now;
	va_list args;

	if ((unsigned int)category >= V4L2R_DIAG_NB_CATEGORIES)
		category = V4L2R_DIAG_KERNEL;
	if ((unsigned int)level > V4L2R_DIAG_LEVEL_DEBUG)
		level = V4L2R_DIAG_LEVEL_ERROR;

	pthread_mutex_lock(&diag.mutex);
	diag_init_locked();
	if (diag.mode == V4L2R_DIAG_MODE_TEXT && level == V4L2R_DIAG_LEVEL_DEBUG) {
		pthread_mutex_unlock(&diag.mutex);
		return;
	}
	now = diag.clock_ns();
	if (!rate_allow_locked(level, category, now)) {
		pthread_mutex_unlock(&diag.mutex);
		return;
	}

	va_start(args, fmt);
	vsnprintf(raw, sizeof(raw), fmt, args);
	va_end(args);
	v4l2r_diag_redact(msg, sizeof(msg), raw,
			  diag.mode == V4L2R_DIAG_MODE_JSON);

	if (diag.mode == V4L2R_DIAG_MODE_TEXT) {
		json_append(line, sizeof(line), &pos, "libva-v4l2request: %s\n",
			    msg);
		emit_locked(line, pos);
		pthread_mutex_unlock(&diag.mutex);
		return;
	}

	json_header(line, sizeof(line), &pos, now, level, category, op);
	if (err)
		json_append(line, sizeof(line), &pos, ",\"errno\":\"%s\"",
			    errno_name(err, errbuf, sizeof(errbuf)));
	if (ctx) {
		if (ctx->diag_serial)
			json_append(line, sizeof(line), &pos, ",\"ctx\":%u",
				    ctx->diag_serial);
		if (ctx->id)
			json_append(line, sizeof(line), &pos,
				    ",\"va_context\":\"0x%08x\"", ctx->id);
		if (ctx->codec && ctx->codec->name)
			json_key_string(line, sizeof(line), &pos, "codec",
					ctx->codec->name);
		if (ctx->codec)
			json_key_string(line, sizeof(line), &pos, "profile",
					vaProfileStr(ctx->profile));
	}
	json_append(line, sizeof(line), &pos, ",\"msg\":");
	json_string(line, sizeof(line), &pos, msg);
	/* A record never exceeds the line buffer: the message is capped and the
	 * remaining fields are bounded, so the closing brace always fits. */
	json_append(line, sizeof(line), &pos, "}\n");
	emit_locked(line, pos);
	pthread_mutex_unlock(&diag.mutex);
}

static void fourcc_string(char out[5], uint32_t fourcc)
{
	for (unsigned int i = 0; i < 4; i++) {
		char c = (char)(fourcc >> (8 * i));
		out[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
	}
	out[4] = '\0';
}

void v4l2r_diag_driver(const struct v4l2r_driver *drv)
{
	char line[V4L2R_DIAG_RECORD_MAX * 4];
	static const char *const high10[] = {
		[V4L2R_H264_HIGH10_OFF] = "off",
		[V4L2R_H264_HIGH10_NATIVE] = "native",
		[V4L2R_H264_HIGH10_FFMPEG] = "ffmpeg",
	};
	size_t pos = 0;

	pthread_mutex_lock(&diag.mutex);
	diag_init_locked();
	if (diag.mode != V4L2R_DIAG_MODE_JSON) {
		pthread_mutex_unlock(&diag.mutex);
		return;
	}

	json_header(line, sizeof(line), &pos, diag.clock_ns(),
		    V4L2R_DIAG_LEVEL_DEBUG, V4L2R_DIAG_INFO, "driver-init");
	json_append(line, sizeof(line), &pos,
		    ",\"version\":\"%s\",\"va_api\":\"%d.%d.%d\","
		    "\"h264_high10\":\"%s\",\"decoders\":[",
		    V4L2R_VERSION, VA_MAJOR_VERSION, VA_MINOR_VERSION,
		    VA_MICRO_VERSION,
		    (unsigned int)drv->h264_high10 < 3 ? high10[drv->h264_high10] : "?");
	for (unsigned int i = 0; i < drv->nb_decoders; i++) {
		const struct v4l2r_decoder *decoder = &drv->decoders[i];
		char raw_card[sizeof(decoder->card) + 1];
		char card[V4L2R_DIAG_MSG_MAX + 1];

		/* The card is a kernel string; never trust it to be printable. */
		memcpy(raw_card, decoder->card, sizeof(decoder->card));
		raw_card[sizeof(decoder->card)] = '\0';
		v4l2r_diag_redact(card, sizeof(card), raw_card, true);
		json_append(line, sizeof(line), &pos, "%s{\"card\":",
			    i ? "," : "");
		json_string(line, sizeof(line), &pos, card);
		json_append(line, sizeof(line), &pos, ",\"hevc_10bit\":%s,"
			    "\"h264_10bit\":%s,\"vp9_10bit\":%s,"
			    "\"av1_10bit\":%s,\"formats\":[",
			    decoder->hevc_10bit ? "true" : "false",
			    decoder->h264_10bit ? "true" : "false",
			    decoder->vp9_10bit ? "true" : "false",
			    decoder->av1_10bit ? "true" : "false");
		for (unsigned int j = 0; j < decoder->nb_pixelformats; j++) {
			char fourcc[5];

			fourcc_string(fourcc, decoder->pixelformats[j]);
			json_append(line, sizeof(line), &pos, "%s", j ? "," : "");
			json_string(line, sizeof(line), &pos, fourcc);
		}
		json_append(line, sizeof(line), &pos, "]}");
	}
	json_append(line, sizeof(line), &pos, "],\"msg\":\"driver capabilities\"}\n");
	emit_locked(line, pos);
	pthread_mutex_unlock(&diag.mutex);
}

void v4l2r_diag_flush(void)
{
	pthread_mutex_lock(&diag.mutex);
	if (diag.initialized) {
		uint64_t now = diag.clock_ns();

		for (unsigned int i = 0; i < V4L2R_DIAG_NB_CATEGORIES; i++)
			for (unsigned int class = 0; class < 2; class++)
				flush_suppressed_locked(i, class, now);
	}
	pthread_mutex_unlock(&diag.mutex);
}
