// The PC's sound on macOS: ScreenCaptureKit's system audio (macOS 13 or later), without the
// streamer's own, through the shared PcAudioSender (48 kHz Opus, as on every OS). It needs no
// picture, but ScreenCaptureKit only captures for a display, so it asks for a tiny one, rarely.
#include "audio/audio.h"

#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#include "audio/audio_sender.h"
#include "rc/platform.h"

namespace rcs {

struct MacAudioCapture {
    std::mutex mutex;  // the sender, between ScreenCaptureKit's queue and Stop
    PcAudioSender sender;
    double rate = 0;
    bool running = true;
    SCStream* stream = nil;
    id sink = nil;
    dispatch_queue_t queue = nil;

    void OnSamples(CMSampleBufferRef sample);
};

}  // namespace rcs

@interface RcAudioSink : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) rcs::MacAudioCapture* capture;
@end

@implementation RcAudioSink
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if (@available(macOS 13.0, *)) {
        if (type == SCStreamOutputTypeAudio && CMSampleBufferIsValid(sample)) self.capture->OnSamples(sample);
    }
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    RC_LOGW("Audio capture stopped: %s", error.localizedDescription.UTF8String);
}
@end

namespace rcs {

// ScreenCaptureKit's audio is float, usually one buffer per channel; interleaved float and 16-bit
// integer are read too.
void MacAudioCapture::OnSamples(CMSampleBufferRef sample) {
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    const AudioStreamBasicDescription* asbd = format ? CMAudioFormatDescriptionGetStreamBasicDescription(format) : nullptr;
    const CMItemCount frames = CMSampleBufferGetNumSamples(sample);
    if (!asbd || frames <= 0) return;

    size_t listSize = 0;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, &listSize, nullptr, 0, nullptr, nullptr,
                                                                kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
                                                                nullptr) != noErr ||
        !listSize)
        return;
    std::vector<uint8_t> storage(listSize);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    CMBlockBufferRef block = nullptr;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, nullptr, list, listSize, nullptr, nullptr,
                                                                kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
                                                                &block) != noErr) {
        if (block) CFRelease(block);
        return;
    }

    const bool isFloat = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    const uint32_t bits = asbd->mBitsPerChannel;
    std::lock_guard lock(mutex);
    if (running) {
        if (asbd->mSampleRate != rate) {
            rate = asbd->mSampleRate;
            sender.SetInputRate(rate);
            RC_LOGI("Streaming the Mac's audio (%.0f Hz)", rate);
        }
        if (list->mNumberBuffers >= 2 && isFloat && bits == 32) {
            const auto* l = static_cast<const float*>(list->mBuffers[0].mData);
            const auto* r = static_cast<const float*>(list->mBuffers[1].mData);
            if (l && r)
                for (CMItemCount i = 0; i < frames; ++i) sender.Push(l[i], r[i]);
        } else if (list->mNumberBuffers == 1 && list->mBuffers[0].mData) {
            const uint32_t ch = std::max<uint32_t>(1, list->mBuffers[0].mNumberChannels);
            if (isFloat && bits == 32) {
                const auto* d = static_cast<const float*>(list->mBuffers[0].mData);
                for (CMItemCount i = 0; i < frames; ++i) sender.Push(d[i * ch], d[i * ch + (ch > 1 ? 1 : 0)]);
            } else if (!isFloat && bits == 16) {
                const auto* d = static_cast<const int16_t*>(list->mBuffers[0].mData);
                for (CMItemCount i = 0; i < frames; ++i)
                    sender.Push(d[i * ch] / 32768.f, d[i * ch + (ch > 1 ? 1 : 0)] / 32768.f);
            }
        }
        sender.Flush();
    }
    if (block) CFRelease(block);
}

bool AudioStreamer::Start(rc::Connection* connection, const wchar_t*, int bitrate) {
    Stop();
    m_connection = connection;
    m_bitrate = bitrate;
    if (@available(macOS 13.0, *)) {
        auto capture = std::make_shared<MacAudioCapture>();
        if (!capture->sender.Init(connection, bitrate)) return false;

        __block SCDisplay* display = nil;
        dispatch_semaphore_t listed = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* error) {
          if (error) RC_LOGW("Audio capture: %s", error.localizedDescription.UTF8String);
          display = content.displays.firstObject;
          dispatch_semaphore_signal(listed);
        }];
        if (dispatch_semaphore_wait(listed, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))) != 0 || !display)
            return false;

        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        SCStreamConfiguration* sc = [[SCStreamConfiguration alloc] init];
        sc.width = 2;
        sc.height = 2;
        sc.minimumFrameInterval = CMTimeMake(1, 1);
        sc.capturesAudio = YES;
        sc.excludesCurrentProcessAudio = YES;
        sc.sampleRate = int(rc::kAudioSampleRate);
        sc.channelCount = 2;

        RcAudioSink* sink = [[RcAudioSink alloc] init];
        sink.capture = capture.get();
        capture->sink = sink;
        capture->stream = [[SCStream alloc] initWithFilter:filter configuration:sc delegate:sink];
        capture->queue = dispatch_queue_create("app.rincast.audio", DISPATCH_QUEUE_SERIAL);
        NSError* error = nil;
        // The picture output too, though nothing is done with it: without one, ScreenCaptureKit
        // logs every frame it has nowhere to put.
        [capture->stream addStreamOutput:sink type:SCStreamOutputTypeScreen sampleHandlerQueue:capture->queue error:nil];
        if (![capture->stream addStreamOutput:sink type:SCStreamOutputTypeAudio sampleHandlerQueue:capture->queue error:&error]) {
            RC_LOGW("Audio capture: %s", error.localizedDescription.UTF8String);
            return false;
        }
        __block bool started = false;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [capture->stream startCaptureWithCompletionHandler:^(NSError* startError) {
          if (startError) RC_LOGW("Audio capture didn't start: %s", startError.localizedDescription.UTF8String);
          started = startError == nil;
          dispatch_semaphore_signal(done);
        }];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))) != 0 || !started)
            return false;
        m_platform = capture;
        m_running = true;
        return true;
    }
    RC_LOGW("Streaming the Mac's audio needs macOS 13 or later");
    return false;
}

void AudioStreamer::Stop() {
    m_running = false;
    auto capture = std::static_pointer_cast<MacAudioCapture>(std::exchange(m_platform, nullptr));
    if (!capture) return;
    if (capture->stream) {
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [capture->stream stopCaptureWithCompletionHandler:^(NSError*) {
          dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC)));
    }
    // Let a callback already queued finish before the capture it points at goes away.
    if (capture->queue) dispatch_sync(capture->queue, ^{});
    std::lock_guard lock(capture->mutex);
    capture->running = false;  // a callback still queued sends nothing (the connection may be gone)
}

void AudioStreamer::Run() {}  // ScreenCaptureKit calls back on its own queue: no thread of ours

}  // namespace rcs
