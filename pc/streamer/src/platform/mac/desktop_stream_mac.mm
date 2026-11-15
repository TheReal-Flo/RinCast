// Desktop streaming on macOS: ScreenCaptureKit delivers the display's pictures (with the cursor
// drawn in, as on Windows), VideoToolbox encodes them, and the shared DesktopPacer and
// VideoSender decide when frames go and at what bitrate, as on every OS.
#include "stream/desktop_stream.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "input/input_injector.h"
#include "platform/mac/mac_display.h"
#include "platform/mac/vt_encoder.h"
#include "rc/platform.h"
#include "stream/desktop_pacer.h"

namespace rcs {

// The newest picture ScreenCaptureKit delivered, for the stream thread to take.
struct MacPictureQueue {
    std::mutex mutex;
    std::condition_variable cv;
    CVPixelBufferRef latest = nullptr;  // retained
    bool fresh = false;
    bool stopped = false;  // the capture ended (display gone, permission withdrawn)

    void Put(CVPixelBufferRef picture) {
        CVPixelBufferRetain(picture);
        std::lock_guard lock(mutex);
        if (latest) CVPixelBufferRelease(latest);
        latest = picture;
        fresh = true;
        cv.notify_one();
    }
    void Stop() {
        std::lock_guard lock(mutex);
        stopped = true;
        cv.notify_one();
    }
    ~MacPictureQueue() {
        if (latest) CVPixelBufferRelease(latest);
    }
};

}  // namespace rcs

@interface RcPictureSink : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) rcs::MacPictureQueue* queue;
@end

@implementation RcPictureSink
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;
    // Only complete frames carry a new picture; idle ones say nothing changed.
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
        if (auto status = static_cast<CFNumberRef>(CFDictionaryGetValue(info, (__bridge CFStringRef)SCStreamFrameInfoStatus))) {
            int value = 0;
            CFNumberGetValue(status, kCFNumberIntType, &value);
            if (value != SCFrameStatusComplete) return;
        }
    }
    if (CVPixelBufferRef picture = CMSampleBufferGetImageBuffer(sample)) self.queue->Put(picture);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    RC_LOGW("Screen capture stopped: %s", error.localizedDescription.UTF8String);
    self.queue->Stop();
}
@end

namespace rcs {

namespace {

// ScreenCaptureKit answers on its own queue; this waits for it (at most 10 s).
bool Wait(dispatch_semaphore_t done) {
    return dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * int64_t(NSEC_PER_SEC))) == 0;
}

struct MacCapture {
    SCStream* stream = nil;
    RcPictureSink* sink = nil;
    dispatch_queue_t queue = nil;

    bool Start(uint32_t displayId, const DesktopStreamConfig& config, MacPictureQueue* pictures) {
        __block SCDisplay* display = nil;
        dispatch_semaphore_t listed = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* content, NSError* error) {
          if (error) RC_LOGW("Screen capture: %s", error.localizedDescription.UTF8String);
          for (SCDisplay* d in content.displays)
              if (d.displayID == displayId) display = d;
          dispatch_semaphore_signal(listed);
        }];
        if (!Wait(listed) || !display) return false;

        SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        SCStreamConfiguration* sc = [[SCStreamConfiguration alloc] init];
        sc.width = config.width;
        sc.height = config.height;
        sc.minimumFrameInterval = CMTimeMake(1, int32_t(config.fps + 0.5f));
        sc.pixelFormat = kCVPixelFormatType_32BGRA;
        sc.colorSpaceName = kCGColorSpaceSRGB;
        sc.showsCursor = YES;
        sc.queueDepth = 4;

        sink = [[RcPictureSink alloc] init];
        sink.queue = pictures;
        stream = [[SCStream alloc] initWithFilter:filter configuration:sc delegate:sink];
        queue = dispatch_queue_create("app.rincast.capture", DISPATCH_QUEUE_SERIAL);
        NSError* error = nil;
        if (![stream addStreamOutput:sink type:SCStreamOutputTypeScreen sampleHandlerQueue:queue error:&error]) {
            RC_LOGW("Screen capture: %s", error.localizedDescription.UTF8String);
            return false;
        }
        __block bool started = false;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [stream startCaptureWithCompletionHandler:^(NSError* startError) {
          if (startError) RC_LOGW("Screen capture didn't start: %s", startError.localizedDescription.UTF8String);
          started = startError == nil;
          dispatch_semaphore_signal(done);
        }];
        return Wait(done) && started;
    }

    void Stop() {
        if (stream) {
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [stream stopCaptureWithCompletionHandler:^(NSError*) {
              dispatch_semaphore_signal(done);
            }];
            Wait(done);
        }
        // Let a callback already queued finish before the pictures it writes to go away.
        if (queue) dispatch_sync(queue, ^{});
        stream = nil;
        sink = nil;
        queue = nil;
    }
};

// Where the cursor is on this monitor, in the stream's pixels; -1 when it's elsewhere.
POINT CursorOn(const MonitorInfo& monitor, uint32_t width, uint32_t height) {
    POINT out{-1, -1};
    CGEventRef event = CGEventCreate(nullptr);
    if (!event) return out;
    const CGPoint p = CGEventGetLocation(event);
    CFRelease(event);
    const RECT& r = monitor.desktopRect;
    if (p.x < r.left || p.x >= r.right || p.y < r.top || p.y >= r.bottom) return out;
    out.x = LONG((p.x - r.left) * width / double(r.right - r.left));
    out.y = LONG((p.y - r.top) * height / double(r.bottom - r.top));
    return out;
}

}  // namespace

void DesktopStream::Run() {
    @autoreleasepool {
        m_sender.SetPacing(m_config.pace);
        const uint32_t displayId = DisplayIdOf(m_monitor);
        MacPictureQueue pictures;
        MacCapture capture;
        VtEncoder encoder;
        auto initAll = [&]() -> bool {
            capture.Stop();
            {
                std::lock_guard lock(pictures.mutex);
                pictures.stopped = false;
            }
            return capture.Start(displayId, m_config, &pictures) && encoder.Init(EncoderSettings());
        };
        // Capture can fail for a while (Screen Recording not allowed yet, the display asleep);
        // keep trying, as on Windows.
        bool told = false;
        while (m_running && !initAll()) {
            if (!told) RC_LOGW("Desktop: can't capture %s yet; retrying", m_monitor.name.c_str());
            told = true;
            input::WakeDisplays();
            rc::SleepMs(1000);
        }
        if (!m_running) {
            capture.Stop();
            return;
        }
        RC_LOGI("Desktop: streaming %s at %ux%u", m_monitor.name.c_str(), m_config.width, m_config.height);

        DesktopPacer pacer(m_config.fps);
        uint32_t appliedKbps = m_sender.TargetBitrateKbps();
        CVPixelBufferRef current = nullptr;  // the picture last taken (retained), repeated when needed
        EncodedFrame out;
        while (m_running) @autoreleasepool {
            bool lost = false;
            {
                std::unique_lock lock(pictures.mutex);
                const int64_t wait = pacer.WaitNs(rc::NowNs(), m_active);
                pictures.cv.wait_for(lock, std::chrono::nanoseconds(wait),
                                     [&] { return pictures.fresh || pictures.stopped || !m_running; });
                if (pictures.fresh) {
                    if (current) CVPixelBufferRelease(current);
                    current = pictures.latest;
                    CVPixelBufferRetain(current);
                    pictures.fresh = false;
                    pacer.OnNewPicture(rc::NowNs());
                }
                lost = pictures.stopped;
            }
            if (!m_running) break;
            if (lost) {
                m_sender.RequestKeyframe();
                while (m_running && !initAll()) rc::SleepMs(1000);
                continue;
            }
            if (!current) continue;  // nothing captured yet
            const int64_t t = rc::NowNs();
            if (pacer.Decide(t, m_active, m_sender.WantsFrame()) == DesktopPacer::Decision::Skip) continue;
            pacer.TakePicture();  // Send and Repeat both encode `current`

            const uint32_t encoderKbps = pacer.EncoderKbps(m_sender.TargetBitrateKbps(), m_config.width, m_config.height);
            if (encoderKbps != appliedKbps) {
                encoder.SetBitrate(encoderKbps);
                appliedKbps = encoderKbps;
            }
            const bool keyframe = m_sender.BeginFrame(encoder);
            const int64_t encStart = rc::NowNs();
            if (!encoder.Encode(current, keyframe, out)) {
                RC_LOGW("Desktop encode failed; re-initializing");
                m_sender.RequestKeyframe();
                encoder.Init(EncoderSettings());
                continue;
            }
            const int64_t encEnd = rc::NowNs();
            rc::DesktopFrameMetadata meta{};
            meta.width = m_config.width;
            meta.height = m_config.height;
            meta.pcCaptureNs = t;
            meta.pcEncodeEndNs = encEnd;
            const POINT cursor = CursorOn(m_monitor, m_config.width, m_config.height);
            meta.cursorX = cursor.x;
            meta.cursorY = cursor.y;
            m_sender.Send(out, &meta, sizeof(meta), 0, rc::NsToMs(encEnd - encStart));
            pacer.OnSent(t);
        }
        if (current) CVPixelBufferRelease(current);
        capture.Stop();
    }
}

}  // namespace rcs
