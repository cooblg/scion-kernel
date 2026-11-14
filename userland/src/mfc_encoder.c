/*
 * H.264 and HEVC encoding for mediaserverd on the Exynos 9810's MFC (kernel: bsd/dev/hakios.exynos_mfc.c, /dev/hakios-mfcenc), part of
 * libhakios.media.dylib.
 *
 * Why: this iPhone's VideoToolbox has no H.264 encoder and only a software HEVC one (VCPHEVC.videocodec, registered for the
 * photographs' HEIC by inject_media.c). A video recording through that one took four to seven cores, fell behind, and at the stop
 * the movie was never finished (15 November 2026, 1080p and 720p alike).
 *
 * How: VideoToolbox's public compression API is interposed in this process. A session asked for H.264 ('avc1') is made as an HEVC
 * session underneath -- an object VideoToolbox knows, for the calls this file passes on (its pixel buffer pool, its properties) --
 * and every picture given to it is encoded by the block instead, synchronously: VTCompressionSessionEncodeFrame returns once the
 * picture's sample buffer has gone to the session's output callback. An HEVC ('hvc1') session is VideoToolbox's own, made as asked;
 * at its first picture it is taken for the block only if it is a video's -- real time asked for, a frame rate given, or a video's
 * size -- so the photographs' HEIC (CMPhoto) stays with VideoToolbox's encoder. Any session when /dev/hakios-mfcenc is missing or
 * busy is VideoToolbox's own, untouched.
 *
 * The block gives Annex B; the sample buffers carry length-prefixed NAL units (4 bytes) and a format made from its parameter sets.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "hakios.mfc.h"

#define SESSIONS_MAX    8
#define HEADER_ROOM     4096
#define STREAM_ROOM     (4u << 20)

struct enc_session {
	VTCompressionSessionRef session;        /* the HEVC session underneath: the identity */
	int fd;
	VTCompressionOutputCallback callback;
	void *refcon;
	int32_t width, height;
	uint32_t fps, bitrate, gop, profile;
	bool hevc;                              /* the codec asked for */
	bool dummy;                             /* H.264: the session underneath is a stand-in; its properties are not passed on */
	bool mine;                              /* its pictures go to the block */
	bool decided;                           /* HEVC: whether they do, settled at the first picture */
	bool realtime, fps_given;
	bool opened, failed;
	CMVideoFormatDescriptionRef format;
	uint8_t *stream;
	uint64_t frames, bytes, us;
};

static struct enc_session sessions[SESSIONS_MAX];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int said_creates;

static void
say(const char *format, ...)
{
	char line[300];
	va_list ap;
	int n = snprintf(line, sizeof(line), "hakios: mfcenc: ");

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	write(2, line, (size_t)n);
}

static struct enc_session *
find(VTSessionRef session)
{
	if (session == NULL) {
		return NULL;
	}
	for (int i = 0; i < SESSIONS_MAX; i++) {
		if (sessions[i].session != NULL && (VTSessionRef)sessions[i].session == session) {
			return &sessions[i];
		}
	}
	return NULL;
}

static void
release(struct enc_session *e)
{
	if (e->fd >= 0) {
		close(e->fd);
	}
	if (e->format != NULL) {
		CFRelease(e->format);
	}
	free(e->stream);
	if (e->frames != 0) {
		say("session %p: %llu pictures, %llu KB, %llu us a picture in the block on average", (void *)e->session,
		    (unsigned long long)e->frames, (unsigned long long)(e->bytes / 1024), (unsigned long long)(e->us / e->frames));
	}
	memset(e, 0, sizeof(*e));
	e->fd = -1;
}

/* the device for a new session; a session never invalidated (only released) still holding it gives it up first */
static int
open_device(void)
{
	int fd = open("/dev/hakios-mfcenc", O_RDWR);

	if (fd < 0 && errno == EBUSY) {
		for (int i = 0; i < SESSIONS_MAX; i++) {
			if (sessions[i].session != NULL && sessions[i].fd >= 0) {
				say("the encoder is held by session %p, never invalidated: taken from it", (void *)sessions[i].session);
				close(sessions[i].fd);
				sessions[i].fd = -1;
				sessions[i].failed = true;
			}
		}
		fd = open("/dev/hakios-mfcenc", O_RDWR);
	}
	return fd;
}

/* a slot for a new session (under the lock): a stale one of the same address dropped; when all are taken, the oldest given up */
static unsigned next_victim;

static struct enc_session *
slot_for(VTCompressionSessionRef session)
{
	struct enc_session *e = NULL;

	for (int i = 0; i < SESSIONS_MAX; i++) {
		if (sessions[i].session == session) {
			release(&sessions[i]);  /* (an object freed without an invalidate, its address used again) */
		}
	}
	for (int i = 0; i < SESSIONS_MAX && e == NULL; i++) {
		if (sessions[i].session == NULL) {
			e = &sessions[i];
		}
	}
	if (e == NULL) {
		e = &sessions[next_victim++ % SESSIONS_MAX];
		release(e);
	}
	memset(e, 0, sizeof(*e));
	e->fd = -1;
	e->session = session;
	return e;
}

/* ---- the interposed functions ---------------------------------------------------------------------------------------------- */

/*
 * VideoToolbox's private create, which its public one calls inside the library (so interposing the public one alone catches nobody
 * inside the shared cache: 15 November, CMCapture's compressor never came through it). Read from its code in 20F66: the public
 * VTCompressionSessionCreate passes its ten arguments on with a NULL inserted before the last -- options, a dictionary.
 */
extern OSStatus VTCompressionSessionCreateWithOptions(CFAllocatorRef allocator, int32_t width, int32_t height, CMVideoCodecType codec,
    CFDictionaryRef specification, CFDictionaryRef source_attributes, CFAllocatorRef compressed_allocator, VTCompressionOutputCallback callback,
    void *refcon, CFDictionaryRef options, VTCompressionSessionRef *out);
extern OSStatus VTCompressionSessionSetProperty(VTCompressionSessionRef session, CFStringRef key, CFTypeRef value);
extern OSStatus VTCompressionSessionSetProperties(VTCompressionSessionRef session, CFDictionaryRef properties);
extern OSStatus VTCompressionSessionCopyProperty(VTCompressionSessionRef session, CFStringRef key, CFAllocatorRef allocator, void *value);

static OSStatus
real_create(bool with_options, CFAllocatorRef allocator, int32_t width, int32_t height, CMVideoCodecType codec, CFDictionaryRef specification,
    CFDictionaryRef source_attributes, CFAllocatorRef compressed_allocator, VTCompressionOutputCallback callback, void *refcon,
    CFDictionaryRef options, VTCompressionSessionRef *out)
{
	if (with_options) {
		return VTCompressionSessionCreateWithOptions(allocator, width, height, codec, specification, source_attributes, compressed_allocator,
		    callback, refcon, options, out);
	}
	return VTCompressionSessionCreate(allocator, width, height, codec, specification, source_attributes, compressed_allocator, callback,
	    refcon, out);
}

static OSStatus
create(bool with_options, CFAllocatorRef allocator, int32_t width, int32_t height, CMVideoCodecType codec, CFDictionaryRef specification,
    CFDictionaryRef source_attributes, CFAllocatorRef compressed_allocator, VTCompressionOutputCallback callback, void *refcon,
    CFDictionaryRef options, VTCompressionSessionRef *out)
{
	OSStatus status;
	int fd;

	if (said_creates++ < 16) {
		say("a session asked for%s: '%c%c%c%c' %dx%d, %s callback", with_options ? " (with options)" : "", (char)(codec >> 24),
		    (char)(codec >> 16), (char)(codec >> 8), (char)codec, width, height, callback != NULL ? "a" : "no");
	}
	if (codec == kCMVideoCodecType_HEVC && out != NULL) {
		status = real_create(with_options, allocator, width, height, codec, specification, source_attributes, compressed_allocator, callback,
		    refcon, options, out);
		if (status == noErr && *out != NULL) {
			pthread_mutex_lock(&lock);
			struct enc_session *e = slot_for(*out);

			e->callback = callback;
			e->refcon = refcon;
			e->width = width;
			e->height = height;
			e->fps = 30;
			e->hevc = true;
			pthread_mutex_unlock(&lock);
		}
		return status;
	}
	if (codec != kCMVideoCodecType_H264 || out == NULL) {
		return real_create(with_options, allocator, width, height, codec, specification, source_attributes, compressed_allocator, callback,
		    refcon, options, out);
	}
	pthread_mutex_lock(&lock);
	fd = open_device();
	if (fd < 0) {
		pthread_mutex_unlock(&lock);
		say("H.264 %dx%d: no /dev/hakios-mfcenc (%d): VideoToolbox's own", width, height, errno);
		return real_create(with_options, allocator, width, height, codec, specification, source_attributes, compressed_allocator, callback,
		    refcon, options, out);
	}
	/* the session underneath: HEVC, which VideoToolbox here can make; it is never given a picture */
	status = real_create(with_options, allocator, width, height, kCMVideoCodecType_HEVC, NULL, source_attributes, compressed_allocator, NULL,
	    NULL, NULL, out);
	if (status != noErr || *out == NULL) {
		close(fd);
		pthread_mutex_unlock(&lock);
		say("H.264 %dx%d: the session underneath was refused (%d)", width, height, (int)status);
		return status != noErr ? status : kVTVideoEncoderNotAvailableNowErr;
	}
	struct enc_session *e = slot_for(*out);

	e->fd = fd;
	e->dummy = true;
	e->mine = true;
	e->decided = true;
	e->callback = callback;
	e->refcon = refcon;
	e->width = width;
	e->height = height;
	e->fps = 30;
	e->profile = HAKIOS_MFC_ENC_HIGH;
	pthread_mutex_unlock(&lock);
	say("H.264 %dx%d: session %p on the MFC", width, height, (void *)*out);
	return noErr;
}

static OSStatus
hk_VTCompressionSessionCreate(CFAllocatorRef allocator, int32_t width, int32_t height, CMVideoCodecType codec, CFDictionaryRef specification,
    CFDictionaryRef source_attributes, CFAllocatorRef compressed_allocator, VTCompressionOutputCallback callback, void *refcon,
    VTCompressionSessionRef *out)
{
	return create(false, allocator, width, height, codec, specification, source_attributes, compressed_allocator, callback, refcon, NULL, out);
}

static OSStatus
hk_VTCompressionSessionCreateWithOptions(CFAllocatorRef allocator, int32_t width, int32_t height, CMVideoCodecType codec,
    CFDictionaryRef specification, CFDictionaryRef source_attributes, CFAllocatorRef compressed_allocator, VTCompressionOutputCallback callback,
    void *refcon, CFDictionaryRef options, VTCompressionSessionRef *out)
{
	return create(true, allocator, width, height, codec, specification, source_attributes, compressed_allocator, callback, refcon, options, out);
}

/* the first call of each function that passes through: which ones mediaserverd's code really calls */
#define FIRST(name) do { static int said_; if (!said_++) say("first call of %s (not ours)", name); } while (0)

static uint32_t
number(CFTypeRef value)
{
	double d = 0;

	if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
		CFNumberGetValue(value, kCFNumberDoubleType, &d);
	}
	return d > 0 && d < 4e9 ? (uint32_t)d : 0;
}

/* what this encoder takes of a property; the rest is accepted and ignored */
static void
take_property(struct enc_session *e, CFStringRef key, CFTypeRef value)
{
	if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) {
		e->bitrate = number(value);
	} else if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate)) {
		e->fps = number(value) ? number(value) : e->fps;
		e->fps_given = number(value) != 0;
	} else if (CFEqual(key, kVTCompressionPropertyKey_RealTime)) {
		e->realtime = value == kCFBooleanTrue;
	} else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) {
		e->gop = number(value);
	} else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel) && value != NULL && CFGetTypeID(value) == CFStringGetTypeID()) {
		char name[64] = "";

		CFStringGetCString(value, name, sizeof(name), kCFStringEncodingUTF8);
		e->profile = strncmp(name, "H264_Baseline", 13) == 0 ? HAKIOS_MFC_ENC_BASELINE :
		    strncmp(name, "H264_Main", 9) == 0 ? HAKIOS_MFC_ENC_MAIN : HAKIOS_MFC_ENC_HIGH;
	}
}

static OSStatus
hk_VTSessionSetProperty(VTSessionRef session, CFStringRef key, CFTypeRef value)
{
	struct enc_session *e;

	pthread_mutex_lock(&lock);
	e = find(session);
	if (e != NULL && key != NULL) {
		take_property(e, key, value);
	}
	bool swallow = e != NULL && e->dummy;

	pthread_mutex_unlock(&lock);
	return swallow ? noErr : VTSessionSetProperty(session, key, value);
}

static void
take_one(const void *key, const void *value, void *context)
{
	if (key != NULL && CFGetTypeID(key) == CFStringGetTypeID()) {
		take_property(context, key, value);
	}
}

static OSStatus
hk_VTSessionSetProperties(VTSessionRef session, CFDictionaryRef properties)
{
	struct enc_session *e;

	pthread_mutex_lock(&lock);
	e = find(session);
	if (e != NULL && properties != NULL) {
		CFDictionaryApplyFunction(properties, take_one, e);
	}
	bool swallow = e != NULL && e->dummy;

	pthread_mutex_unlock(&lock);
	return swallow ? noErr : VTSessionSetProperties(session, properties);
}

static OSStatus
hk_VTSessionCopyProperty(VTSessionRef session, CFStringRef key, CFAllocatorRef allocator, void *value)
{
	struct enc_session *e = find(session);

	if (e != NULL && e->mine && key != NULL && value != NULL) {
		if (CFEqual(key, CFSTR("UsingHardwareAcceleratedVideoEncoder"))) {
			*(CFTypeRef *)value = CFRetain(kCFBooleanTrue);
			return noErr;
		}
		if (CFEqual(key, kVTCompressionPropertyKey_NumberOfPendingFrames)) {
			int zero = 0;

			*(CFTypeRef *)value = CFNumberCreate(NULL, kCFNumberIntType, &zero);
			return noErr;
		}
	}
	return VTSessionCopyProperty(session, key, allocator, value);
}

static OSStatus
hk_VTCompressionSessionSetProperty(VTCompressionSessionRef session, CFStringRef key, CFTypeRef value)
{
	struct enc_session *e;

	pthread_mutex_lock(&lock);
	e = find(session);
	if (e != NULL && key != NULL) {
		take_property(e, key, value);
	}
	bool swallow = e != NULL && e->dummy;

	pthread_mutex_unlock(&lock);
	return swallow ? noErr : VTCompressionSessionSetProperty(session, key, value);
}

static OSStatus
hk_VTCompressionSessionSetProperties(VTCompressionSessionRef session, CFDictionaryRef properties)
{
	struct enc_session *e;

	pthread_mutex_lock(&lock);
	e = find(session);
	if (e != NULL && properties != NULL) {
		CFDictionaryApplyFunction(properties, take_one, e);
	}
	bool swallow = e != NULL && e->dummy;

	pthread_mutex_unlock(&lock);
	return swallow ? noErr : VTCompressionSessionSetProperties(session, properties);
}

static OSStatus
hk_VTCompressionSessionCopyProperty(VTCompressionSessionRef session, CFStringRef key, CFAllocatorRef allocator, void *value)
{
	struct enc_session *e = find(session);

	if (e != NULL && e->mine) {
		return hk_VTSessionCopyProperty(session, key, allocator, value);
	}
	return VTCompressionSessionCopyProperty(session, key, allocator, value);
}

static OSStatus
hk_VTCompressionSessionPrepareToEncodeFrames(VTCompressionSessionRef session)
{
	struct enc_session *e = find(session);

	if (e != NULL && e->dummy) {
		return noErr;
	}
	return VTCompressionSessionPrepareToEncodeFrames(session);
}

static OSStatus
hk_VTCompressionSessionCompleteFrames(VTCompressionSessionRef session, CMTime until)
{
	struct enc_session *e = find(session);

	if (e != NULL && e->mine) {
		return noErr;                   /* (every picture was finished in its own encode call) */
	}
	return VTCompressionSessionCompleteFrames(session, until);
}

static void
hk_VTCompressionSessionInvalidate(VTCompressionSessionRef session)
{
	pthread_mutex_lock(&lock);
	struct enc_session *e = find(session);

	if (e != NULL) {
		release(e);
	}
	pthread_mutex_unlock(&lock);
	VTCompressionSessionInvalidate(session);
}

/* a NAL unit's type: H.264's 5 bits; HEVC's 6, after the forbidden bit */
static uint8_t
nal_type(bool hevc, uint8_t first)
{
	return hevc ? (first >> 1) & 0x3F : first & 0x1F;
}

/* parameter sets: H.264's SPS 7, PPS 8, AUD 9; HEVC's VPS 32, SPS 33, PPS 34, AUD 35 */
static bool
is_parameter_set(bool hevc, uint8_t type)
{
	return hevc ? (type >= 32 && type <= 35) : (type >= 7 && type <= 9);
}

/* the block's parameter sets, Annex B: H.264's SPS and PPS, or HEVC's VPS, SPS and PPS */
static CMVideoFormatDescriptionRef
format_from(bool hevc, const uint8_t *h, size_t n)
{
	const uint8_t *sets[3] = { NULL, NULL, NULL };
	size_t sizes[3] = { 0, 0, 0 }, i = 0;

	while (i + 3 < n) {
		if (h[i] == 0 && h[i + 1] == 0 && h[i + 2] == 1) {
			size_t start = i + 3, end = start;

			while (end + 2 < n && !(h[end] == 0 && h[end + 1] == 0 && (h[end + 2] == 1 || (h[end + 2] == 0 && end + 3 < n && h[end + 3] == 1)))) {
				end++;
			}
			if (end + 2 >= n) {
				end = n;
			}
			uint8_t type = nal_type(hevc, h[start]);
			int k = hevc ? (type == 32 ? 0 : type == 33 ? 1 : type == 34 ? 2 : -1) : (type == 7 ? 0 : type == 8 ? 1 : -1);

			if (k >= 0 && sets[k] == NULL) {
				sets[k] = h + start, sizes[k] = end - start;
			}
			i = end;
		} else {
			i++;
		}
	}
	if (sets[0] == NULL || sets[1] == NULL || (hevc && sets[2] == NULL)) {
		say("the %s header lacks a parameter set (%zu bytes)", hevc ? "HEVC" : "H.264", n);
		return NULL;
	}
	CMVideoFormatDescriptionRef format = NULL;
	OSStatus status = hevc ? CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, 3, sets, sizes, 4, NULL, &format) :
	    CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, sizes, 4, &format);

	if (status != noErr) {
		say("the %s format from parameter sets of %zu, %zu, %zu bytes: %d", hevc ? "HEVC" : "H.264", sizes[0], sizes[1], sizes[2], (int)status);
		return NULL;
	}
	return format;
}

/* Annex B to AVCC, in place into `out`; parameter sets dropped (they are in the format). Answers the bytes written. */
static size_t
to_avcc(bool hevc, const uint8_t *in, size_t n, uint8_t *out)
{
	size_t i = 0, at = 0;

	while (i + 3 <= n) {
		size_t sc = 0;

		if (in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1) {
			sc = 3;
		} else if (i + 4 <= n && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 0 && in[i + 3] == 1) {
			sc = 4;
		}
		if (sc == 0) {
			i++;
			continue;
		}
		size_t start = i + sc, end = start;

		while (end + 3 <= n && !(in[end] == 0 && in[end + 1] == 0 && (in[end + 2] == 1 || (end + 4 <= n && in[end + 2] == 0 && in[end + 3] == 1)))) {
			end++;
		}
		if (end + 3 > n) {
			end = n;
		}
		size_t len = end - start;
		if (len > 0 && !is_parameter_set(hevc, nal_type(hevc, in[start]))) {
			out[at] = (uint8_t)(len >> 24);
			out[at + 1] = (uint8_t)(len >> 16);
			out[at + 2] = (uint8_t)(len >> 8);
			out[at + 3] = (uint8_t)len;
			memmove(out + at + 4, in + start, len);
			at += 4 + len;
		}
		i = end;
	}
	return at;
}

static bool
open_encoder(struct enc_session *e)
{
	uint8_t header[HEADER_ROOM];
	struct hakios_mfc_enc_open o = {
		.width = (uint32_t)e->width & ~1u, .height = (uint32_t)e->height & ~1u, .fps = e->fps, .bitrate = e->bitrate,
		.gop = e->gop, .profile = e->hevc ? 0 : e->profile,
		.level = e->hevc ? ((e->width * e->height > 1280 * 720) ? 41 : 31) : ((e->width * e->height > 1280 * 720) ? 40 : 31),
		.header_room = sizeof(header), .header = (uint64_t)(uintptr_t)header,
		.codec = e->hevc ? HAKIOS_MFC_HEVC : HAKIOS_MFC_H264,
	};

	if (ioctl(e->fd, HAKIOS_MFC_ENC_OPEN, &o) != 0) {
		say("session %p: the encoder refused %ux%u (%d)", (void *)e->session, o.width, o.height, errno);
		return false;
	}
	e->format = format_from(e->hevc, header, o.header_bytes);
	e->stream = malloc(STREAM_ROOM);
	if (e->format == NULL || e->stream == NULL) {
		return false;
	}
	say("session %p: %s %ux%u, %u fps, %u b/s, key every %u, profile %u: header %u bytes", (void *)e->session, e->hevc ? "HEVC" : "H.264",
	    o.width, o.height, o.fps, o.bitrate, o.gop, o.profile, o.header_bytes);
	return true;
}

/* one picture through the block; the sample buffer for it, or NULL with *status set */
static CMSampleBufferRef
encode(struct enc_session *e, CVImageBufferRef image, CMTime pts, CMTime duration, CFDictionaryRef options, OSStatus *status)
{
	OSType pf = CVPixelBufferGetPixelFormatType(image);
	struct hakios_mfc_enc_frame f;
	CMSampleBufferRef sample = NULL;

	*status = kVTVideoEncoderMalfunctionErr;
	if (pf != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && pf != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
		static int told;

		if (told++ < 3) {
			say("session %p: a picture of '%c%c%c%c', not NV12: refused", (void *)e->session, (char)(pf >> 24), (char)(pf >> 16),
			    (char)(pf >> 8), (char)pf);
		}
		*status = kVTPixelTransferNotSupportedErr;
		return NULL;
	}
	if (!e->opened) {
		/* (the picture's own size wins over the session's, should they differ) */
		e->width = (int32_t)CVPixelBufferGetWidth(image);
		e->height = (int32_t)CVPixelBufferGetHeight(image);
		e->opened = true;
		e->failed = !open_encoder(e);
	}
	if (e->failed || e->fd < 0) {
		return NULL;
	}
	if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
		return NULL;
	}
	memset(&f, 0, sizeof(f));
	f.y = (uint64_t)(uintptr_t)CVPixelBufferGetBaseAddressOfPlane(image, 0);
	f.uv = (uint64_t)(uintptr_t)CVPixelBufferGetBaseAddressOfPlane(image, 1);
	f.y_stride = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(image, 0);
	f.uv_stride = (uint32_t)CVPixelBufferGetBytesPerRowOfPlane(image, 1);
	f.key = e->frames == 0 || (options != NULL && CFDictionaryGetValue(options, kVTEncodeFrameOptionKey_ForceKeyFrame) == kCFBooleanTrue);
	f.stream = (uint64_t)(uintptr_t)e->stream;
	f.stream_room = STREAM_ROOM;
	int r = ioctl(e->fd, HAKIOS_MFC_ENC_FRAME, &f);
	int error = errno;

	CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
	if (r != 0) {
		static int told;

		if (told++ < 5) {
			say("session %p: picture %llu: the block refused it (%d)", (void *)e->session, (unsigned long long)e->frames, error);
		}
		return NULL;
	}
	size_t n = to_avcc(e->hevc, e->stream, f.bytes, e->stream);
	if (n == 0) {
		*status = noErr;                /* (a picture the block skipped: nothing to emit) */
		return NULL;
	}
	CMBlockBufferRef block = NULL;
	void *data = malloc(n);

	if (data == NULL) {
		return NULL;
	}
	memcpy(data, e->stream, n);
	if (CMBlockBufferCreateWithMemoryBlock(NULL, data, n, kCFAllocatorMalloc, NULL, 0, n, 0, &block) != kCMBlockBufferNoErr) {
		free(data);
		return NULL;
	}
	CMSampleTimingInfo timing = { .duration = duration, .presentationTimeStamp = pts, .decodeTimeStamp = pts };
	size_t size = n;
	OSStatus s = CMSampleBufferCreateReady(NULL, block, e->format, 1, 1, &timing, 1, &size, &sample);

	CFRelease(block);
	if (s != noErr || sample == NULL) {
		say("session %p: the sample buffer: %d", (void *)e->session, (int)s);
		return NULL;
	}
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);

	if (attachments != NULL && CFArrayGetCount(attachments) > 0) {
		CFMutableDictionaryRef a = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);

		CFDictionarySetValue(a, kCMSampleAttachmentKey_NotSync, f.key ? kCFBooleanFalse : kCFBooleanTrue);
		CFDictionarySetValue(a, kCMSampleAttachmentKey_DependsOnOthers, f.key ? kCFBooleanFalse : kCFBooleanTrue);
	}
	e->frames++;
	e->bytes += n;
	e->us += f.us;
	if (e->frames <= 2) {
		say("session %p: picture %llu: %zu bytes%s, %u us in the block", (void *)e->session, (unsigned long long)e->frames, n,
		    f.key ? " (key)" : "", f.us);
	}
	*status = noErr;
	return sample;
}


/*
 * An HEVC session at its first picture: the block's if it is a video's (real time asked for, a frame rate given, or a video's size),
 * VideoToolbox's otherwise (the photographs' HEIC, made by CMPhoto). Under the lock.
 */
static void
decide(struct enc_session *e, CVImageBufferRef image)
{
	if (e->decided) {
		return;
	}
	e->decided = true;
	size_t w = image != NULL ? CVPixelBufferGetWidth(image) : (size_t)e->width, h = image != NULL ? CVPixelBufferGetHeight(image) : (size_t)e->height;
	size_t big = w > h ? w : h, small = w > h ? h : w;
	bool video_size = (big == 1920 && small == 1080) || (big == 1280 && small == 720) || (big == 3840 && small == 2160) || (big == 640 && small == 480);
	bool video = e->realtime || e->fps_given || video_size;

	if (video) {
		e->fd = open_device();
		e->mine = e->fd >= 0;
	}
	say("HEVC session %p, %zux%zu (real time %d, frame rate %s, a video's size %d): %s", (void *)e->session, w, h, e->realtime,
	    e->fps_given ? "given" : "not given", video_size, e->mine ? "the MFC's" : video ? "VideoToolbox's (no /dev/hakios-mfcenc)" :
	    "VideoToolbox's (not a video)");
}

static OSStatus
hk_VTCompressionSessionEncodeFrame(VTCompressionSessionRef session, CVImageBufferRef image, CMTime pts, CMTime duration,
    CFDictionaryRef options, void *frame_refcon, VTEncodeInfoFlags *info)
{
	pthread_mutex_lock(&lock);
	struct enc_session *e = find(session);

	if (e != NULL) {
		decide(e, image);
	}
	if (e == NULL || !e->mine) {
		pthread_mutex_unlock(&lock);
		FIRST("VTCompressionSessionEncodeFrame");
		return VTCompressionSessionEncodeFrame(session, image, pts, duration, options, frame_refcon, info);
	}
	OSStatus status;
	CMSampleBufferRef sample = image != NULL ? encode(e, image, pts, duration, options, &status) : NULL;
	VTCompressionOutputCallback callback = e->callback;
	void *refcon = e->refcon;

	if (image == NULL) {
		status = kVTParameterErr;
	}
	pthread_mutex_unlock(&lock);
	if (info != NULL) {
		*info = 0;
	}
	/* every picture gets its one output, as VideoToolbox gives it: a failure with no sample buffer */
	if (callback != NULL) {
		callback(refcon, frame_refcon, status, 0, sample);
	}
	if (sample != NULL) {
		CFRelease(sample);
	}
	return noErr;
}

static OSStatus
hk_VTCompressionSessionEncodeFrameWithOutputHandler(VTCompressionSessionRef session, CVImageBufferRef image, CMTime pts, CMTime duration,
    CFDictionaryRef options, VTEncodeInfoFlags *info, VTCompressionOutputHandler handler)
{
	pthread_mutex_lock(&lock);
	struct enc_session *e = find(session);

	if (e != NULL) {
		decide(e, image);
	}
	if (e == NULL || !e->mine) {
		pthread_mutex_unlock(&lock);
		FIRST("VTCompressionSessionEncodeFrameWithOutputHandler");
		return VTCompressionSessionEncodeFrameWithOutputHandler(session, image, pts, duration, options, info, handler);
	}
	OSStatus status = kVTParameterErr;
	CMSampleBufferRef sample = image != NULL ? encode(e, image, pts, duration, options, &status) : NULL;

	pthread_mutex_unlock(&lock);
	if (info != NULL) {
		*info = 0;
	}
	if (handler != NULL) {
		handler(status, 0, sample);
	}
	if (sample != NULL) {
		CFRelease(sample);
	}
	return noErr;
}

__attribute__((used)) static const struct { const void *replacement, *replacee; } hakios_mfcenc_interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)hk_VTCompressionSessionCreate, (const void *)VTCompressionSessionCreate },
	{ (const void *)hk_VTCompressionSessionCreateWithOptions, (const void *)VTCompressionSessionCreateWithOptions },
	{ (const void *)hk_VTCompressionSessionSetProperty, (const void *)VTCompressionSessionSetProperty },
	{ (const void *)hk_VTCompressionSessionSetProperties, (const void *)VTCompressionSessionSetProperties },
	{ (const void *)hk_VTCompressionSessionCopyProperty, (const void *)VTCompressionSessionCopyProperty },
	{ (const void *)hk_VTSessionSetProperty, (const void *)VTSessionSetProperty },
	{ (const void *)hk_VTSessionSetProperties, (const void *)VTSessionSetProperties },
	{ (const void *)hk_VTSessionCopyProperty, (const void *)VTSessionCopyProperty },
	{ (const void *)hk_VTCompressionSessionPrepareToEncodeFrames, (const void *)VTCompressionSessionPrepareToEncodeFrames },
	{ (const void *)hk_VTCompressionSessionCompleteFrames, (const void *)VTCompressionSessionCompleteFrames },
	{ (const void *)hk_VTCompressionSessionInvalidate, (const void *)VTCompressionSessionInvalidate },
	{ (const void *)hk_VTCompressionSessionEncodeFrame, (const void *)VTCompressionSessionEncodeFrame },
	{ (const void *)hk_VTCompressionSessionEncodeFrameWithOutputHandler, (const void *)VTCompressionSessionEncodeFrameWithOutputHandler },
};

__attribute__((constructor)) static void
mfcenc_init(void)
{
	for (int i = 0; i < SESSIONS_MAX; i++) {
		sessions[i].fd = -1;
	}
	say("H.264 sessions go to the MFC when /dev/hakios-mfcenc is there (%s)", access("/dev/hakios-mfcenc", F_OK) == 0 ? "it is" : "it is not");
}
