/* graphics/host's media players (ADR-0060 e2e1), which minato_graphics_set_providers gives the runtime
   (apkrt_set_media_players, whose contract they keep): one AVPlayer per player.
   - Sources: an http or https URL as it is, which the runtime let through only for an app with INTERNET; a
     descriptor (the runtime's, within the file) and a callback source (the guest's MediaDataSource) through an
     AVAssetResourceLoader of a URL of our own, whose requests are read from the file at the source's offset or asked
     of the guest by READ records, one at a time, on a serial queue that waits for the DATA (or the player's end).
     Their type comes from their first bytes (an ISO BMFF brand, MP3, ADTS, WAVE, FLAC, CAF, AIFF, AMR); another is
     no movie, as AVFoundation reads no other.
   - prepare: the asset's tracks, duration and playability and the tracks' own values, all loaded asynchronously (no
     read on the player's queue waits for the resource loader), then an AVPlayerItem ready to play; PREPARED with the
     first video track's size (0 by 0 without one) and the longest track's end, or ERROR (MEDIA_ERROR_UNKNOWN,
     UNKNOWN_ERROR: Android's for a source it cannot read or reach). The tracks in the asset's order, their language
     (ISO 639-2, "und" without one) and their MIME type from the format's subtype, as Android's extractors name them.
   - Playback: start plays at the speed set (the spectral time-pitch algorithm, so that the pitch stays), pause
     stops; the end posts COMPLETED and pauses, or with looping seeks to 0 and plays on. Volume: the mean of the
     left and the right (no balance). A seek by MediaPlayer's mode as tolerances (previous sync: any before, none
     after; next sync: the reverse; closest sync: any; closest: none), each answered by one SEEK_DONE with the
     position reached, also one that a later seek cancelled.
   - Frames: a video track's frames from an AVPlayerItemVideoOutput ('420v', IOSurface-backed), polled every 8 ms on
     the player's queue: each new one a FRAME with a token of the player's, its time now (CLOCK_UPTIME_RAW, the
     guest's CLOCK_MONOTONIC), waiting for its RENDER, RENDER_WINDOW or DISCARD, drawn by video_frames.h as the
     decoders' are. A frame the runtime refuses (the guest behind) is dropped.
   The runtime calls a player's functions one at a time, on the guest's thread; AVFoundation calls back on threads of
   its own, which the player's serial queue orders. No Objective-C exception leaves a function (an allocation that
   fails answers -ENOMEM, anything else thrown -EIO). ARC, with -fobjc-arc-exceptions. */
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <MediaToolbox/MediaToolbox.h>
#import <QuartzCore/QuartzCore.h>

#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "minato_graphics.h"
#include "video_frames.h"

/* Linux's errno values, which the guest gets. */
enum { linux_eio = 5, linux_enomem = 12, linux_einval = 22 };

/* MediaPlayer's error for a source it cannot read: MEDIA_ERROR_UNKNOWN with UNKNOWN_ERROR. */
static const int32_t media_error_unknown = 1, unknown_error = INT32_MIN;

/* The bytes a read of the source asks for at most, the runtime's limit for a READ. */
static const int64_t max_read = (int64_t)APKRT_MEDIA_PLAYER_MAX_DATA;

/* The scheme of the URLs whose bytes the resource loader reads. */
static NSString *const own_scheme = @"minato-media";

@interface MinatoMediaPlayer : NSObject <AVAssetResourceLoaderDelegate> {
@public
    uint64_t key;  // the runtime's player, for apkrt_media_player_post
    struct apkrt_media_player_source_info source;
    NSURL *url;    // a URL source's
    dispatch_queue_t queue;    // the player's: its AVFoundation call backs, its frames, its calls' state
    dispatch_queue_t loading;  // the resource loader's, serial: it waits for the guest's DATA
    AVURLAsset *asset;
    AVPlayerItem *item;
    AVPlayer *player;
    AVPlayerItemVideoOutput *output;
    dispatch_source_t timer;
    id end_observer;
    BOOL observing;  // the item's status
    BOOL sourced, prepared, gone, looping;
    float speed, volume, volume_left, volume_right;  // volume: their mean
    int32_t session;       // the app's audio session of the player (SET), for its audio effects
    uint64_t audio_source;  // the runtime's apkrt_audio_source for the tap, 0 for none
    BOOL tap_active;        // the tap takes the player's audio (its format is one it handles): it applies the volume
    BOOL playing;
    int64_t duration_us;
    uint32_t width, height;
    NSData *tracks;  // struct apkrt_media_player_track records
    NSMutableDictionary<NSNumber *, id> *waiting;  // the frames' images by their tokens
    uint64_t next_token;
    VTPixelTransferSessionRef transfer;
    uint64_t cropped_window;
    // A callback source's read, under read_lock.
    pthread_mutex_t read_lock;
    pthread_cond_t read_changed;
    uint64_t read_token, read_count;
    NSData *read_bytes;
    BOOL read_done, read_failed;
}
@end

static void item_status(MinatoMediaPlayer *self);

@implementation MinatoMediaPlayer
- (void)dealloc {
    apkrt_audio_source_close(audio_source);
    if (transfer != NULL) {
        VTPixelTransferSessionInvalidate(transfer);
        CFRelease(transfer);
    }
    pthread_cond_destroy(&read_changed);
    pthread_mutex_destroy(&read_lock);
}

/* The item's status, on the player's queue. */
- (void)observeValueForKeyPath:(NSString *)path
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id> *)change
                       context:(void *)context {
    (void)path, (void)object, (void)change, (void)context;
    dispatch_async(queue, ^{ item_status(self); });
}

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSNumber *, MinatoMediaPlayer *> *players;
static uint64_t last_handle;  // handles count up from 1 and are never used again

static int exception_errno(id thrown) {
    return [thrown isKindOfClass:[NSException class]] &&
                   [((NSException *)thrown).name isEqualToString:NSMallocException]
               ? -linux_enomem
               : -linux_eio;
}

static MinatoMediaPlayer *find_player(uint64_t handle) {
    pthread_mutex_lock(&state_lock);
    MinatoMediaPlayer *player = players[@(handle)];
    pthread_mutex_unlock(&state_lock);
    return player;
}

static int64_t microseconds(CMTime time) {
    if (!CMTIME_IS_NUMERIC(time)) return -1;
    return CMTimeConvertScale(time, 1000000, kCMTimeRoundingMethod_RoundHalfAwayFromZero).value;
}

static void post(MinatoMediaPlayer *self, uint32_t type, int32_t ext1, int32_t ext2, int64_t value, uint64_t token) {
    const struct apkrt_media_player_event event = {type, ext1, ext2, 0, value, token};
    if (apkrt_media_player_post(self->key, &event) != 0 && type != APKRT_MEDIA_PLAYER_FRAME) {
        NSLog(@"media player: a record of type %u did not reach the guest", type);
    }
}

// MARK: - The source's bytes

/* Up to `count` bytes of the source at `at`, from the file or asked of the guest: the bytes (fewer at the end), or
   nil for a failure or the player's end. On the loading queue. */
static NSData *read_source(MinatoMediaPlayer *self, int64_t at, int64_t count) {
    if (count > max_read) count = max_read;
    if (self->source.kind == APKRT_MEDIA_PLAYER_FD) {
        if (at >= self->source.length) return [NSData data];
        if (count > self->source.length - at) count = self->source.length - at;
        NSMutableData *bytes = [NSMutableData dataWithLength:(NSUInteger)count];
        const ssize_t got = pread(self->source.fd, bytes.mutableBytes, (size_t)count, self->source.offset + at);
        if (got < 0) return nil;
        bytes.length = (NSUInteger)got;
        return bytes;
    }
    pthread_mutex_lock(&self->read_lock);
    if (self->gone) {
        pthread_mutex_unlock(&self->read_lock);
        return nil;
    }
    const uint64_t token = ++self->read_count;
    self->read_token = token;
    self->read_done = NO;
    self->read_bytes = nil;
    pthread_mutex_unlock(&self->read_lock);
    const struct apkrt_media_player_event event = {APKRT_MEDIA_PLAYER_READ, (int32_t)count, 0, 0, at, token};
    if (apkrt_media_player_post(self->key, &event) != 0) return nil;
    pthread_mutex_lock(&self->read_lock);
    while (!self->read_done && !self->gone) pthread_cond_wait(&self->read_changed, &self->read_lock);
    NSData *bytes = self->read_done && !self->read_failed ? self->read_bytes : nil;
    self->read_token = 0;
    self->read_bytes = nil;
    pthread_mutex_unlock(&self->read_lock);
    return bytes;
}

/* The source's type, from its first bytes: a UTI AVFoundation reads, or nil. */
static NSString *source_type(NSData *head) {
    const uint8_t *b = head.bytes;
    const NSUInteger n = head.length;
    if (n >= 12 && memcmp(b + 4, "ftyp", 4) == 0) {
        if (memcmp(b + 8, "qt  ", 4) == 0) return AVFileTypeQuickTimeMovie;
        if (memcmp(b + 8, "M4A ", 4) == 0) return AVFileTypeAppleM4A;
        if (memcmp(b + 8, "3gp", 3) == 0 || memcmp(b + 8, "3g2", 3) == 0) return AVFileType3GPP;
        return AVFileTypeMPEG4;
    }
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WAVE", 4) == 0) return AVFileTypeWAVE;
    if (n >= 12 && memcmp(b, "FORM", 4) == 0 && (memcmp(b + 8, "AIFF", 4) == 0 || memcmp(b + 8, "AIFC", 4) == 0)) {
        return AVFileTypeAIFF;
    }
    if (n >= 4 && memcmp(b, "fLaC", 4) == 0) return @"org.xiph.flac";
    if (n >= 4 && memcmp(b, "caff", 4) == 0) return AVFileTypeCoreAudioFormat;
    if (n >= 6 && memcmp(b, "#!AMR", 5) == 0) return AVFileTypeAMR;
    if (n >= 3 && memcmp(b, "ID3", 3) == 0) return AVFileTypeMPEGLayer3;
    if (n >= 2 && b[0] == 0xff && (b[1] & 0xf6) == 0xf0) return @"public.aac-audio";  // ADTS
    if (n >= 2 && b[0] == 0xff && (b[1] & 0xe0) == 0xe0) return AVFileTypeMPEGLayer3;
    return nil;
}

- (BOOL)resourceLoader:(AVAssetResourceLoader *)loader
    shouldWaitForLoadingOfRequestedResource:(AVAssetResourceLoadingRequest *)request {
    (void)loader;
    @try {
        @autoreleasepool {
            if (self->gone) {
                [request finishLoadingWithError:[NSError errorWithDomain:NSPOSIXErrorDomain code:ECANCELED
                                                                userInfo:nil]];
                return YES;
            }
            AVAssetResourceLoadingContentInformationRequest *information = request.contentInformationRequest;
            if (information != nil) {
                NSString *type = source_type(read_source(self, 0, 12));
                if (type == nil || self->source.length < 0) {
                    [request finishLoadingWithError:[NSError errorWithDomain:AVFoundationErrorDomain
                                                                        code:AVErrorFileFormatNotRecognized
                                                                    userInfo:nil]];
                    return YES;
                }
                information.contentType = type;
                information.contentLength = self->source.length;
                information.byteRangeAccessSupported = YES;
            }
            AVAssetResourceLoadingDataRequest *data = request.dataRequest;
            if (data != nil) {
                int64_t at = data.requestedOffset;
                const int64_t end = data.requestsAllDataToEndOfResource
                                        ? self->source.length
                                        : at + (int64_t)data.requestedLength;
                while (at < end && !request.isCancelled && !self->gone) {
                    NSData *bytes = read_source(self, at, end - at);
                    if (bytes == nil) {
                        [request finishLoadingWithError:[NSError errorWithDomain:NSPOSIXErrorDomain code:EIO
                                                                        userInfo:nil]];
                        return YES;
                    }
                    if (bytes.length == 0) break;  // the source ended early
                    [data respondWithData:bytes];
                    at += (int64_t)bytes.length;
                }
            }
            if (!request.isCancelled) [request finishLoading];
            return YES;
        }
    } @catch (id thrown) {
        [request finishLoadingWithError:[NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil]];
        return YES;
    }
}

@end

// MARK: - Preparing

/* Android's name of a format's subtype. */
static NSString *mime_of(AVAssetTrack *track) {
    CMFormatDescriptionRef format = (__bridge CMFormatDescriptionRef)track.formatDescriptions.firstObject;
    const FourCharCode subtype = format != NULL ? CMFormatDescriptionGetMediaSubType(format) : 0;
    switch (subtype) {
        case 'avc1': case 'avc3': return @"video/avc";
        case 'hvc1': case 'hev1': return @"video/hevc";
        case 'mp4v': return @"video/mp4v-es";
        case 's263': return @"video/3gpp";
        case 'vp09': return @"video/x-vnd.on2.vp9";
        case 'av01': return @"video/av01";
        case 'mp4a': case 'aac ': return @"audio/mp4a-latm";
        case '.mp3': return @"audio/mpeg";
        case 'ac-3': return @"audio/ac3";
        case 'ec-3': return @"audio/eac3";
        case 'alac': return @"audio/alac";
        case 'flac': return @"audio/flac";
        case 'opus': return @"audio/opus";
        case 'samr': return @"audio/3gpp";
        case 'sawb': return @"audio/amr-wb";
        case 'lpcm': return @"audio/raw";
        case 'tx3g': return @"text/3gpp-tt";
        case 'wvtt': return @"text/vtt";
        default: return @"application/octet-stream";
    }
}

static uint32_t track_type(AVAssetTrack *track) {
    NSString *type = track.mediaType;
    if ([type isEqualToString:AVMediaTypeVideo]) return 1;
    if ([type isEqualToString:AVMediaTypeAudio]) return 2;
    if ([type isEqualToString:AVMediaTypeText]) return 3;
    if ([type isEqualToString:AVMediaTypeSubtitle] || [type isEqualToString:AVMediaTypeClosedCaption]) return 4;
    if ([type isEqualToString:AVMediaTypeMetadata]) return 5;
    return 0;
}

static NSData *track_records(NSArray<AVAssetTrack *> *list) {
    NSMutableData *records = [NSMutableData data];
    for (AVAssetTrack *track in list) {
        if (records.length / sizeof(struct apkrt_media_player_track) >= APKRT_MEDIA_PLAYER_MAX_TRACKS) break;
        struct apkrt_media_player_track record = {0};
        record.type = track_type(track);
        NSString *language = track.languageCode.length == 3 ? track.languageCode : @"und";
        strlcpy(record.language, language.UTF8String, sizeof record.language);
        strlcpy(record.mime, mime_of(track).UTF8String, sizeof record.mime);
        [records appendBytes:&record length:sizeof record];
    }
    return records;
}

static void frames_tick(MinatoMediaPlayer *self);

/* The item ready or failed, on the player's queue. */
static void item_status(MinatoMediaPlayer *self) {
    if (self->gone || self->prepared || self->item == nil) return;
    if (self->item.status == AVPlayerItemStatusReadyToPlay) {
        self->prepared = YES;
        post(self, APKRT_MEDIA_PLAYER_PREPARED, (int32_t)self->width, (int32_t)self->height, self->duration_us, 0);
    } else if (self->item.status == AVPlayerItemStatusFailed) {
        self->prepared = YES;  // nothing more of it
        post(self, APKRT_MEDIA_PLAYER_ERROR, media_error_unknown, unknown_error, 0, 0);
    }
}

/* The end of the item: COMPLETED, or played again from the start with looping. On the player's queue. */
static void item_end(MinatoMediaPlayer *self) {
    if (self->gone) return;
    if (self->looping) {
        AVPlayer *player = self->player;
        const float rate = self->speed;
        [player seekToTime:kCMTimeZero toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero
            completionHandler:^(BOOL finished) {
                if (finished) [player playImmediatelyAtRate:rate];
            }];
        return;
    }
    self->playing = NO;
    post(self, APKRT_MEDIA_PLAYER_COMPLETED, 0, 0, 0, 0);
}

static void tracks_loaded(MinatoMediaPlayer *self);

/* The asset's values loaded, on the player's queue: its tracks' next, or ERROR. */
static void asset_loaded(MinatoMediaPlayer *self) {
    if (self->gone) return;
    AVURLAsset *asset = self->asset;
    NSError *error = nil;
    for (NSString *key in @[ @"tracks", @"duration", @"playable" ]) {
        if ([asset statusOfValueForKey:key error:&error] != AVKeyValueStatusLoaded) {
            post(self, APKRT_MEDIA_PLAYER_ERROR, media_error_unknown, unknown_error, 0, 0);
            return;
        }
    }
    if (!asset.playable) {
        post(self, APKRT_MEDIA_PLAYER_ERROR, media_error_unknown, unknown_error, 0, 0);
        return;
    }
    // The tracks' own values load too before any is read, so that no read on the player's queue waits for the
    // resource loader (whose callback source waits for the guest's DATA).
    dispatch_group_t group = dispatch_group_create();
    for (AVAssetTrack *track in asset.tracks) {
        dispatch_group_enter(group);
        [track loadValuesAsynchronouslyForKeys:@[ @"mediaType", @"formatDescriptions", @"languageCode",
                                                  @"naturalSize", @"preferredTransform", @"timeRange" ]
                             completionHandler:^{ dispatch_group_leave(group); }];
    }
    __weak MinatoMediaPlayer *weak = self;
    dispatch_group_notify(group, self->queue, ^{
        MinatoMediaPlayer *strong = weak;
        if (strong != nil) tracks_loaded(strong);
    });
}

/* The player's volume: with the tap on a format it handles the tap applies it, in front of the session's effects as
   Android's mixer does, with a gain for each of the left and the right channel; else AVPlayer does, by the mean. On
   the player's queue. */
static void apply_volume(MinatoMediaPlayer *player) {
    if (player->player == nil) return;
    if (player->audio_source != 0 && player->tap_active) {
        player->player.volume = 1;
        apkrt_audio_source_set_volume(player->audio_source, player->volume_left, player->volume_right);
    } else {
        player->player.volume = player->volume;
        apkrt_audio_source_set_volume(player->audio_source, 1, 1);
    }
}

/* What the tap's callbacks, on AVFoundation's threads, hold of their player: a weak reference. */
@interface MinatoTapOwner : NSObject {
@public
    __weak MinatoMediaPlayer *player;
}
@end
@implementation MinatoTapOwner
@end

/* The audio tap: the player's audio track, before the player's speed and volume, passes through the runtime's audio
   effects of the player's session (apkrt_audio_source_process) and into the Visualizer's tap. A format the tap cannot
   take (not 32-bit float, more channels than the effects have) is left as it is. */
typedef struct {
    uint64_t source;
    CFTypeRef owner;  // a MinatoTapOwner (retained), through which the player hears whether the tap takes its audio
    BOOL usable, interleaved;
    UInt32 channels, capacity;  // the channels per frame; the frames `scratch` holds
    double rate;
    float *scratch;  // a planar format's frames, interleaved for the effects
} MinatoAudioTap;

/* Tells the player whether the tap takes its audio now, on the player's queue. */
static void audio_tap_report(MinatoAudioTap *state, BOOL active) {
    MinatoTapOwner *owner = (__bridge MinatoTapOwner *)state->owner;
    MinatoMediaPlayer *player = owner->player;
    if (player == nil) return;
    dispatch_async(player->queue, ^{
        if (player->gone) return;
        player->tap_active = active;
        apply_volume(player);
    });
}

static void audio_tap_init(MTAudioProcessingTapRef tap, void *client, void **storage) {
    (void)tap;
    *storage = client;
}

static void audio_tap_finalize(MTAudioProcessingTapRef tap) {
    MinatoAudioTap *state = MTAudioProcessingTapGetStorage(tap);
    free(state->scratch);
    if (state->owner != NULL) CFRelease(state->owner);
    free(state);
}

static void audio_tap_prepare(MTAudioProcessingTapRef tap, CMItemCount max_frames,
                              const AudioStreamBasicDescription *format) {
    MinatoAudioTap *state = MTAudioProcessingTapGetStorage(tap);
    state->usable = format->mFormatID == kAudioFormatLinearPCM &&
                    (format->mFormatFlags & kAudioFormatFlagIsFloat) != 0 && format->mBitsPerChannel == 32 &&
                    format->mChannelsPerFrame >= 1 && format->mChannelsPerFrame <= 8;
    state->interleaved = (format->mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0;
    state->channels = format->mChannelsPerFrame;
    state->rate = format->mSampleRate;
    if (state->usable && !state->interleaved && max_frames > 0 && max_frames <= 1 << 16) {
        state->capacity = (UInt32)max_frames;
        state->scratch = malloc((size_t)state->capacity * state->channels * sizeof(float));
    }
    if (!state->interleaved && state->scratch == NULL) state->usable = NO;
    audio_tap_report(state, state->usable);
}

static void audio_tap_unprepare(MTAudioProcessingTapRef tap) {
    MinatoAudioTap *state = MTAudioProcessingTapGetStorage(tap);
    state->usable = NO;
    audio_tap_report(state, NO);
    free(state->scratch);
    state->scratch = NULL;
    state->capacity = 0;
}

static void audio_tap_process(MTAudioProcessingTapRef tap, CMItemCount frames, MTAudioProcessingTapFlags flags,
                              AudioBufferList *buffers, CMItemCount *frames_out,
                              MTAudioProcessingTapFlags *flags_out) {
    (void)frames;
    (void)flags;
    if (MTAudioProcessingTapGetSourceAudio(tap, frames, buffers, flags_out, NULL, frames_out) != noErr) return;
    MinatoAudioTap *state = MTAudioProcessingTapGetStorage(tap);
    const UInt32 count = (UInt32)*frames_out;
    if (!state->usable || count == 0) return;
    if (state->interleaved) {
        if (buffers->mNumberBuffers != 1) return;
        apkrt_audio_source_process(state->source, buffers->mBuffers[0].mData, count, (int32_t)state->channels,
                                   state->rate);
        return;
    }
    if (buffers->mNumberBuffers != state->channels || count > state->capacity) return;
    for (UInt32 channel = 0; channel < state->channels; ++channel) {
        const float *plane = buffers->mBuffers[channel].mData;
        for (UInt32 frame = 0; frame < count; ++frame) state->scratch[frame * state->channels + channel] = plane[frame];
    }
    apkrt_audio_source_process(state->source, state->scratch, count, (int32_t)state->channels, state->rate);
    for (UInt32 channel = 0; channel < state->channels; ++channel) {
        float *plane = buffers->mBuffers[channel].mData;
        for (UInt32 frame = 0; frame < count; ++frame) plane[frame] = state->scratch[frame * state->channels + channel];
    }
}

/* Puts the tap on `item`'s first audio track, with a source of the runtime for the player's session; nothing changes
   for an asset without an audio track or when the tap cannot be made. */
static void attach_audio_tap(MinatoMediaPlayer *self, AVPlayerItem *item, NSArray<AVAssetTrack *> *tracks) {
    AVAssetTrack *audio = nil;
    for (AVAssetTrack *track in tracks) {
        if ([track.mediaType isEqualToString:AVMediaTypeAudio]) {
            audio = track;
            break;
        }
    }
    if (audio == nil) return;
    const uint64_t source = apkrt_audio_source_open(self->session);
    if (source == 0) return;
    MinatoAudioTap *state = calloc(1, sizeof *state);
    if (state == NULL) {
        apkrt_audio_source_close(source);
        return;
    }
    state->source = source;
    MinatoTapOwner *owner = [[MinatoTapOwner alloc] init];
    owner->player = self;
    state->owner = (__bridge_retained CFTypeRef)owner;
    MTAudioProcessingTapCallbacks callbacks = {kMTAudioProcessingTapCallbacksVersion_0, state, audio_tap_init,
                                               audio_tap_finalize, audio_tap_prepare, audio_tap_unprepare,
                                               audio_tap_process};
    MTAudioProcessingTapRef tap = NULL;
    const OSStatus made =
        MTAudioProcessingTapCreate(kCFAllocatorDefault, &callbacks, kMTAudioProcessingTapCreationFlag_PreEffects, &tap);
    if (made != noErr) {
        CFRelease(state->owner);
        free(state);
        apkrt_audio_source_close(source);
        return;
    }
    AVMutableAudioMixInputParameters *parameters =
        [AVMutableAudioMixInputParameters audioMixInputParametersWithTrack:audio];
    parameters.audioTapProcessor = tap;
    CFRelease(tap);
    AVMutableAudioMix *mix = [AVMutableAudioMix audioMix];
    mix.inputParameters = @[ parameters ];
    item.audioMix = mix;
    self->audio_source = source;
}

/* The tracks' values loaded, on the player's queue: the item. */
static void tracks_loaded(MinatoMediaPlayer *self) {
    if (self->gone) return;
    AVURLAsset *asset = self->asset;
    // MPEG4Extractor's and NuPlayer's: the longest track's end, which the movie's duration may fall short of.
    int64_t duration = microseconds(asset.duration);
    for (AVAssetTrack *track in asset.tracks) {
        const int64_t end = microseconds(CMTimeRangeGetEnd(track.timeRange));
        if (end > duration) duration = end;
    }
    self->duration_us = duration;
    self->tracks = track_records(asset.tracks);
    AVAssetTrack *video = nil;
    for (AVAssetTrack *track in asset.tracks) {
        if ([track.mediaType isEqualToString:AVMediaTypeVideo]) {
            video = track;
            break;
        }
    }
    if (video != nil) {
        const CGSize size = CGSizeApplyAffineTransform(video.naturalSize, video.preferredTransform);
        self->width = (uint32_t)fabs(size.width);
        self->height = (uint32_t)fabs(size.height);
        if (self->width > APKRT_VIDEO_DECODER_MAX_DIMENSION || self->height > APKRT_VIDEO_DECODER_MAX_DIMENSION) {
            self->width = self->height = 0;
        }
    }
    AVPlayerItem *item = [AVPlayerItem playerItemWithAsset:asset];
    item.audioTimePitchAlgorithm = AVAudioTimePitchAlgorithmSpectral;
    attach_audio_tap(self, item, asset.tracks);
    if (video != nil) {
        NSDictionary *attributes = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
            (id)kCVPixelBufferMetalCompatibilityKey : @YES,
        };
        self->output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attributes];
        [item addOutput:self->output];
    }
    self->item = item;
    self->player = [AVPlayer playerWithPlayerItem:item];
    self->player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
    apply_volume(self);  // the tap's prepare tells later whether it takes the audio
    if (self->source.kind != APKRT_MEDIA_PLAYER_URL) self->player.automaticallyWaitsToMinimizeStalling = NO;
    __weak MinatoMediaPlayer *weak = self;
    self->end_observer = [[NSNotificationCenter defaultCenter]
        addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                    object:item
                     queue:nil
                usingBlock:^(NSNotification *note) {
                    (void)note;
                    MinatoMediaPlayer *strong = weak;
                    if (strong != nil) dispatch_async(strong->queue, ^{ item_end(strong); });
                }];
    [item addObserver:self forKeyPath:@"status" options:NSKeyValueObservingOptionInitial context:NULL];
    self->observing = YES;
    if (self->output != nil) {
        self->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, self->queue);
        dispatch_source_set_timer(self->timer, DISPATCH_TIME_NOW, 8 * NSEC_PER_MSEC, NSEC_PER_MSEC);
        dispatch_source_set_event_handler(self->timer, ^{
            MinatoMediaPlayer *strong = weak;
            if (strong != nil) frames_tick(strong);
        });
        dispatch_resume(self->timer);
    }
}

/* A new frame of the video output, if one shows now: a FRAME, or dropped when the runtime refuses it. */
static void frames_tick(MinatoMediaPlayer *self) {
    if (self->gone || self->output == nil || !self->prepared) return;
    @autoreleasepool {
        const CMTime now = [self->output itemTimeForHostTime:CACurrentMediaTime()];
        if (![self->output hasNewPixelBufferForItemTime:now]) return;
        CVPixelBufferRef image = [self->output copyPixelBufferForItemTime:now itemTimeForDisplay:NULL];
        if (image == NULL) return;
        if (self->waiting.count >= APKRT_MEDIA_PLAYER_MAX_WAITING) {
            CVPixelBufferRelease(image);
            return;
        }
        const uint64_t token = ++self->next_token;
        self->waiting[@(token)] = (__bridge_transfer id)image;
        const struct apkrt_media_player_event event = {
            APKRT_MEDIA_PLAYER_FRAME, 0, 0, 0, (int64_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000), token};
        if (apkrt_media_player_post(self->key, &event) != 0) [self->waiting removeObjectForKey:@(token)];
    }
}

// MARK: - The runtime's functions

static int create(void *context, uint64_t key, uint64_t *handle) {
    (void)context;
    @try {
        MinatoMediaPlayer *player = [[MinatoMediaPlayer alloc] init];
        player->key = key;
        player->queue = dispatch_queue_create("minato.media-player", DISPATCH_QUEUE_SERIAL);
        player->loading = dispatch_queue_create("minato.media-player.loading", DISPATCH_QUEUE_SERIAL);
        player->speed = 1;
        player->volume = player->volume_left = player->volume_right = 1;
        player->duration_us = -1;
        player->waiting = [NSMutableDictionary dictionary];
        player->source.fd = -1;
        pthread_mutex_init(&player->read_lock, NULL);
        pthread_cond_init(&player->read_changed, NULL);
        pthread_mutex_lock(&state_lock);
        if (players == nil) players = [NSMutableDictionary dictionary];
        *handle = ++last_handle;
        players[@(*handle)] = player;
        pthread_mutex_unlock(&state_lock);
        return 0;
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

static int source(void *context, uint64_t handle, const struct apkrt_media_player_source_info *info) {
    (void)context;
    @try {
        MinatoMediaPlayer *player = find_player(handle);
        if (player == nil || player->sourced) return -linux_einval;
        NSURL *url = nil;
        if (info->kind == APKRT_MEDIA_PLAYER_URL) {
            NSString *text = [[NSString alloc] initWithBytes:info->url length:info->url_size
                                                    encoding:NSUTF8StringEncoding];
            url = text != nil ? [NSURL URLWithString:text] : nil;
            if (url == nil) return -linux_einval;
        } else {
            url = [NSURL URLWithString:[NSString stringWithFormat:@"%@://player/%llu", own_scheme, handle]];
        }
        dispatch_sync(player->queue, ^{
            player->source = *info;
            player->source.url = NULL;  // the runtime's memory only for the call
            player->url = url;
            player->sourced = YES;
        });
        return 0;
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

static int prepare(void *context, uint64_t handle) {
    (void)context;
    @try {
        MinatoMediaPlayer *player = find_player(handle);
        if (player == nil || !player->sourced) return -linux_einval;
        dispatch_sync(player->queue, ^{
            AVURLAsset *asset = [AVURLAsset URLAssetWithURL:player->url
                                                    options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
            if (player->source.kind != APKRT_MEDIA_PLAYER_URL) {
                [asset.resourceLoader setDelegate:player queue:player->loading];
            }
            player->asset = asset;
            __weak MinatoMediaPlayer *weak = player;
            [asset loadValuesAsynchronouslyForKeys:@[ @"tracks", @"duration", @"playable" ]
                                 completionHandler:^{
                                     MinatoMediaPlayer *strong = weak;
                                     if (strong != nil) dispatch_async(strong->queue, ^{ asset_loaded(strong); });
                                 }];
        });
        return 0;
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

/* `work` on the player's queue, once it is prepared: 0, or -EINVAL. */
static int on_player(uint64_t handle, void (^work)(MinatoMediaPlayer *player)) {
    @try {
        MinatoMediaPlayer *player = find_player(handle);
        if (player == nil) return -linux_einval;
        __block int answer = 0;
        dispatch_sync(player->queue, ^{
            @autoreleasepool {
                if (player->player == nil) {
                    answer = -linux_einval;
                    return;
                }
                work(player);
            }
        });
        return answer;
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

static int start(void *context, uint64_t handle) {
    (void)context;
    return on_player(handle, ^(MinatoMediaPlayer *player) {
        player->playing = YES;
        [player->player playImmediatelyAtRate:player->speed];
    });
}

static int pause_player(void *context, uint64_t handle) {
    (void)context;
    return on_player(handle, ^(MinatoMediaPlayer *player) {
        player->playing = NO;
        [player->player pause];
    });
}

static int seek(void *context, uint64_t handle, int64_t time_us, uint32_t mode) {
    (void)context;
    return on_player(handle, ^(MinatoMediaPlayer *player) {
        CMTime before = kCMTimeZero, after = kCMTimeZero;
        if (mode == 0 || mode == 2) before = kCMTimePositiveInfinity;  // previous sync, closest sync
        if (mode == 1 || mode == 2) after = kCMTimePositiveInfinity;   // next sync, closest sync
        AVPlayer *avplayer = player->player;
        __weak MinatoMediaPlayer *weak = player;
        [avplayer seekToTime:CMTimeMake(time_us, 1000000) toleranceBefore:before toleranceAfter:after
            completionHandler:^(BOOL finished) {
                (void)finished;  // each seek's one SEEK_DONE, also one a later seek cancelled
                MinatoMediaPlayer *strong = weak;
                if (strong == nil) return;
                dispatch_async(strong->queue, ^{
                    if (strong->gone) return;
                    const int64_t at = microseconds(avplayer.currentTime);
                    post(strong, APKRT_MEDIA_PLAYER_SEEK_DONE, 0, 0, at < 0 ? 0 : at, 0);
                });
            }];
    });
}

static int status(void *context, uint64_t handle, struct apkrt_media_player_state *state) {
    (void)context;
    MinatoMediaPlayer *found = find_player(handle);
    if (found == nil) return -linux_einval;
    if (found->player == nil) {  // not prepared yet: nothing played
        *state = (struct apkrt_media_player_state){0, -1, 0, 0, 0};
        return 0;
    }
    return on_player(handle, ^(MinatoMediaPlayer *player) {
        const int64_t at = microseconds(player->player.currentTime);
        state->position_us = at < 0 ? 0 : at;
        state->duration_us = player->duration_us;
        state->width = player->width;
        state->height = player->height;
        state->flags = player->player.rate != 0 ? APKRT_MEDIA_PLAYER_PLAYING : 0;
    });
}

static int set(void *context, uint64_t handle, uint32_t key, uint64_t value) {
    (void)context;
    @try {
        MinatoMediaPlayer *player = find_player(handle);
        if (player == nil) return -linux_einval;
        float low = 0, high = 0;
        const uint32_t low_bits = (uint32_t)value, high_bits = (uint32_t)(value >> 32);
        memcpy(&low, &low_bits, sizeof low);
        memcpy(&high, &high_bits, sizeof high);
        dispatch_sync(player->queue, ^{
            switch (key) {
                case APKRT_MEDIA_PLAYER_VOLUME:
                    player->volume_left = low;
                    player->volume_right = high;
                    player->volume = (low + high) / 2;
                    apply_volume(player);
                    break;
                case APKRT_MEDIA_PLAYER_LOOPING: player->looping = value != 0; break;
                case APKRT_MEDIA_PLAYER_SESSION: player->session = (int32_t)(uint32_t)value; break;
                case APKRT_MEDIA_PLAYER_SPEED:
                    player->speed = low;
                    apkrt_audio_source_set_speed(player->audio_source, low);  // its tap's blocks come further apart
                    if (player->playing && player->player.rate != 0) player->player.rate = low;
                    break;
                default: break;
            }
        });
        return 0;
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

static int tracks(void *context, uint64_t handle, struct apkrt_media_player_track *records, uint32_t capacity,
                  uint32_t *count) {
    (void)context;
    @try {
        MinatoMediaPlayer *player = find_player(handle);
        if (player == nil) return -linux_einval;
        __block NSData *all = nil;
        dispatch_sync(player->queue, ^{ all = player->tracks; });
        const uint32_t have = (uint32_t)(all.length / sizeof *records);
        memcpy(records, all.bytes, (have < capacity ? have : capacity) * sizeof *records);
        *count = have;
        return 0;
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

static void data(void *context, uint64_t handle, uint64_t token, const void *bytes, size_t size, uint32_t flags) {
    (void)context;
    @try {
        MinatoMediaPlayer *player = find_player(handle);
        if (player == nil) return;
        NSData *copy = size != 0 ? [NSData dataWithBytes:bytes length:size] : [NSData data];
        pthread_mutex_lock(&player->read_lock);
        if (token == player->read_token && !player->read_done) {
            player->read_bytes = copy;
            player->read_failed = (flags & APKRT_MEDIA_PLAYER_READ_FAILED) != 0;
            player->read_done = YES;
            pthread_cond_broadcast(&player->read_changed);
        }
        pthread_mutex_unlock(&player->read_lock);
    } @catch (id thrown) {
        // Nothing to answer: the read waits for the player's end.
    }
}

/* The waiting frame of `token`, taken; nil for one that does not wait. */
static id take_frame(MinatoMediaPlayer *player, uint64_t token) {
    __block id image = nil;
    dispatch_sync(player->queue, ^{
        image = player->waiting[@(token)];
        [player->waiting removeObjectForKey:@(token)];
    });
    return image;
}

static int render(void *context, uint64_t handle, uint64_t token, const struct apkrt_video_target *target) {
    (void)context;
    @try {
        @autoreleasepool {
            MinatoMediaPlayer *player = find_player(handle);
            // Its only owner from here: kept until the render has read it.
            __attribute__((objc_precise_lifetime)) id image = player != nil ? take_frame(player, token) : nil;
            if (image == nil) return -linux_einval;
            return minato_video_frame_render((__bridge CVPixelBufferRef)image, target, &player->transfer);
        }
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

static int render_window(void *context, uint64_t handle, uint64_t token, uint64_t window, int64_t time_ns,
                         uint32_t flags) {
    (void)context;
    @try {
        @autoreleasepool {
            MinatoMediaPlayer *player = find_player(handle);
            __attribute__((objc_precise_lifetime)) id image = player != nil ? take_frame(player, token) : nil;
            if (image == nil) return -linux_einval;
            return minato_video_frame_render_window((__bridge CVPixelBufferRef)image, window, time_ns, flags,
                                                    &player->cropped_window);
        }
    } @catch (id thrown) {
        return exception_errno(thrown);
    }
}

static void discard(void *context, uint64_t handle, uint64_t token) {
    (void)context;
    @try {
        MinatoMediaPlayer *player = find_player(handle);
        if (player != nil) (void)take_frame(player, token);
    } @catch (id thrown) {
        // Nothing to answer: the token has ended for the runtime either way.
    }
}

static void destroy(void *context, uint64_t handle) {
    (void)context;
    @try {
        pthread_mutex_lock(&state_lock);
        MinatoMediaPlayer *player = players[@(handle)];
        [players removeObjectForKey:@(handle)];
        pthread_mutex_unlock(&state_lock);
        if (player == nil) return;
        pthread_mutex_lock(&player->read_lock);
        player->gone = YES;  // a read that waits ends
        pthread_cond_broadcast(&player->read_changed);
        pthread_mutex_unlock(&player->read_lock);
        dispatch_sync(player->queue, ^{
            if (player->timer != nil) dispatch_source_cancel(player->timer);
            if (player->end_observer != nil) [[NSNotificationCenter defaultCenter] removeObserver:player->end_observer];
            if (player->observing) [player->item removeObserver:player forKeyPath:@"status"];
            player->observing = NO;
            [player->player pause];
            [player->player replaceCurrentItemWithPlayerItem:nil];
            [player->asset cancelLoading];
            [player->waiting removeAllObjects];
            if (player->cropped_window != 0) minato_video_frame_give_back_gravity(player->cropped_window);
            apkrt_audio_source_close(player->audio_source);  // the tap, if it still runs, finds no source
            player->audio_source = 0;
        });
        // The loader's reads of the source end before the runtime closes its descriptor.
        dispatch_sync(player->loading, ^{});
    } @catch (id thrown) {
        // Nothing to answer: the player has gone for the runtime either way.
    }
}

static const struct apkrt_media_players functions = {
    create, source, prepare, start, pause_player, seek, status, set, tracks, data, render, render_window, discard,
    destroy, NULL};

const struct apkrt_media_players *minato_graphics_media_players(void) {
    return &functions;
}

int minato_graphics_set_media_players(void) {
    return apkrt_set_media_players(&functions) == 0 ? 0 : -1;
}
