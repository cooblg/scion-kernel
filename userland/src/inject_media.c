/*
 * libhakios.media.dylib: code of ours inside mediaserverd (see inject.c for how it gets there).
 *
 * mediaserverd is the audio server, and also the camera's: the capture stack of CMCapture lives
 * in the same process. Soon after it starts, that stack "prewarms" the Core Image filters of the
 * portrait mode, for which Core Image wants an OpenGL ES context; there is no GPU, the context
 * has no functions behind it, and the daemon jumps to address 0 -- every time launchd restarts
 * it. Nobody is going to take a portrait here: the prewarming is skipped.
 *
 * What the daemon is wanted for meanwhile is neither sound nor the camera: it is where
 * VideoToolbox decodes for everybody else (the service com.apple.coremedia.decompressionsession),
 * and a HEIC picture is HEVC inside. Without the daemon ImageIO has "no way to decode a HEIC",
 * and the stock wallpaper of Darwin userland 16, whose layers are four such pictures, is its gradient alone.
 * There is no hardware decoder here; VideoToolbox takes its software one (VCPHEVC.videocodec).
 *
 * The flashlight (only where the kernel has one: torch=3, /dev/hakios-torch): Control Center's
 * button and AVFlashlight reach FigFlashlight in here, which asks the capture stack's device vendor
 * (+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]) for the camera device and its back
 * camera's stream, and sets the stream's "TorchLevel" (0 to 1). Without a camera there is no
 * vendor (it is nil), every message to it does nothing, and FigFlashlight says it has set the level
 * when nothing has been. So where there is no vendor, FigFlashlight -- and only FigFlashlight,
 * by where it calls from -- is given one of ours, with a device and a stream whose torch level goes
 * to the kernel (see flashlight_*() below). Everybody else still gets none, as before.
 *
 * The camera (only where the kernel has one: cam=5, /dev/hakios-camera): the vendor is made for
 * everybody, with the create function of our capture device (camera_device.c) in place of the
 * one H10ISP.mediacapture would give for the iPhone's ISP; the capture graph is built around it and
 * its frames reach Camera.app's preview (see camera_device.c). Its stream takes the torch level too,
 * so FigFlashlight would need nothing of its own then (not tried yet).
 *
 * What the capture stack asks of hardware this phone has none of is refused here: there is no ISP
 * processing session, so there can be no video stabilization (see video_stabilization_method below).
 *
 * CMCapture is not loaded yet when this library is: the methods are replaced when its image
 * arrives.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
}

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;
static id send0(id o, const char *sel);
static CFMutableSetRef ended_outputs;           /* outputs we marked ended: their later buffers are dropped */
static pthread_mutex_t ended_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef ends_owed;        /* output -> how many of the node's own (late) ends to swallow (end_live_output) */
static __thread bool ending_here;               /* audio_source_end's own markEndOfLiveOutput */
static CFMutableSetRef stopped_sources;         /* audio source nodes stopped and not started again (under ended_lock) */

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}

/* ---- the flashlight ------------------------------------------------------------------------------- */

static int torch = -1;                  /* /dev/hakios-torch */
static bool camera;                     /* /dev/hakios-camera is there */
extern int hakios_camera_device_create(const void *allocator, const void *options, void **out);
static Class vendor_class, device_class, stream_class;
static IMP original_shared_vendor;
static unsigned torch_said;

/*
 * FigFlashlight's functions that ask for the vendor, unslid (20F66): flashlight_IsAvailable,
 * _NotifyForCurrentState and _setupDevice lie together, _teardownDevice apart.
 */
static const struct { uintptr_t from, to; } flashlight_code[] = {
	{ 0x1a1fb0830UL, 0x1a1fb0c5cUL }, { 0x1a1eee5c0UL, 0x1a1eee6e8UL },
};

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor] */
static id
shared_vendor(id self, SEL selector)
{
	uintptr_t caller = (uintptr_t)__builtin_return_address(0) - slide;
	id vendor = ((id (*)(id, SEL))original_shared_vendor)(self, selector);
	static id ours, camera_vendor;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	if (vendor != NULL) {
		return vendor;
	}
	if (!camera && access("/dev/hakios-camera", F_OK) == 0) {
		camera = true;          /* (should the kernel have made it after mediaserverd started) */
		say("hakios: mediaserverd: the kernel has a camera (/dev/hakios-camera)\n");
	}
	if (camera) {
		pthread_mutex_lock(&lock);
		if (camera_vendor == NULL) {
			id made = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("BWFigCaptureDeviceVendor"), sel_registerName("alloc"));

			camera_vendor = ((id (*)(id, SEL, void *))objc_msgSend)(made, sel_registerName("initWithDeviceCreateFunction:"),
			    (void *)hakios_camera_device_create);
			say("hakios: mediaserverd: the camera's device vendor %s\n", camera_vendor ? "made (camera_device.c)" :
			    "CANNOT BE MADE");
		}
		pthread_mutex_unlock(&lock);
		return camera_vendor;
	}
	for (size_t i = 0; torch >= 0 && i < sizeof(flashlight_code) / sizeof(flashlight_code[0]); i++) {
		if (caller >= flashlight_code[i].from && caller < flashlight_code[i].to) {
			if (ours == NULL) {
				ours = class_createInstance(vendor_class, 0);
				say("hakios: mediaserverd: no camera, so no device vendor: FigFlashlight gets the flashlight's own\n");
			}
			return ours;
		}
	}
	return NULL;
}

static void
flashlight_set(uint32_t level)
{
	ssize_t written = write(torch, &level, sizeof(level));

	if (written != (ssize_t)sizeof(level) || torch_said < 40) {
		torch_said++;
		say("hakios: mediaserverd: the flashlight at %u of 1000%s\n", level, written == (ssize_t)sizeof(level) ? "" :
		    " (NOT TAKEN by /dev/hakios-torch)");
	}
}

/* the vendor's: one client, one device, always available to it */
static int
flashlight_register(__unused id self, __unused SEL selector, __unused int pid, __unused id description, __unused int priority,
    __unused BOOL steal, __unused BOOL share, __unused id handler)
{
	return 1;
}

static id
flashlight_copy_device(__unused id self, __unused SEL selector, __unused int client, __unused BOOL inform, int *error)
{
	if (error != NULL) {
		*error = 0;
	}
	return class_createInstance(device_class, 0);
}

static id
flashlight_copy_stream(__unused id self, __unused SEL selector, __unused int position, __unused int type, id device)
{
	return device != NULL && object_getClass(device) == device_class ? class_createInstance(stream_class, 0) : NULL;
}

static void
flashlight_take_back(__unused id self, __unused SEL selector, __unused id device, __unused int client, __unused BOOL inform)
{
	flashlight_set(0);
}

static BOOL
flashlight_active_equals(__unused id self, __unused SEL selector, id device)
{
	return device == NULL || object_getClass(device) == device_class;
}

/* the stream's -supportedProperties: none that FigFlashlight looks for ("HighCurrentTorchEnabled") */
static id
flashlight_stream_properties(__unused id self, __unused SEL selector)
{
	static CFDictionaryRef none;

	if (none == NULL) {
		none = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	return (id)none;
}

/* the stream's -setProperty:value: -- "TorchLevel", 0 to 1, as thousandths to the kernel */
static int
flashlight_stream_set(__unused id self, __unused SEL selector, id key, id value)
{
	float level = 0;

	if (key != NULL && CFEqual((CFTypeRef)key, CFSTR("TorchLevel"))) {
		if (value != NULL && CFGetTypeID((CFTypeRef)value) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef)value, kCFNumberFloatType, &level);
		}
		level = level < 0 ? 0 : level > 1 ? 1 : level;
		flashlight_set((uint32_t)(level * 1000 + 0.5f));
	}
	return 0;
}

/*
 * Video stabilization, of which this hardware has none: the capture graph's VIS node
 * (-[FigCaptureVISPipeline _newVISNodeWithUpstreamOutput:...]) is made with the ISP's processing
 * session, and this device has no such session, so the node comes back nil, the VIS pipeline with
 * it, the movie file sink's tail pipeline with that, and captureSession_buildGraphWithConfiguration
 * tears the whole graph down -- the preview too, for all that the preview has nothing to do with
 * stabilizing a recording. The connection's stabilization method is where the whole stack reads the
 * wish from (+[FigVideoCaptureConnectionConfiguration videoStabilizationMethods:includeIris:] ->
 * cspc_getVideoStabilizationType -> the connections' FigCaptureVideoStabilizationType), and 0 there
 * means none: the graph is then built without a VIS pipeline anywhere
 * (+[FigCaptureVISPipeline visPipelineIsRequiredForFigCaptureVideoStabilizationType:] says no to
 * every type but 2 and 3), and recordings are as steady as the hand that holds the phone.
 */
static int
video_stabilization_method(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;
}

/*
 * The type the stack acts on. The connection configuration arrives from the application's
 * AVFoundation with the type in it, which is read straight out of the object and not computed from
 * the method above (-[FigCaptureSessionParsedConfiguration _computeVideoStabilizationTypes] only
 * fills in what the client left out), so the wish has to be refused here as well.
 */
static int
video_stabilization_type(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;                       /* kFigCaptureVideoStabilizationType_None */
}

/*
 * The microphone, which the capture stack has none of here. A capture session's sound comes through
 * CoreMedia's own capture audio session -- BWAudioSourceNode's start sets its properties and reads
 * its pull format, all of it through a brokered plug-in of the audio HAL -- and that plug-in is not
 * on this phone: the node's start comes back with 'nope', the HAL's word for "no such thing".
 *
 * Camera.app's session has an audio input in it, because it is a session for photographs and for
 * video both. The graph starts its source nodes late (startDeferredSourceNodesIfNeeded) and takes
 * the first error it gets for the whole graph's, so the session reported AVCaptureSessionRuntimeError
 * to the application, which took the session down and built it again, two seconds later, for ever:
 * the camera's preview came in bursts of five frames with the sensor powering down in between.
 *
 * The node is told that it started. It then produces no sound -- which is all a phone with no
 * capture audio has to offer -- and the rest of the graph, the preview and the photograph, runs.
 * Only that one error is swallowed: a microphone that is there and fails for another reason still
 * takes the session down, as it should. (The kernel's own microphone, adsp=1 mic=4, is a different
 * path: AVAudioRecorder and Voice Memos record through it, and nothing here touches it.)
 */
#define ERROR_NO_SUCH_THING     1852797029      /* 'nope' */

static IMP original_audio_source_start;

static BOOL
audio_source_start(id self, SEL selector, id *error)
{
	BOOL started;
	id made;

	pthread_mutex_lock(&ended_lock);
	if (stopped_sources != NULL) {
		CFSetRemoveValue(stopped_sources, (const void *)self);         /* running again */
	}
	if (ended_outputs != NULL) {
		id outs = send0(self, "outputs");
		long n = outs != nil ? ((long (*)(id, SEL))objc_msgSend)(outs, sel_registerName("count")) : 0;

		for (long i = 0; i < n; i++) {
			CFSetRemoveValue(ended_outputs, (const void *)((id (*)(id, SEL, long))objc_msgSend)(outs,
			    sel_registerName("objectAtIndex:"), i));
		}
	}
	pthread_mutex_unlock(&ended_lock);
	started = ((BOOL (*)(id, SEL, id *))original_audio_source_start)(self, selector, error);
	made = error != NULL ? *error : NULL;
	static bool said;

	if (started || made == NULL || ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) != ERROR_NO_SUCH_THING) {
		return started;
	}
	*error = NULL;                  /* (autoreleased by the capture stack: not ours to give back) */
	if (!said) {
		said = true;
		say("hakios: mediaserverd: there is no capture audio on this phone: the session's audio source gives nothing, "
		    "and the camera runs without it\n");
	}
	return true;
}

/*
 * A photograph's quality prioritization, "speed" at the most. Asked for "balanced" (Camera.app's) or
 * "quality", the capture device takes a scene for a low-light one whenever a frame was exposed at
 * least as long as the sensor's still image stabilization threshold
 * (-[BWFigVideoCaptureDevice _isLowLightSceneUsingFrameStatisticsByPortType:]) -- a threshold from
 * the sensor's tuning in CameraSetup.plist, 0 for a sensor it has no tuning for, as ours -- and
 * then asks the stream for a bracket of four frames for SIS fusion (captureType 4, BWSISNode), which
 * this port has nothing to fuse with: the photograph never came. With "speed" it asks for one frame
 * (-_resolveStillImageCaptureTypeFromStatisticsByPortType:..., captureType 1), which the stream
 * gives (camera_device.c, StillImageCaptureNow).
 */
static IMP original_quality_prioritization;

static int
quality_prioritization(id self, SEL selector)
{
	int asked = ((int (*)(id, SEL))original_quality_prioritization)(self, selector);
	static bool said;

	if (asked > 1 && !said) {
		said = true;
		say("hakios: mediaserverd: a photograph's quality prioritization %d taken as 1 (speed): one frame, no SIS bracket\n",
		    asked);
	}
	return asked > 1 ? 1 : asked;
}

/*
 * A photograph's noise reduction (BWGNRNode, between the still image coordinator and the photo
 * encoder) is a FigSampleBufferProcessor of the iPhone's, which is handed the picture and never
 * hands one back here: the photograph stopped there, with no error, and the request with it (the
 * shutter's second press asked for nothing more). The picture goes past it to the node's output as
 * it came, as with no noise reduction to do.
 */
static void
gnr_render(id self, SEL selector, void *sample_buffer, id input)
{
	id output = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("output"));
	static bool said;

	(void)selector, (void)input;
	if (!said) {
		said = true;
		say("hakios: mediaserverd: a photograph goes past the noise reduction, which has nothing to do it with here\n");
	}
	if (output != nil) {
		((void (*)(id, SEL, void *))objc_msgSend)(output, sel_registerName("emitSampleBuffer:"), sample_buffer);
	}
}

static bool
is_node(id node, const char *name)
{
	return node != nil && strcmp(object_getClassName(node), name) == 0;
}

/*
 * How long a photograph's encoding took, from the picture handed to the photo encoder node to the one
 * it hands on (HEVC in software, see register_hevc_encoder below: the slowest step of a photograph).
 */
static void
time_photo_encoder(id output, CMSampleBufferRef sample_buffer)
{
	CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample_buffer);
	static uint64_t began;
	static mach_timebase_info_data_t timebase;
	id connection, input;

	if (image == NULL || CVPixelBufferGetWidth(image) < 2000) {
		return;                 /* not a photograph */
	}
	if (is_node(((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("node")), "BWPhotoEncoderNode")) {
		if (began != 0) {
			if (timebase.denom == 0) {
				mach_timebase_info(&timebase);
			}
			say("hakios: mediaserverd: a photograph of %zux%zu encoded in %.0f ms\n", CVPixelBufferGetWidth(image),
			    CVPixelBufferGetHeight(image), (double)(mach_absolute_time() - began) * timebase.numer / timebase.denom / 1e6);
		}
		began = 0;
		return;
	}
	connection = ((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("connection"));
	input = connection != nil ? ((id (*)(id, SEL))objc_msgSend)(connection, sel_registerName("input")) : nil;
	if (input != nil && is_node(((id (*)(id, SEL))objc_msgSend)(input, sel_registerName("node")), "BWPhotoEncoderNode")) {
		began = mach_absolute_time();
	}
}

/*
 * A node's output given no sample buffer at all, which no node takes: the session's audio source
 * (-[BWAudioSourceNode _generateSamples]), whose audio unit never started here (see audio_source_start
 * above), fills the silence by a timer with buffers it fails to make and emits them all the same --
 * NULL. Live Photo's staging node added that to an array (-[BWIrisStagingNode renderSampleBuffer:forInput:])
 * and mediaserverd died of the NSInvalidArgumentException, every half a minute with Live Photo on, the
 * preview black for a second each time. A NULL is not passed on.
 */
static IMP original_emit_sample_buffer;

static void
emit_sample_buffer(id self, SEL selector, CMSampleBufferRef sample_buffer)
{
	static bool said;

	if (sample_buffer == NULL) {
		if (!said) {
			said = true;
			say("hakios: mediaserverd: a node's output was given no sample buffer (the silent audio source's): not passed on\n");
		}
		return;
	}
	if (ended_outputs != NULL) {
		bool drop = false;

		pthread_mutex_lock(&ended_lock);
		/* until its node is started again (audio_source_start takes its outputs out of the set) */
		drop = CFSetContainsValue(ended_outputs, (const void *)self);
		pthread_mutex_unlock(&ended_lock);
		if (drop) {
			static bool said_drop;

			if (!said_drop) {
				said_drop = true;
				say("hakios: mediaserverd: a buffer for an output already ended (the audio source after its stop): dropped\n");
			}
			return;
		}
	}
	time_photo_encoder(self, sample_buffer);
	/*
	 * How far behind real time the buffers go out: the host clock now less the buffer's presentation time, every 100th
	 * audio buffer and every 150th video one (the audio source's queue ran ~15 s late in step 85/86: is its audio late too?)
	 */
	{
		CMFormatDescriptionRef f = CMSampleBufferGetFormatDescription(sample_buffer);
		CMMediaType type = f != NULL ? CMFormatDescriptionGetMediaType(f) : 0;
		static unsigned audio_n, video_n;
		unsigned n = type == kCMMediaType_Audio ? ++audio_n : type == kCMMediaType_Video ? ++video_n : 0;

		if ((type == kCMMediaType_Audio && n % 100 == 1) || (type == kCMMediaType_Video && n % 150 == 1)) {
			CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample_buffer);
			double now = CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock()));
			id node = send0(self, "node");
			id name = node != nil ? send0(node, "name") : nil;

			say("hakios: mediaserverd: lag: %s buffer %u from %s: pts %.3f s, %.3f s behind the host clock\n",
			    type == kCMMediaType_Audio ? "audio" : "video", n, name != nil ?
			    ((const char *(*)(id, SEL))objc_msgSend)(name, sel_registerName("UTF8String")) : "?",
			    CMTimeGetSeconds(pts), now - CMTimeGetSeconds(pts));
		}
	}
	((void (*)(id, SEL, CMSampleBufferRef))original_emit_sample_buffer)(self, selector, sample_buffer);
}

/*
 * The photograph's encoder. Camera.app asks for HEIC -- HEVC inside a HEIF file, which CMPhoto makes
 * through VideoToolbox -- and VideoToolbox here has no HEVC encoder but the iPhone's hardware ones
 * (com.apple.videotoolbox.videoencoder.hevc and .hevc.gva, behind its AVE): the photo encoder node
 * failed with kVTCouldNotFindVideoEncoderErr (-12908). Apple's software HEVC codec is in the system
 * all the same, VCPHEVC.videocodec, whose decoder VideoToolbox takes here for every HEIC picture (see
 * the top), and there is an encoder in it, HEVCVideoEncoder_CreateInstance, that Darwin never registers:
 * the codec's VCPHEVCRegisterEncoder is an empty function. It is registered here, as the codec
 * registers its decoder (VTRegisterVideoDecoderWithInfo, not hardware accelerated).
 */

/*
 * -[BWGraph stop:] waits ten seconds for its sink nodes to go idle and then traps, after logging which are still active
 * (-[BWGraph _logActiveSinkNodesAfterGraphStopTimeout]) at a level this console never shows. That logging is ours instead:
 * every sink node, its class and name, and for each input whether it has had its end of data, with the node above it.
 */
static const char *
class_of(id o)
{
	return o != nil ? object_getClassName(o) : "nil";
}

static id
send0(id o, const char *sel)
{
	SEL s = sel_registerName(sel);

	if (o == nil || !((BOOL (*)(id, SEL, SEL))objc_msgSend)(o, sel_registerName("respondsToSelector:"), s)) {
		return nil;
	}
	return ((id (*)(id, SEL))objc_msgSend)(o, s);
}

static id
upstream_node(id in)
{
	id conn = send0(in, "connection"), out = nil;
	const char *sels[] = { "output", "sourceOutput", "upstreamOutput", "producer" };

	for (unsigned k = 0; out == nil && k < sizeof(sels) / sizeof(sels[0]); k++) {
		out = send0(conn, sels[k]);
	}
	if (out == nil) {
		out = send0(in, "upstreamOutput");
	}
	return send0(out, "node");
}

static void
node_name(id node, char *nm, size_t size)
{
	id name = send0(node, "name");

	nm[0] = 0;
	if (name == nil || ((long (*)(id, SEL))objc_msgSend)(name, sel_registerName("length")) == 0) {
		name = send0(node, "displayName");
	}
	if (name != nil) {
		CFStringGetCString((CFStringRef)name, nm, (CFIndex)size, kCFStringEncodingUTF8);
	}
}

/* a node, its inputs' and outputs' liveness, and (depth permitting) the nodes above each input; seen: printed already */
static void
walk_up(id node, int depth, CFMutableSetRef seen)
{
	char nm[96];
	id inputs = send0(node, "inputs"), outputs = send0(node, "outputs");
	long ni = inputs != nil ? ((long (*)(id, SEL))objc_msgSend)(inputs, sel_registerName("count")) : 0;
	long no = outputs != nil ? ((long (*)(id, SEL))objc_msgSend)(outputs, sel_registerName("count")) : 0;
	char live_in[40] = "", live_out[40] = "";

	if (node == nil || CFSetContainsValue(seen, (const void *)node)) {
		return;
	}
	CFSetAddValue(seen, (const void *)node);
	node_name(node, nm, sizeof(nm));
	for (long j = 0; j < ni && j < 32; j++) {
		id in = ((id (*)(id, SEL, long))objc_msgSend)(inputs, sel_registerName("objectAtIndex:"), j);
		live_in[j] = send0(in, "liveFormat") != nil ? 'L' : '.';
	}
	for (long j = 0; j < no && j < 32; j++) {
		id out = ((id (*)(id, SEL, long))objc_msgSend)(outputs, sel_registerName("objectAtIndex:"), j);
		live_out[j] = send0(out, "liveFormat") != nil ? 'L' : '.';
	}
	say("hakios: mediaserverd: %*s%s \"%s\": inputs [%s] outputs [%s] (L live, . ended)\n", 2 * depth + 2, "", class_of(node), nm,
	    live_in, live_out);
	if (depth >= 8) {
		return;
	}
	for (long j = 0; j < ni; j++) {
		id in = ((id (*)(id, SEL, long))objc_msgSend)(inputs, sel_registerName("objectAtIndex:"), j);

		walk_up(upstream_node(in), depth + 1, seen);
	}
}

static void
log_active_sinks(id self, SEL selector)
{
	Ivar iv = class_getInstanceVariable(object_getClass(self), "_sinkNodes");
	id sinks = iv != NULL ? object_getIvar(self, iv) : nil;
	CFMutableSetRef seen = CFSetCreateMutable(NULL, 0, NULL);

	(void)selector;
	say("hakios: mediaserverd: the graph's stop timed out; the sinks still live, and every node above them:\n");
	if (sinks == nil || seen == NULL) {
		return;
	}
	long n = ((long (*)(id, SEL))objc_msgSend)(sinks, sel_registerName("count"));

	for (long i = 0; i < n; i++) {
		id node = ((id (*)(id, SEL, long))objc_msgSend)(sinks, sel_registerName("objectAtIndex:"), i);
		id inputs = send0(node, "inputs");
		long ni = inputs != nil ? ((long (*)(id, SEL))objc_msgSend)(inputs, sel_registerName("count")) : 0;
		bool live = false;

		for (long j = 0; j < ni; j++) {
			id in = ((id (*)(id, SEL, long))objc_msgSend)(inputs, sel_registerName("objectAtIndex:"), j);
			live = live || send0(in, "liveFormat") != nil;
		}
		if (live) {
			walk_up(node, 0, seen);
		}
	}
	CFRelease(seen);
}

static void
register_hevc_encoder(void)
{
	void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	void *create = codec != NULL ? dlsym(codec, "HEVCVideoEncoder_CreateInstance") : NULL;
	int (*register_encoder)(uint32_t, CFDictionaryRef, void *) = vt != NULL ? dlsym(vt, "VTRegisterVideoEncoderWithInfo") : NULL;
	CFStringRef *id_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderID") : NULL;
	CFStringRef *name_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderName") : NULL;
	CFStringRef *hardware_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated") : NULL;
	int status;

	if (create == NULL || register_encoder == NULL || id_key == NULL || name_key == NULL || hardware_key == NULL) {
		say("hakios: mediaserverd: no software HEVC encoder to register (%s)\n", codec == NULL ? "no VCPHEVC.videocodec" :
		    create == NULL ? "no HEVCVideoEncoder_CreateInstance" : "VideoToolbox without VTRegisterVideoEncoderWithInfo");
		return;
	}
	const void *keys[] = { *id_key, *name_key, *hardware_key };
	const void *values[] = { CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"),
		kCFBooleanFalse };
	CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);

	status = register_encoder('hvc1', info, create);
	CFRelease(info);
	say("hakios: mediaserverd: the software HEVC encoder of VCPHEVC.videocodec registered for 'hvc1' (%d)\n", status);
}

struct method_spec {
	const char *selector, *types;
	IMP imp;
};

static Class
make_class(const char *name, const struct method_spec *methods, size_t count)
{
	Class cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);

	for (size_t i = 0; i < count; i++) {
		class_addMethod(cls, sel_registerName(methods[i].selector), methods[i].imp, methods[i].types);
	}
	objc_registerClassPair(cls);
	return cls;
}

static void
flashlight_classes(void)
{
	static const struct method_spec vendor[] = {
		{ "registerClientWithPID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
		  "deviceSharingWithOtherClientsAllowed:deviceAvailabilityChangedHandler:", "i@:i@iBB@", (IMP)flashlight_register },
		{ "copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:", "@@:iB^i", (IMP)flashlight_copy_device },
		{ "copyStreamForFlashlightWithPosition:deviceType:forDevice:", "@@:ii@", (IMP)flashlight_copy_stream },
		{ "takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:", "v@:@iB", (IMP)flashlight_take_back },
		{ "activeDeviceEquals:", "B@:@", (IMP)flashlight_active_equals },
	}, stream[] = {
		{ "supportedProperties", "@@:", (IMP)flashlight_stream_properties },
		{ "setProperty:value:", "i@:@@", (IMP)flashlight_stream_set },
	};

	vendor_class = make_class("HakiosFlashlightVendor", vendor, sizeof(vendor) / sizeof(vendor[0]));
	device_class = make_class("HakiosFlashlightDevice", NULL, 0);
	stream_class = make_class("HakiosFlashlightStream", stream, sizeof(stream) / sizeof(stream[0]));
}

/*
 * The class methods replaced, each as soon as its class exists. `expected` is the unslid address
 * of the code in Darwin userland 16.5 (20F66): a method that is not what it was is left alone. `original`, where
 * there is one, keeps what was there (the vendor's: given as it is wherever there is one, and where
 * there is none, ours only with the kernel's camera or flashlight).
 */
static struct replacement {
	const char *class_name, *selector;
	uintptr_t expected;
	IMP replacement, *original;
	bool instance;                  /* an instance method, not a class method */
	bool done;
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0x1a2115c0cUL,
	  (IMP)prewarm_portrait_filter, NULL, false, false },
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendor", 0x1a1ef0d04UL,
	  (IMP)shared_vendor, &original_shared_vendor, false, false },
	{ "BWAudioSourceNode", "start:", 0x1a1eefa18UL, (IMP)audio_source_start, &original_audio_source_start, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod", 0x1a1e99ef8UL,
	  (IMP)video_stabilization_method, NULL, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationType", 0x1a20d8f48UL,
	  (IMP)video_stabilization_type, NULL, true, false },
	{ "FigCaptureStillImageSettings", "qualityPrioritization", 0x1a1f533dcUL,
	  (IMP)quality_prioritization, &original_quality_prioritization, true, false },
	{ "BWGNRNode", "renderSampleBuffer:forInput:", 0x1a1ec83b8UL, (IMP)gnr_render, NULL, true, false },
	{ "BWNodeOutput", "emitSampleBuffer:", 0x1a1ea37e0UL, (IMP)emit_sample_buffer, &original_emit_sample_buffer, true, false },
};

/* ---- the Metal device leaves this process before the capture stack asks for it -------------------- */

/*
 * With the Metal properties on the accelerator service (boot-arg metal=1) MTLCreateSystemDefaultDevice()
 * answers in every process, and in mediaserverd the first to ask is the capture stack: CMCapture's
 * FigPreloadShadersInternal reaches -[_MTLDevice newLibraryWithURL:error:] through three functions of
 * AppleCVAPhoto, asking for System/Library/PrivateFrameworks/AppleCVAPhoto.framework/Resources/
 * cvadepth-iOS.metallib. That file is not in this image (sysimage/select-system.py drops .metallib under
 * the framework directories), so the URL is nil and Metal aborts on `url must not be nil' -- twenty-eight
 * times in one boot, launchd restarting the daemon after each (logs/135751-run.log, docs/metal.md 10.38).
 * AppleCVAPhoto itself survives a library it cannot make: it checks the result and logs "Error creating
 * metal library". What kills the process is Metal's own assertion, and that is reached only because there
 * IS a device to send the message to.
 *
 * So the device is taken out of this process, exactly as inject.c does for the window server: Metal keeps
 * every device it has made in one static NSMutableArray (0x1e1efe9a0, __DATA_DIRTY.__bss, so one per
 * process), and MTLCreateSystemDefaultDevice() is no more than "if that array holds exactly one object,
 * return it" -- an empty array answers nil without a single instruction of Metal's being touched. The
 * array is built on first use, so it has to be asked for first; that makes the device object, which is
 * what the capture stack's next instruction would have done anyway, so the process pays nothing new.
 * The device is retained and never released, for inject.c's reason: what our -dealloc would do to a GPU
 * is not measured.
 *
 * WHEN, and why not where inject.c does it. There the hook is -[CAWindowServer _detectDisplays], the last
 * Objective-C method on the way in and an address that can be checked. Here the way in is three C
 * functions of AppleCVAPhoto with no method of ours to stand on, so this runs from the add-image notifier
 * this library already registers, the first time Metal is in the process at all -- which is before any
 * code of the image that brought it has run. Being early cannot cost correctness: if the device can be
 * made, it is made and removed; if the accelerator service is not matched yet and the array is built
 * empty, nil is what the capture stack gets, and nil is the whole point.
 *
 * Boot-arg mediamtl=1 leaves the device where it is. It is a switch of its own and not inject.c's camtl=1
 * because the two processes do not risk the same thing: a window server that aborts is a crash loop and a
 * black screen, while mediaserverd aborting costs HEIC decoding, the camera and the sound. A boot may
 * reasonably want Metal in one and not the other. Either way the metallib has to be in the image first,
 * or the same assertion comes straight back.
 */
#define MTL_DEVICE_ARRAY        0x1e1efe9a0UL   /* Metal's static NSMutableArray *_deviceArray */

extern id objc_retain(id object);

static bool metal_left_alone;                   /* boot-arg mediamtl=1 */

static bool
boot_arg(const char *what)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;

	return sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0 &&
	    strstr(arguments, what) != NULL;
}

static void
keep_metal_from_the_capture_stack(void)
{
	static bool done;
	unsigned long (*ask)(id, SEL) = (unsigned long (*)(id, SEL))objc_msgSend;
	void (*tell)(id, SEL) = (void (*)(id, SEL))objc_msgSend;
	id *slot = (id *)(MTL_DEVICE_ARRAY + slide);
	id (*create)(void);
	id device, array;
	unsigned long held;

	if (done || metal_left_alone) {
		return;
	}
	/* Metal is not in this process yet: the image that brings it has not arrived, and the notifier
	 * will be back here when it does. Nothing is printed for that -- a line per image would bury
	 * the log, and the constructor has already said that this is armed. */
	if ((create = (id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice")) == NULL) {
		return;
	}
	done = true;
	if ((device = create()) == NULL) {
		say("hakios: mediaserverd: no Metal device in this process, so the capture stack's shader "
		    "preload has nothing to ask\n");
		return;
	}
	if ((array = *slot) == NULL) {
		say("hakios: mediaserverd: a device, but nothing at %#lx; the device list is NOT emptied\n",
		    MTL_DEVICE_ARRAY);
		return;
	}
	if (!((BOOL (*)(id, SEL, Class))objc_msgSend)(array, sel_registerName("isKindOfClass:"),
	    objc_getClass("NSMutableArray"))) {
		say("hakios: mediaserverd: %#lx holds a %s, not Metal's device array; left alone\n",
		    MTL_DEVICE_ARRAY, object_getClassName(array));
		return;
	}
	held = ask(array, sel_registerName("count"));
	objc_retain(device);
	tell(array, sel_registerName("removeAllObjects"));
	say("hakios: mediaserverd: %s taken out of the capture stack (%lu in the list, %lu now); "
	    "MTLCreateSystemDefaultDevice() = %s\n", object_getClassName(device), held,
	    ask(array, sel_registerName("count")), create() != NULL ? "A DEVICE STILL" : "nil");
}

/*
 * The hardware video decoder (mfc_decoder.c, only where the kernel has one: mfc=5). The kernel makes /dev/hakios-mfc when its firmware is up, which
 * may be after this process starts; a thread waits a minute for it and goes.
 */
extern void hakios_mfc_decoder_register(void);
extern void hakios_mfc_encoder_register(void);

static void *
wait_for_mfc(void *unused)
{
	(void)unused;
	for (int i = 0; i < 60; i++) {
		if (access("/dev/hakios-mfc", F_OK) == 0) {
			hakios_mfc_decoder_register();
			break;
		}
		sleep(1);
	}
	return NULL;
}


/*
 * The microphone's source node does not mark its outputs ended when the graph stops it (seen 14 November 2026, at the stop's
 * timeout: the movie's staging node had its video input ended and its audio input -- Audio Splitter (Back), from
 * BWAudioSourceNode -- still live, so the movie file sink never went idle, the graph waited its ten seconds and trapped, and the
 * movie was never finished). Its stop is wrapped: half a second after the node's own stop, every output of it still live is
 * marked ended. A node that ended its outputs itself is left as it is.
 */
static IMP original_audio_source_stop;

static const void *
kept_retain(__unused CFAllocatorRef allocator, const void *value)
{
	return CFRetain(value);
}

static void
kept_release(__unused CFAllocatorRef allocator, const void *value)
{
	CFRelease(value);
}

static void
audio_source_end(void *context)
{
	id node = (id)context;
	id outputs = send0(node, "outputs");
	long n = outputs != nil ? ((long (*)(id, SEL))objc_msgSend)(outputs, sel_registerName("count")) : 0, ended = 0;

	for (long i = 0; i < n; i++) {
		id out = ((id (*)(id, SEL, long))objc_msgSend)(outputs, sel_registerName("objectAtIndex:"), i);

		if (send0(out, "liveFormat") != nil) {
			pthread_mutex_lock(&ended_lock);
			if (ended_outputs == NULL) {
				/*
				 * Each output kept (retained) while in the set: a node freed with its graph and another's output made
				 * at the same address would have had its buffers dropped (the video's, after a photo<->video switch).
				 */
				static const CFSetCallBacks kept = { 0, kept_retain, kept_release, NULL, NULL, NULL };

				ended_outputs = CFSetCreateMutable(NULL, 0, &kept);
			}
			CFSetAddValue(ended_outputs, (const void *)out);
			if (ends_owed == NULL) {
				ends_owed = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, NULL);
			}
			CFDictionarySetValue(ends_owed, (const void *)out,
			    (const void *)((uintptr_t)CFDictionaryGetValue(ends_owed, (const void *)out) + 1));
			pthread_mutex_unlock(&ended_lock);
			ending_here = true;
			((void (*)(id, SEL))objc_msgSend)(out, sel_registerName("markEndOfLiveOutput"));
			ending_here = false;
			ended++;
		}
	}
	say("hakios: mediaserverd: the audio source stopped: %ld of its %ld outputs still live, marked ended\n", ended, n);
	CFRelease((CFTypeRef)node);
}

static BOOL
audio_source_stop(id self, SEL selector, id *error)
{
	BOOL r = ((BOOL (*)(id, SEL, id *))original_audio_source_stop)(self, selector, error);

	/*
	 * At once, inside the stop, while the graph is still whole: a delayed end (it was half a second) landed after a quick
	 * photo/video switch had torn the graph down, and its end-of-data marker went to a freed node (SIGSEGV, 14 November).
	 */
	pthread_mutex_lock(&ended_lock);
	if (stopped_sources == NULL) {
		stopped_sources = CFSetCreateMutable(NULL, 0, NULL);
	}
	CFSetAddValue(stopped_sources, (const void *)self);
	pthread_mutex_unlock(&ended_lock);
	CFRetain((CFTypeRef)self);
	audio_source_end((void *)self);
	/*
	 * The node's own "stopped" flag (_endOfDataMarkerSent, which its stop's block sets after ending the outputs -- run some 15 s
	 * late on the node's queue here) set now: everything still queued for the node checks it first and does nothing. Without
	 * it the silence path ran on a stopped node, read its _auOutputFormatDescription, already NULL, and died (SIGSEGV in a
	 * CMAudioFormatDescriptionGetStreamBasicDescription caller, 14 s into a recording, step 86, 16 November). Nothing in
	 * CMCapture ever clears the flag: a node stopped is not started again (a new graph makes new nodes).
	 */
	{
		Ivar flag = class_getInstanceVariable(object_getClass(self), "_endOfDataMarkerSent");
		static bool said;

		if (flag != NULL) {
			__atomic_store_n((uint8_t *)((uintptr_t)self + (uintptr_t)ivar_getOffset(flag)), 1, __ATOMIC_RELEASE);
		}
		if (!said) {
			said = true;
			say("hakios: mediaserverd: the audio source's _endOfDataMarkerSent set at its stop (%s)\n", flag != NULL ? "done" :
			    "no such ivar: left alone");
		}
	}
	return r;
}


/*
 * The source node's sample generator (-[BWAudioSourceNode _generateSamples], run by a timer) kept running after the stop and
 * emitted into a graph already torn down -- the next session's start found mediaserverd dead of SIGSEGV in the emit (15
 * November). A node stopped and not started again generates nothing.
 */
static IMP original_generate_samples;

static void
generate_samples(id self, SEL selector)
{
	bool stopped;

	pthread_mutex_lock(&ended_lock);
	stopped = stopped_sources != NULL && CFSetContainsValue(stopped_sources, (const void *)self);
	pthread_mutex_unlock(&ended_lock);
	if (stopped) {
		static bool said;

		if (!said) {
			said = true;
			say("hakios: mediaserverd: the audio source's generator ran after its stop: nothing generated\n");
		}
		return;
	}
	((void (*)(id, SEL))original_generate_samples)(self, selector);
}

/*
 * With the microphone working (abox_mic=1), the node's audio unit still delivers a buffer or two after the stop, and each one
 * runs -[BWAudioSourceNode makeOutputsLiveIfNeeded], which, on a queue of its own, makes the outputs live again -- the outputs
 * audio_source_end has just ended -- and sends the live message to the node downstream, which the graph has freed by then
 * (SIGSEGV in objc_msgSend from -[BWNodeOutput makeConfiguredFormatLive], a second after a 10 s recording's stop, 15 November;
 * the movie never finished). A node stopped and not started again makes nothing live.
 */
static IMP original_make_outputs_live;

static void
make_outputs_live(id self, SEL selector)
{
	bool stopped;

	pthread_mutex_lock(&ended_lock);
	stopped = stopped_sources != NULL && CFSetContainsValue(stopped_sources, (const void *)self);
	pthread_mutex_unlock(&ended_lock);
	if (stopped) {
		static int said;

		if (said++ < 3) {
			say("hakios: mediaserverd: the audio source was asked to make its outputs live after its stop: not done\n");
		}
		return;
	}
	((void (*)(id, SEL))original_make_outputs_live)(self, selector);
}

/*
 * Not enough, that (step 84, 15 November): a makeOutputsLiveIfNeeded queued BEFORE the stop still runs after it; its block
 * (0x1a1efda18) finds the outputs audio_source_end has just ended -- liveFormat nil -- and calls -[BWNodeOutput
 * makeConfiguredFormatLive] on each, which sends consumeMessage:fromOutput: to the freed node downstream (SIGABRT,
 * "-[__NSCFType consumeMessage:fromOutput:]: unrecognized selector", 3 s after a recording's stop: mediaserverd died
 * while the movie was still being written, and the file was left without its moov atom -- the gray, unplayable movie).
 * So an output this file ended stays ended: its makeConfiguredFormatLive does nothing until its node is started again
 * (audio_source_start takes its outputs out of ended_outputs).
 */
/*
 * And the node ends its outputs itself, after all: -[BWAudioSourceNode stop:] dispatches a block (0x1a1f51d38) that calls
 * markEndOfLiveOutput on each of them -- on the node's queue, which ran it some 15 s after the stop here (step 85, 15
 * November): the photo session's audio node, stopped when the camera switched to video, ended its outputs in the middle
 * of the recording, a second time (audio_source_end had ended them at the stop), and the end-of-data message went to
 * the freed node downstream (SIGSEGV in -[BWNodeOutput markEndOfLiveOutput]; the recording "stopped by itself" at 9 s,
 * the movie unfinished). So for every end audio_source_end made first, the node's own later end of that output is
 * swallowed, once; an end of an output we did not end goes through.
 */
static IMP original_end_live_output;

static void
end_live_output(id self, SEL selector)
{
	bool swallow = false;

	if (!ending_here) {
		pthread_mutex_lock(&ended_lock);
		uintptr_t owed = ends_owed != NULL ? (uintptr_t)CFDictionaryGetValue(ends_owed, (const void *)self) : 0;

		if (owed != 0) {
			swallow = true;
			if (owed == 1) {
				CFDictionaryRemoveValue(ends_owed, (const void *)self);
			} else {
				CFDictionarySetValue(ends_owed, (const void *)self, (const void *)(owed - 1));
			}
		}
		pthread_mutex_unlock(&ended_lock);
	}
	if (swallow) {
		static int said;

		if (said++ < 5) {
			say("hakios: mediaserverd: the audio source's own late end of an output we had ended: swallowed\n");
		}
		return;
	}
	((void (*)(id, SEL))original_end_live_output)(self, selector);
}

static IMP original_make_format_live;

static void
make_format_live(id self, SEL selector)
{
	bool ended = false;

	pthread_mutex_lock(&ended_lock);
	ended = ended_outputs != NULL && CFSetContainsValue(ended_outputs, (const void *)self);
	pthread_mutex_unlock(&ended_lock);
	if (ended) {
		static int said;

		if (said++ < 3) {
			say("hakios: mediaserverd: an output we ended was to be made live again (a block queued before the stop): not done\n");
		}
		return;
	}
	((void (*)(id, SEL))original_make_format_live)(self, selector);
}

static void
wrap_audio_source_stop(void)
{
	static bool done;
	Class cls;
	Method m;
	SEL sel = sel_registerName("stop:");

	if (done || (cls = objc_getClass("BWAudioSourceNode")) == Nil || (m = class_getInstanceMethod(cls, sel)) == NULL) {
		return;
	}
	done = true;
	original_audio_source_stop = method_getImplementation(m);
	/* on BWAudioSourceNode itself (added there if it inherits stop:), never on its superclass */
	class_replaceMethod(cls, sel, (IMP)audio_source_stop, method_getTypeEncoding(m));
	say("hakios: mediaserverd: -[BWAudioSourceNode stop:] wrapped (its outputs marked ended after a stop)\n");
	{
		SEL g = sel_registerName("_generateSamples");
		Method gm = class_getInstanceMethod(cls, g);
		const char *types = gm != NULL ? method_getTypeEncoding(gm) : NULL;

		const char *colon = types != NULL ? strchr(types, ':') : NULL;
		bool no_args = colon != NULL && colon[1] != 0 && strspn(colon + 1, "0123456789") == strlen(colon + 1);

		if (gm != NULL && types != NULL && types[0] == 'v' && no_args) {
			original_generate_samples = method_getImplementation(gm);
			class_replaceMethod(cls, g, (IMP)generate_samples, types);
			say("hakios: mediaserverd: -[BWAudioSourceNode _generateSamples] (%s) wrapped: nothing after a stop\n", types);
		} else {
			say("hakios: mediaserverd: -[BWAudioSourceNode _generateSamples]: %s; left alone\n", gm ? types : "not there");
		}
	}
	{
		SEL l = sel_registerName("makeOutputsLiveIfNeeded");
		Method lm = class_getInstanceMethod(cls, l);
		const char *types = lm != NULL ? method_getTypeEncoding(lm) : NULL;

		if (types != NULL && types[0] == 'v' && strchr(types, ':') != NULL && strspn(strchr(types, ':') + 1, "0123456789") ==
		    strlen(strchr(types, ':') + 1)) {
			original_make_outputs_live = method_getImplementation(lm);
			class_replaceMethod(cls, l, (IMP)make_outputs_live, types);
			say("hakios: mediaserverd: -[BWAudioSourceNode makeOutputsLiveIfNeeded] (%s) wrapped: nothing after a stop\n", types);
		} else {
			say("hakios: mediaserverd: -[BWAudioSourceNode makeOutputsLiveIfNeeded]: %s; left alone\n", lm ? types : "not there");
		}
	}
	{
		Class oc = objc_getClass("BWNodeOutput");
		SEL f = sel_registerName("makeConfiguredFormatLive");
		Method fm = oc != Nil ? class_getInstanceMethod(oc, f) : NULL;
		const char *types = fm != NULL ? method_getTypeEncoding(fm) : NULL;

		if (types != NULL && types[0] == 'v' && strspn(strchr(types, ':') + 1, "0123456789") == strlen(strchr(types, ':') + 1)) {
			original_make_format_live = method_getImplementation(fm);
			class_replaceMethod(oc, f, (IMP)make_format_live, types);
			say("hakios: mediaserverd: -[BWNodeOutput makeConfiguredFormatLive] (%s) wrapped: an output we ended stays ended\n", types);
		} else {
			say("hakios: mediaserverd: -[BWNodeOutput makeConfiguredFormatLive]: %s; left alone\n", fm ? types : "not there");
		}
		SEL m = sel_registerName("markEndOfLiveOutput");
		Method mm = oc != Nil ? class_getInstanceMethod(oc, m) : NULL;
		const char *mt = mm != NULL ? method_getTypeEncoding(mm) : NULL;

		if (mt != NULL && mt[0] == 'v' && strspn(strchr(mt, ':') + 1, "0123456789") == strlen(strchr(mt, ':') + 1)) {
			original_end_live_output = method_getImplementation(mm);
			class_replaceMethod(oc, m, (IMP)end_live_output, mt);
			say("hakios: mediaserverd: -[BWNodeOutput markEndOfLiveOutput] (%s) wrapped: the node's own late end swallowed "
			    "where we ended first\n", mt);
		} else {
			say("hakios: mediaserverd: -[BWNodeOutput markEndOfLiveOutput]: %s; left alone\n", mm ? mt : "not there");
		}
	}
}

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	/* before anything of the image that was just added runs, and this is the only place in this
	 * library that is told about a new image at all */
	keep_metal_from_the_capture_stack();
	wrap_audio_source_stop();
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		r->done = true;
		method = r->instance ? class_getInstanceMethod(cls, sel_registerName(r->selector)) :
		    class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		if (original == NULL || (uintptr_t)original != r->expected + slide) {
			say("hakios: %c[%s %s]: found %#lx, expected %#lx; left alone\n", r->instance ? '-' : '+', r->class_name,
			    r->selector,
			    original ? (uintptr_t)original - slide : 0, r->expected);
			continue;
		}
		if (r->original != NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("hakios: mediaserverd: %c[%s %s] replaced\n", r->instance ? '-' : '+', r->class_name, r->selector);
	}
}


/*
 * mediaserverd has been seen to die with SIGTRAP about nine seconds after the camera stops (10 November 2026), and this phone has no
 * crash reporter. So the daemon says where it was: the fault's signal, the thread, and the frames from the program counter (and the link
 * register, then the frame pointers' chain, kept inside the thread's own stack), each as image, symbol and offset where dyld knows
 * them, else as the plain address (less the shared cache's slide, which `slide' holds, so that it can be looked up in the cache's
 * symbols). Then the default action is put back and the fault happens again, as it would have done: nothing is hidden or
 * handled.
 */
static void
crash_handler(int sig, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;
	uintptr_t pc = 0, lr = 0, fp = 0, lo = 0, hi = 0;
	pthread_t self = pthread_self();
	char thread[64] = "?";

	(void)info;
	pthread_getname_np(self, thread, sizeof(thread));
	if (uc != NULL && uc->uc_mcontext != NULL) {
		pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
		lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
		fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
	}
	hi = (uintptr_t)pthread_get_stackaddr_np(self);
	lo = hi - pthread_get_stacksize_np(self);
	say("hakios: mediaserverd: CRASH, signal %d on thread '%s'; frames follow\n", sig, thread);
	/*
	 * The trap at the end of -[BWGraph stop:] (0x1a1f1fe60 in 20F66's cache) after its ten seconds: the graph is at x19 + 8
	 * there (the stop's block), and its sink nodes are said before the process goes (log_active_sinks; the method that
	 * would have logged them is a direct one, not replaceable through the runtime).
	 */
	if (pc - slide == 0x1a1f1fe60UL && uc != NULL && uc->uc_mcontext != NULL) {
		uintptr_t x19 = (uintptr_t)uc->uc_mcontext->__ss.__x[19];

		if (x19 != 0) {
			id graph = *(id *)(x19 + 8);

			if (graph != nil) {
				log_active_sinks(graph, NULL);
			}
		}
	}
	for (int i = 0; i < 40; i++) {
		uintptr_t addr = i == 0 ? pc : i == 1 ? lr : 0;
		Dl_info d;

		if (i >= 2) {
			if (fp < lo || fp + 16 > hi || (fp & 15) != 0) {
				break;
			}
			addr = ((uintptr_t *)fp)[1];
			fp = ((uintptr_t *)fp)[0];
		}
		if (addr == 0) {
			continue;
		}
		if (dladdr((void *)addr, &d) && d.dli_fname != NULL) {
			const char *base = strrchr(d.dli_fname, '/');

			say("hakios: mediaserverd:   #%d %s %s+%lu (cache address %#lx)\n", i, base ? base + 1 : d.dli_fname,
			    d.dli_sname ? d.dli_sname : "?", d.dli_sname ? (unsigned long)(addr - (uintptr_t)d.dli_saddr) : 0UL,
			    (unsigned long)(addr - slide));
		} else {
			say("hakios: mediaserverd:   #%d %#lx (cache address %#lx)\n", i, (unsigned long)addr, (unsigned long)(addr - slide));
		}
	}
	signal(sig, SIG_DFL);
}

static void
crash_handler_arm(void)
{
	struct sigaction action;
	static const int signals[] = { SIGTRAP, SIGSEGV, SIGBUS, SIGILL, SIGABRT };

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = crash_handler;
	action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
	sigemptyset(&action.sa_mask);
	for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		sigaction(signals[i], &action, NULL);
	}
	say("hakios: mediaserverd: the crash reporter of ours is armed (SIGTRAP, SIGSEGV, SIGBUS, SIGILL, SIGABRT)\n");
}

__attribute__((constructor))
static void
hakios_media_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		return;
	}
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	crash_handler_arm();
	/*
	 * The capture graph says which of its sink nodes it waited for when a stop times out ("Not all sink nodes became idle
	 * after waiting 10s", then a trap) only with its trace category on: FigNote's "bwgraph_trace" in com.apple.coremedia,
	 * read from the preferences when CMCapture first logs. Set here, before that, for finding the node.
	 */
	{
		int one = 1;
		CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &one);

		CFPreferencesSetAppValue(CFSTR("bwgraph_trace"), n, CFSTR("com.apple.coremedia"));
		CFPreferencesAppSynchronize(CFSTR("com.apple.coremedia"));
		CFRelease(n);
		say("hakios: mediaserverd: com.apple.coremedia bwgraph_trace = 1 (the capture graph's trace)\n");
		/*
		 * CMCapture reads the category when it is loaded, before this constructor: its flag itself (FigNote's word in
		 * CMCapture's __DATA, 0x1dd72cb38 in 20F66's cache, before the slide) is set as well.
		 */
		volatile int *flag = (volatile int *)(0x1dd72cb38UL + slide);
		uintptr_t lo = (uintptr_t)_dyld_get_shared_cache_range(&length);

		if ((uintptr_t)flag > lo && (uintptr_t)flag < lo + length) {
			*flag = 1;
			say("hakios: mediaserverd: bwgraph_trace's flag set in CMCapture\n");
		}
	}
	metal_left_alone = boot_arg("mediamtl=1");
	if (metal_left_alone) {
		say("hakios: mediaserverd: mediamtl=1: the capture stack may have the Metal device (it "
		    "needs AppleCVAPhoto's cvadepth-iOS.metallib in the image)\n");
	} else {
		say("hakios: mediaserverd: the Metal device leaves this process as soon as Metal is in "
		    "it (mediamtl=1 leaves it there)\n");
	}
	torch = open("/dev/hakios-torch", O_WRONLY);
	camera = access("/dev/hakios-camera", F_OK) == 0;
	if (torch >= 0) {
		flashlight_classes();
		say("hakios: mediaserverd: the kernel has a flashlight (/dev/hakios-torch)\n");
	}
	if (camera) {
		say("hakios: mediaserverd: the kernel has a camera (/dev/hakios-camera)\n");
		register_hevc_encoder();
	}
	hakios_mfc_encoder_register();          /* (after VCPHEVC's: an HEVC session not a video's size is handed to it) */
	if (boot_arg("mfc=")) {
		pthread_t waiter;

		pthread_create(&waiter, NULL, wait_for_mfc, NULL);
	}
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
}
