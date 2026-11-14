/*
 * H.264 encoding for mediaserverd on the Exynos 9810's MFC (kernel: bsd/dev/hakios.exynos_mfc.c, /dev/hakios-mfcenc), part of
 * libhakios.media.dylib.
 *
 * Why: this iPhone's VideoToolbox has no H.264 encoder and only a software HEVC one (VCPHEVC.videocodec, registered for the
 * photographs' HEIC by inject_media.c). A video recording through that one took four to seven cores, fell behind, and at the stop
 * the movie was never finished (15 November 2026, 1080p and 720p alike).
 *
 * How: VideoToolbox's public compression API is interposed in this process. A session asked for H.264 ('avc1') is made as an HEVC
 * session underneath -- an object VideoToolbox knows, for the calls this file passes on (its pixel buffer pool, its properties) --
 * and every picture given to it is encoded by the block instead, synchronously: VTCompressionSessionEncodeFrame returns once the
 * picture's sample buffer has gone to the session's output callback. Everything else ('hvc1' for the photographs, any session
 * when /dev/hakios-mfcenc is missing or busy) is VideoToolbox's own, untouched.
 *
 * The block gives Annex B; the sample buffers carry AVCC (4-byte lengths) and a format made from its SPS and PPS.
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

/* ---- the interposed functions ---------------------------------------------------------------------------------------------- */

static OSStatus
hk_VTCompressionSessionCreate(CFAllocatorRef allocator, int32_t width, int32_t height, CMVideoCodecType codec, CFDictionaryRef specification,
    CFDictionaryRef source_attributes, CFAllocatorRef compressed_allocator, VTCompressionOutputCallback callback, void *refcon,
    VTCompressionSessionRef *out)
{
	OSStatus status;
	int fd;

	if (said_creates++ < 12) {
		say("a session asked for: '%c%c%c%c' %dx%d, %s callback", (char)(codec >> 24), (char)(codec >> 16), (char)(codec >> 8), (char)codec,
		    width, height, callback != NULL ? "a" : "no");
	}
	if (codec != kCMVideoCodecType_H264 || out == NULL) {
		return VTCompressionSessionCreate(allocator, width, height, codec, specification, source_attributes, compressed_allocator, callback,
		    refcon, out);
	}
	pthread_mutex_lock(&lock);
	fd = open_device();
	if (fd < 0) {
		pthread_mutex_unlock(&lock);
		say("H.264 %dx%d: no /dev/hakios-mfcenc (%d): VideoToolbox's own", width, height, errno);
		return VTCompressionSessionCreate(allocator, width, height, codec, specification, source_attributes, compressed_allocator, callback,
		    refcon, out);
	}
	/* the session underneath: HEVC, which VideoToolbox here can make; it is never given a picture */
	status = VTCompressionSessionCreate(allocator, width, height, kCMVideoCodecType_HEVC, NULL, source_attributes, compressed_allocator, NULL,
	    NULL, out);
	if (status != noErr || *out == NULL) {
		close(fd);
		pthread_mutex_unlock(&lock);
		say("H.264 %dx%d: the session underneath was refused (%d)", width, height, (int)status);
		return status != noErr ? status : kVTVideoEncoderNotAvailableNowErr;
	}
	struct enc_session *e = NULL;

	for (int i = 0; i < SESSIONS_MAX && e == NULL; i++) {
		if (sessions[i].session == NULL) {
			e = &sessions[i];
		}
	}
	if (e == NULL) {
		release(&sessions[0]);          /* (the oldest slot: one never invalidated) */
		e = &sessions[0];
	}
	memset(e, 0, sizeof(*e));
	e->session = *out;
	e->fd = fd;
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
	pthread_mutex_unlock(&lock);
	if (e != NULL) {
		return noErr;
	}
	return VTSessionSetProperty(session, key, value);
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
	pthread_mutex_unlock(&lock);
	if (e != NULL) {
		return noErr;
	}
	return VTSessionSetProperties(session, properties);
}

static OSStatus
hk_VTSessionCopyProperty(VTSessionRef session, CFStringRef key, CFAllocatorRef allocator, void *value)
{
	if (find(session) != NULL && key != NULL && value != NULL) {
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
hk_VTCompressionSessionPrepareToEncodeFrames(VTCompressionSessionRef session)
{
	if (find(session) != NULL) {
		return noErr;
	}
	return VTCompressionSessionPrepareToEncodeFrames(session);
}

static OSStatus
hk_VTCompressionSessionCompleteFrames(VTCompressionSessionRef session, CMTime until)
{
	if (find(session) != NULL) {
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

/* the block's parameter sets: Annex B, SPS (type 7) then PPS (type 8) */
static CMVideoFormatDescriptionRef
format_from(const uint8_t *h, size_t n, int32_t width, int32_t height)
{
	const uint8_t *sets[2] = { NULL, NULL };
	size_t sizes[2] = { 0, 0 }, i = 0;

	while (i + 3 < n) {
		if (h[i] == 0 && h[i + 1] == 0 && h[i + 2] == 1) {
			size_t start = i + 3, end = start;

			while (end + 2 < n && !(h[end] == 0 && h[end + 1] == 0 && (h[end + 2] == 1 || (h[end + 2] == 0 && end + 3 < n && h[end + 3] == 1)))) {
				end++;
			}
			if (end + 2 >= n) {
				end = n;
			}
			uint8_t type = h[start] & 0x1F;

			if (type == 7 && sets[0] == NULL) {
				sets[0] = h + start, sizes[0] = end - start;
			} else if (type == 8 && sets[1] == NULL) {
				sets[1] = h + start, sizes[1] = end - start;
			}
			i = end;
		} else {
			i++;
		}
	}
	if (sets[0] == NULL || sets[1] == NULL) {
		say("the header has no SPS or PPS (%zu bytes)", n);
		return NULL;
	}
	CMVideoFormatDescriptionRef format = NULL;
	OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, sizes, 4, &format);

	if (status != noErr) {
		say("the format from the SPS (%zu) and PPS (%zu): %d", sizes[0], sizes[1], (int)status);
		return NULL;
	}
	(void)width, (void)height;
	return format;
}

/* Annex B to AVCC, in place into `out`; parameter sets dropped (they are in the format). Answers the bytes written. */
static size_t
to_avcc(const uint8_t *in, size_t n, uint8_t *out)
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
		uint8_t type = len > 0 ? in[start] & 0x1F : 0;

		if (len > 0 && type != 7 && type != 8 && type != 9) {
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
		.gop = e->gop, .profile = e->profile, .level = (e->width * e->height > 1280 * 720) ? 40 : 31,
		.header_room = sizeof(header), .header = (uint64_t)(uintptr_t)header,
	};

	if (ioctl(e->fd, HAKIOS_MFC_ENC_OPEN, &o) != 0) {
		say("session %p: the encoder refused %ux%u (%d)", (void *)e->session, o.width, o.height, errno);
		return false;
	}
	e->format = format_from(header, o.header_bytes, e->width, e->height);
	e->stream = malloc(STREAM_ROOM);
	if (e->format == NULL || e->stream == NULL) {
		return false;
	}
	say("session %p: %ux%u, %u fps, %u b/s, key every %u, profile %u: header %u bytes", (void *)e->session, o.width, o.height, o.fps,
	    o.bitrate, o.gop, o.profile, o.header_bytes);
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
	size_t n = to_avcc(e->stream, f.bytes, e->stream);
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

static OSStatus
hk_VTCompressionSessionEncodeFrame(VTCompressionSessionRef session, CVImageBufferRef image, CMTime pts, CMTime duration,
    CFDictionaryRef options, void *frame_refcon, VTEncodeInfoFlags *info)
{
	pthread_mutex_lock(&lock);
	struct enc_session *e = find(session);

	if (e == NULL) {
		pthread_mutex_unlock(&lock);
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

	if (e == NULL) {
		pthread_mutex_unlock(&lock);
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
