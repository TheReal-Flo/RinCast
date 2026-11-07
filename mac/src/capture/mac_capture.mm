#include "mac_capture.h"

#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>

#include "rc/platform.h"

@interface RinStreamOutputHandler : NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, assign) rcm::FrameCallback frameCallback;
@property (nonatomic, assign) rcm::AudioCallback audioCallback;
@end

@implementation RinStreamOutputHandler

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    if (!CMSampleBufferIsValid(sampleBuffer)) return;

    if (type == SCStreamOutputTypeScreen) {
        CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
        if (!pixelBuffer) return;

        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
        uint64_t timestampNs = static_cast<uint64_t>(CMTimeGetSeconds(pts) * 1e9);
        if (timestampNs == 0) timestampNs = rc::NowNs();

        if (self.frameCallback) {
            self.frameCallback(pixelBuffer, timestampNs);
        }
    } else if (type == SCStreamOutputTypeAudio) {
        if (self.audioCallback) {
            self.audioCallback(sampleBuffer);
        }
    }
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    if (error) {
        RC_LOGE("SCStream stopped with error: %s", error.localizedDescription.UTF8String);
    }
}

@end

namespace rcm {

struct MacDesktopCapture::Impl {
    SCStream* stream = nil;
    RinStreamOutputHandler* handler = nil;
    dispatch_queue_t queue = nil;
    bool running = false;
};

MacDesktopCapture::MacDesktopCapture() : m_impl(std::make_unique<Impl>()) {}

MacDesktopCapture::~MacDesktopCapture() {
    Stop();
}

bool MacDesktopCapture::Init(const MacMonitorInfo& monitor, uint32_t outWidth, uint32_t outHeight, float fps,
                             FrameCallback onFrame, AudioCallback onAudio) {
    Stop();

    m_monitor = monitor;
    m_outWidth = outWidth;
    m_outHeight = outHeight;
    m_fps = fps > 0 ? fps : 60.0f;

    __block SCDisplay* targetDisplay = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *error) {
        if (error) {
            RC_LOGE("Failed to get SCShareableContent: %s", error.localizedDescription.UTF8String);
            dispatch_semaphore_signal(sem);
            return;
        }

        for (SCDisplay* d in content.displays) {
            if (d.displayID == monitor.displayId) {
                targetDisplay = d;
                break;
            }
        }
        if (!targetDisplay && content.displays.count > 0) {
            targetDisplay = content.displays.firstObject;
        }
        dispatch_semaphore_signal(sem);
    }];

    dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC);
    if (dispatch_semaphore_wait(sem, timeout) != 0 || !targetDisplay) {
        RC_LOGE("Display %u not found in ScreenCaptureKit shareable content", monitor.displayId);
        return false;
    }

    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:targetDisplay excludingWindows:@[]];

    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width = m_outWidth;
    config.height = m_outHeight;
    config.minimumFrameInterval = CMTimeMake(1, static_cast<int32_t>(m_fps));
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.showsCursor = YES;
    config.queueDepth = 3;

    // Enable system audio capture if audio callback was provided
    if (onAudio && [config respondsToSelector:@selector(setCapturesAudio:)]) {
        config.capturesAudio = YES;
        config.sampleRate = 48000;
        config.channelCount = 2;
    }

    m_impl->handler = [[RinStreamOutputHandler alloc] init];
    m_impl->handler.frameCallback = std::move(onFrame);
    m_impl->handler.audioCallback = std::move(onAudio);

    m_impl->stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:m_impl->handler];
    if (!m_impl->stream) {
        RC_LOGE("Failed to create SCStream");
        return false;
    }

    m_impl->queue = dispatch_queue_create("com.rincast.capture_queue", DISPATCH_QUEUE_SERIAL);
    NSError* addErr = nil;
    if (![m_impl->stream addStreamOutput:m_impl->handler type:SCStreamOutputTypeScreen sampleHandlerQueue:m_impl->queue error:&addErr]) {
        RC_LOGE("Failed to add video stream output: %s", addErr.localizedDescription.UTF8String);
        return false;
    }

    if (m_impl->handler.audioCallback) {
        NSError* audioErr = nil;
        if (![m_impl->stream addStreamOutput:m_impl->handler type:SCStreamOutputTypeAudio sampleHandlerQueue:m_impl->queue error:&audioErr]) {
            RC_LOGW("Failed to add audio stream output: %s", audioErr.localizedDescription.UTF8String);
        } else {
            RC_LOGI("ScreenCaptureKit audio output enabled");
        }
    }

    RC_LOGI("MacDesktopCapture initialized for display %u (%s, %ux%u @ %.1f fps)",
            monitor.displayId, monitor.name.c_str(), m_outWidth, m_outHeight, m_fps);
    return true;
}

bool MacDesktopCapture::Start() {
    if (!m_impl->stream || m_impl->running) return false;

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block bool success = false;

    [m_impl->stream startCaptureWithCompletionHandler:^(NSError *error) {
        if (error) {
            RC_LOGE("Failed to start SCStream capture: %s", error.localizedDescription.UTF8String);
            success = false;
        } else {
            success = true;
        }
        dispatch_semaphore_signal(sem);
    }];

    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    m_impl->running = success;
    return success;
}

void MacDesktopCapture::Stop() {
    if (m_impl->stream && m_impl->running) {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [m_impl->stream stopCaptureWithCompletionHandler:^(NSError *error) {
            if (error) {
                RC_LOGW("Error stopping SCStream: %s", error.localizedDescription.UTF8String);
            }
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        m_impl->running = false;
    }

    m_impl->stream = nil;
    m_impl->handler = nil;
    m_impl->queue = nil;
}

}  // namespace rcm
