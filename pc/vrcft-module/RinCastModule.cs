// RinCast module for VRCFaceTracking: the face and eye tracking a Quest sends through RinCast,
// read from the shared memory RinCast Streamer keeps (layout: pc/common/face_tracking.h).
using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using Microsoft.Extensions.Logging;
using VRCFaceTracking;
using VRCFaceTracking.Core.Params.Expressions;
using VRCFaceTracking.Core.Types;

namespace RinCast.VRCFaceTracking;

public class RinCastModule : ExtTrackingModule
{
    const string BlockName = "RinCastFaceTracking";
    const uint Magic = 0x54464352;  // "RCFT"
    const uint Version = 1;

    // Offsets in the block, as in face_tracking.h.
    const int OffMagic = 0, OffVersion = 4, OffSequence = 8, OffUpdated = 16, OffFlags = 24;
    const int OffExpressions = 36, OffLeftGaze = 316, OffRightGaze = 328, BlockSize = 348;
    const int ExpressionCount = 70;

    const uint FaceValid = 1, LeftEyeValid = 4, RightEyeValid = 8;
    const long StaleNs = 500_000_000;  // the headset stopped sending

    MemoryMappedFile? _file;
    MemoryMappedViewAccessor? _view;
    readonly float[] _w = new float[ExpressionCount];
    readonly float[] _left = new float[3], _right = new float[3];

    public override (bool SupportsEye, bool SupportsExpression) Supported => (true, true);

    public override (bool eyeSuccess, bool expressionSuccess) Initialize(bool eyeAvailable, bool expressionAvailable)
    {
        ModuleInformation.Name = "RinCast";
        // The block appears when a headset first sends its face; until then, keep looking.
        TryOpen();
        return (eyeAvailable, expressionAvailable);
    }

    public override void Update()
    {
        Thread.Sleep(10);
        if (_view == null && !TryOpen())
        {
            Thread.Sleep(1000);
            return;
        }
        if (!Read(out uint flags, out long updatedNs)) return;
        if (NowNs() - updatedNs > StaleNs) return;  // keeps the last face rather than snapping it shut

        if ((flags & FaceValid) != 0) MapExpressions();
        if ((flags & LeftEyeValid) != 0) UnifiedTracking.Data.Eye.Left.Gaze = Gaze(_left);
        if ((flags & RightEyeValid) != 0) UnifiedTracking.Data.Eye.Right.Gaze = Gaze(_right);
    }

    public override void Teardown()
    {
        _view?.Dispose();
        _file?.Dispose();
        _view = null;
        _file = null;
    }

    bool TryOpen()
    {
        try
        {
            _file = MemoryMappedFile.OpenExisting(BlockName, MemoryMappedFileRights.Read);
            _view = _file.CreateViewAccessor(0, BlockSize, MemoryMappedFileAccess.Read);
            if (_view.ReadUInt32(OffMagic) != Magic || _view.ReadUInt32(OffVersion) != Version)
            {
                Logger.LogWarning("RinCast: the shared face tracking has another layout; update the module or the streamer");
                Teardown();
                return false;
            }
            Logger.LogInformation("RinCast: connected to RinCast Streamer");
            return true;
        }
        catch (FileNotFoundException)
        {
            return false;  // RinCast Streamer isn't running, or no face sent yet
        }
    }

    // A consistent copy: the streamer makes the sequence odd while it writes.
    bool Read(out uint flags, out long updatedNs)
    {
        flags = 0;
        updatedNs = 0;
        var v = _view!;
        for (int attempt = 0; attempt < 4; attempt++)
        {
            long before = v.ReadInt64(OffSequence);
            if ((before & 1) != 0) continue;
            flags = v.ReadUInt32(OffFlags);
            updatedNs = v.ReadInt64(OffUpdated);
            v.ReadArray(OffExpressions, _w, 0, ExpressionCount);
            v.ReadArray(OffLeftGaze, _left, 0, 3);
            v.ReadArray(OffRightGaze, _right, 0, 3);
            if (v.ReadInt64(OffSequence) == before) return true;
        }
        return false;
    }

    static long NowNs() => (long)(Stopwatch.GetTimestamp() * (1e9 / Stopwatch.Frequency));

    // Head-space direction (-z ahead) to VRCFaceTracking's gaze: left/right and up/down.
    static Vector2 Gaze(float[] d) => new(d[0], d[1]);

    // XR_FB_face_tracking2's expressions, in its order.
    enum Fb
    {
        BrowLowererL, BrowLowererR, CheekPuffL, CheekPuffR, CheekRaiserL, CheekRaiserR, CheekSuckL, CheekSuckR,
        ChinRaiserB, ChinRaiserT, DimplerL, DimplerR, EyesClosedL, EyesClosedR, EyesLookDownL, EyesLookDownR,
        EyesLookLeftL, EyesLookLeftR, EyesLookRightL, EyesLookRightR, EyesLookUpL, EyesLookUpR, InnerBrowRaiserL,
        InnerBrowRaiserR, JawDrop, JawSidewaysLeft, JawSidewaysRight, JawThrust, LidTightenerL, LidTightenerR,
        LipCornerDepressorL, LipCornerDepressorR, LipCornerPullerL, LipCornerPullerR, LipFunnelerLB, LipFunnelerLT,
        LipFunnelerRB, LipFunnelerRT, LipPressorL, LipPressorR, LipPuckerL, LipPuckerR, LipStretcherL, LipStretcherR,
        LipSuckLB, LipSuckLT, LipSuckRB, LipSuckRT, LipTightenerL, LipTightenerR, LipsToward, LowerLipDepressorL,
        LowerLipDepressorR, MouthLeft, MouthRight, NoseWrinklerL, NoseWrinklerR, OuterBrowRaiserL, OuterBrowRaiserR,
        UpperLidRaiserL, UpperLidRaiserR, UpperLipRaiserL, UpperLipRaiserR, TongueTipInterdental, TongueTipAlveolar,
        TongueFrontDorsalPalate, TongueMidDorsalPalate, TongueBackDorsalVelar, TongueOut, TongueRetreat,
    }

    float W(Fb e) => _w[(int)e];

    static void Set(UnifiedExpressions u, float weight) => UnifiedTracking.Data.Shapes[(int)u].Weight = weight;

    void MapExpressions()
    {
        // Written in full: the eye data may be structs, and a copy would change nothing.
        UnifiedTracking.Data.Eye.Left.Openness = 1f - W(Fb.EyesClosedL);
        UnifiedTracking.Data.Eye.Right.Openness = 1f - W(Fb.EyesClosedR);

        Set(UnifiedExpressions.EyeWideLeft, W(Fb.UpperLidRaiserL));
        Set(UnifiedExpressions.EyeWideRight, W(Fb.UpperLidRaiserR));
        Set(UnifiedExpressions.EyeSquintLeft, W(Fb.LidTightenerL));
        Set(UnifiedExpressions.EyeSquintRight, W(Fb.LidTightenerR));

        Set(UnifiedExpressions.BrowLowererLeft, W(Fb.BrowLowererL));
        Set(UnifiedExpressions.BrowLowererRight, W(Fb.BrowLowererR));
        Set(UnifiedExpressions.BrowPinchLeft, W(Fb.BrowLowererL));
        Set(UnifiedExpressions.BrowPinchRight, W(Fb.BrowLowererR));
        Set(UnifiedExpressions.BrowInnerUpLeft, W(Fb.InnerBrowRaiserL));
        Set(UnifiedExpressions.BrowInnerUpRight, W(Fb.InnerBrowRaiserR));
        Set(UnifiedExpressions.BrowOuterUpLeft, W(Fb.OuterBrowRaiserL));
        Set(UnifiedExpressions.BrowOuterUpRight, W(Fb.OuterBrowRaiserR));

        Set(UnifiedExpressions.CheekPuffLeft, W(Fb.CheekPuffL));
        Set(UnifiedExpressions.CheekPuffRight, W(Fb.CheekPuffR));
        Set(UnifiedExpressions.CheekSuckLeft, W(Fb.CheekSuckL));
        Set(UnifiedExpressions.CheekSuckRight, W(Fb.CheekSuckR));
        Set(UnifiedExpressions.CheekSquintLeft, W(Fb.CheekRaiserL));
        Set(UnifiedExpressions.CheekSquintRight, W(Fb.CheekRaiserR));
        Set(UnifiedExpressions.NoseSneerLeft, W(Fb.NoseWrinklerL));
        Set(UnifiedExpressions.NoseSneerRight, W(Fb.NoseWrinklerR));

        Set(UnifiedExpressions.JawOpen, W(Fb.JawDrop));
        Set(UnifiedExpressions.JawLeft, W(Fb.JawSidewaysLeft));
        Set(UnifiedExpressions.JawRight, W(Fb.JawSidewaysRight));
        Set(UnifiedExpressions.JawForward, W(Fb.JawThrust));
        Set(UnifiedExpressions.MouthClosed, W(Fb.LipsToward));

        Set(UnifiedExpressions.MouthUpperUpLeft, W(Fb.UpperLipRaiserL));
        Set(UnifiedExpressions.MouthUpperUpRight, W(Fb.UpperLipRaiserR));
        Set(UnifiedExpressions.MouthLowerDownLeft, W(Fb.LowerLipDepressorL));
        Set(UnifiedExpressions.MouthLowerDownRight, W(Fb.LowerLipDepressorR));
        Set(UnifiedExpressions.MouthCornerPullLeft, W(Fb.LipCornerPullerL));
        Set(UnifiedExpressions.MouthCornerPullRight, W(Fb.LipCornerPullerR));
        Set(UnifiedExpressions.MouthCornerSlantLeft, W(Fb.LipCornerPullerL));
        Set(UnifiedExpressions.MouthCornerSlantRight, W(Fb.LipCornerPullerR));
        Set(UnifiedExpressions.MouthFrownLeft, W(Fb.LipCornerDepressorL));
        Set(UnifiedExpressions.MouthFrownRight, W(Fb.LipCornerDepressorR));
        Set(UnifiedExpressions.MouthStretchLeft, W(Fb.LipStretcherL));
        Set(UnifiedExpressions.MouthStretchRight, W(Fb.LipStretcherR));
        Set(UnifiedExpressions.MouthDimpleLeft, W(Fb.DimplerL));
        Set(UnifiedExpressions.MouthDimpleRight, W(Fb.DimplerR));
        Set(UnifiedExpressions.MouthRaiserUpper, W(Fb.ChinRaiserT));
        Set(UnifiedExpressions.MouthRaiserLower, W(Fb.ChinRaiserB));
        Set(UnifiedExpressions.MouthPressLeft, W(Fb.LipPressorL));
        Set(UnifiedExpressions.MouthPressRight, W(Fb.LipPressorR));
        Set(UnifiedExpressions.MouthTightenerLeft, W(Fb.LipTightenerL));
        Set(UnifiedExpressions.MouthTightenerRight, W(Fb.LipTightenerR));
        Set(UnifiedExpressions.MouthUpperLeft, W(Fb.MouthLeft));
        Set(UnifiedExpressions.MouthLowerLeft, W(Fb.MouthLeft));
        Set(UnifiedExpressions.MouthUpperRight, W(Fb.MouthRight));
        Set(UnifiedExpressions.MouthLowerRight, W(Fb.MouthRight));

        Set(UnifiedExpressions.LipFunnelUpperLeft, W(Fb.LipFunnelerLT));
        Set(UnifiedExpressions.LipFunnelUpperRight, W(Fb.LipFunnelerRT));
        Set(UnifiedExpressions.LipFunnelLowerLeft, W(Fb.LipFunnelerLB));
        Set(UnifiedExpressions.LipFunnelLowerRight, W(Fb.LipFunnelerRB));
        Set(UnifiedExpressions.LipPuckerUpperLeft, W(Fb.LipPuckerL));
        Set(UnifiedExpressions.LipPuckerLowerLeft, W(Fb.LipPuckerL));
        Set(UnifiedExpressions.LipPuckerUpperRight, W(Fb.LipPuckerR));
        Set(UnifiedExpressions.LipPuckerLowerRight, W(Fb.LipPuckerR));
        Set(UnifiedExpressions.LipSuckUpperLeft, W(Fb.LipSuckLT));
        Set(UnifiedExpressions.LipSuckUpperRight, W(Fb.LipSuckRT));
        Set(UnifiedExpressions.LipSuckLowerLeft, W(Fb.LipSuckLB));
        Set(UnifiedExpressions.LipSuckLowerRight, W(Fb.LipSuckRB));

        Set(UnifiedExpressions.TongueOut, W(Fb.TongueOut));
    }
}
