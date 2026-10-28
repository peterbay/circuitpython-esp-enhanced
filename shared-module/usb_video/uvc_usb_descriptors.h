// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2020 Jerzy Kasenbreg
// SPDX-FileCopyrightText: Copyright (c) 2021 Koji KITAYAMA
//
// SPDX-License-Identifier: MIT

#pragma once

#include "class/video/video.h"

/* Time stamp base clock. It is a deprecated parameter. */
#define UVC_CLOCK_FREQUENCY    27000000
/* video capture path */
#define UVC_ENTITY_CAP_INPUT_TERMINAL  0x01
#define UVC_ENTITY_CAP_OUTPUT_TERMINAL 0x02

#define DEFAULT_FRAME_WIDTH   128
#define DEFAULT_FRAME_HEIGHT  96
// CIRCUITPY-CHANGE: the descriptor advertises exactly one frame rate, and from
// it the host works out the bit rate the stream needs: width * height * 16 * fps.
// The isochronous endpoint carries CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE bytes per
// USB frame and no more, so a large enough picture at ten frames a second
// promises more than the endpoint can deliver and the host refuses to start the
// stream at all. Lowering this is what makes a bigger frame work.
#ifndef DEFAULT_FRAME_RATE
#define DEFAULT_FRAME_RATE    10
#endif

// CIRCUITPY-CHANGE: room for one compressed frame, and the figure the MJPEG
// frame descriptor advertises as dwMaxVideoFrameBufferSize. Both have to be the
// same, and both have to be larger than any frame the encoder can produce: the
// encoder ignores the size it is given and writes past the end rather than
// saying the frame did not fit. It does not scale with the picture, because what
// the buffer has to survive is the worst case, not the usual one -- see
// CIRCUITPY_USB_VIDEO_JPEG_FRAME_BYTES in supervisor/supervisor.mk.
#define USB_VIDEO_JPEG_FRAME_SIZE(_width, _height) (CIRCUITPY_USB_VIDEO_JPEG_FRAME_BYTES)

#define TUD_VIDEO_CAPTURE_DESC_UNCOMPR_LEN ( \
    TUD_VIDEO_DESC_IAD_LEN \
    /* control */ \
    + TUD_VIDEO_DESC_STD_VC_LEN \
    + (TUD_VIDEO_DESC_CS_VC_LEN + 1 /*bInCollection*/) \
    + TUD_VIDEO_DESC_CAMERA_TERM_LEN \
    + TUD_VIDEO_DESC_OUTPUT_TERM_LEN \
    /* Interface 1, Alternate 0 */ \
    + TUD_VIDEO_DESC_STD_VS_LEN \
    + (TUD_VIDEO_DESC_CS_VS_IN_LEN + 1 /*bNumFormats x bControlSize*/) \
    + TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN \
    + TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT_LEN \
    + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN \
    /* Interface 1, Alternate 1 */ \
    + TUD_VIDEO_DESC_STD_VS_LEN \
    + 7/* Endpoint */ \
    )

#define TUD_VIDEO_CAPTURE_DESC_MJPEG_LEN ( \
    TUD_VIDEO_DESC_IAD_LEN \
    /* control */ \
    + TUD_VIDEO_DESC_STD_VC_LEN \
    + (TUD_VIDEO_DESC_CS_VC_LEN + 1 /*bInCollection*/) \
    + TUD_VIDEO_DESC_CAMERA_TERM_LEN \
    + TUD_VIDEO_DESC_OUTPUT_TERM_LEN \
    /* Interface 1, Alternate 0 */ \
    + TUD_VIDEO_DESC_STD_VS_LEN \
    + (TUD_VIDEO_DESC_CS_VS_IN_LEN + 1 /*bNumFormats x bControlSize*/) \
    + TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN \
    + TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN \
    + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN \
    /* Interface 1, Alternate 1 */ \
    + TUD_VIDEO_DESC_STD_VS_LEN \
    + 7/* Endpoint */ \
    )

#define TUD_VIDEO_CAPTURE_DESC_UNCOMPR_BULK_LEN ( \
    TUD_VIDEO_DESC_IAD_LEN \
    /* control */ \
    + TUD_VIDEO_DESC_STD_VC_LEN \
    + (TUD_VIDEO_DESC_CS_VC_LEN + 1 /*bInCollection*/) \
    + TUD_VIDEO_DESC_CAMERA_TERM_LEN \
    + TUD_VIDEO_DESC_OUTPUT_TERM_LEN \
    /* Interface 1, Alternate 0 */ \
    + TUD_VIDEO_DESC_STD_VS_LEN \
    + (TUD_VIDEO_DESC_CS_VS_IN_LEN + 1 /*bNumFormats x bControlSize*/) \
    + TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN \
    + TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT_LEN \
    + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN \
    + 7/* Endpoint */ \
    )

#define TUD_VIDEO_CAPTURE_DESC_MJPEG_BULK_LEN ( \
    TUD_VIDEO_DESC_IAD_LEN \
    /* control */ \
    + TUD_VIDEO_DESC_STD_VC_LEN \
    + (TUD_VIDEO_DESC_CS_VC_LEN + 1 /*bInCollection*/) \
    + TUD_VIDEO_DESC_CAMERA_TERM_LEN \
    + TUD_VIDEO_DESC_OUTPUT_TERM_LEN \
    /* Interface 1, Alternate 0 */ \
    + TUD_VIDEO_DESC_STD_VS_LEN \
    + (TUD_VIDEO_DESC_CS_VS_IN_LEN + 1 /*bNumFormats x bControlSize*/) \
    + TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN \
    + TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN \
    + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN \
    + 7/* Endpoint */ \
    )

/* Windows support YUY2 and NV12
 * https://docs.microsoft.com/en-us/windows-hardware/drivers/stream/usb-video-class-driver-overview */

#define TUD_VIDEO_DESC_CS_VS_FMT_YUY2(_fmtidx, _numfmtdesc, _frmidx, _asrx, _asry, _interlace, _cp) \
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR(_fmtidx, _numfmtdesc, TUD_VIDEO_GUID_YUY2, 16, _frmidx, _asrx, _asry, _interlace, _cp)
#define TUD_VIDEO_DESC_CS_VS_FMT_NV12(_fmtidx, _numfmtdesc, _frmidx, _asrx, _asry, _interlace, _cp) \
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR(_fmtidx, _numfmtdesc, TUD_VIDEO_GUID_NV12, 12, _frmidx, _asrx, _asry, _interlace, _cp)
#define TUD_VIDEO_DESC_CS_VS_FMT_M420(_fmtidx, _numfmtdesc, _frmidx, _asrx, _asry, _interlace, _cp) \
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR(_fmtidx, _numfmtdesc, TUD_VIDEO_GUID_M420, 12, _frmidx, _asrx, _asry, _interlace, _cp)
#define TUD_VIDEO_DESC_CS_VS_FMT_I420(_fmtidx, _numfmtdesc, _frmidx, _asrx, _asry, _interlace, _cp) \
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR(_fmtidx, _numfmtdesc, TUD_VIDEO_GUID_I420, 12, _frmidx, _asrx, _asry, _interlace, _cp)

#define TUD_VIDEO_CAPTURE_DESCRIPTOR_UNCOMPR(_stridx, _epin, _width, _height, _fps, _epsize, _itf_num_video_control, _itf_num_video_streaming) \
    TUD_VIDEO_DESC_IAD(_itf_num_video_control, /* 2 Interfaces */ 0x02, _stridx), \
    /* Video control 0 */ \
    TUD_VIDEO_DESC_STD_VC(_itf_num_video_control, 0, _stridx), \
    TUD_VIDEO_DESC_CS_VC(/* UVC 1.5*/ 0x0150, \
    /* wTotalLength - bLength */ \
    TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN, \
    UVC_CLOCK_FREQUENCY, _itf_num_video_streaming), \
    TUD_VIDEO_DESC_CAMERA_TERM(UVC_ENTITY_CAP_INPUT_TERMINAL, 0, 0, \
    /*wObjectiveFocalLengthMin*/ 0, /*wObjectiveFocalLengthMax*/ 0, \
    /*wObjectiveFocalLength*/ 0, /*bmControls*/ 0), \
    TUD_VIDEO_DESC_OUTPUT_TERM(UVC_ENTITY_CAP_OUTPUT_TERMINAL, VIDEO_TT_STREAMING, 0, 1, 0), \
    /* Video stream alt. 0 */ \
    TUD_VIDEO_DESC_STD_VS(_itf_num_video_streaming, 0, 0, _stridx), \
    /* Video stream header for without still image capture */ \
    TUD_VIDEO_DESC_CS_VS_INPUT(/*bNumFormats*/ 1, \
    /*wTotalLength - bLength */ \
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN \
    + TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT_LEN \
    + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN, \
    _epin, /*bmInfo*/ 0, /*bTerminalLink*/ UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
    /*bStillCaptureMethod*/ 0, /*bTriggerSupport*/ 0, /*bTriggerUsage*/ 0, \
    /*bmaControls(1)*/ 0), \
    /* Video stream format */ \
    TUD_VIDEO_DESC_CS_VS_FMT_YUY2(/*bFormatIndex*/ 1, /*bNumFrameDescriptors*/ 1, \
    /*bDefaultFrameIndex*/ 1, 0, 0, 0, /*bCopyProtect*/ 0), \
    /* Video stream frame format */ \
    TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT(/*bFrameIndex */ 1, 0, _width, _height, \
    /*dwMinBitRate*/ _width * _height * 16, /*dwMaxBitRate*/ _width * _height * 16 * _fps, \
    /* CIRCUITPY-CHANGE: dwMaxVideoFrameBufferSize is a size in bytes, and a YUY2
       frame is two bytes a pixel. It was given the bit count, eight times too
       large, and a host that believes it refuses to start a stream whose frames
       are big enough for that figure to matter. */ \
    _width * _height * 2, \
    (10000000 / _fps), (10000000 / _fps), (10000000 / _fps) * _fps, (10000000 / _fps)), \
    TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING(VIDEO_COLOR_PRIMARIES_BT709, VIDEO_COLOR_XFER_CH_BT709, VIDEO_COLOR_COEF_SMPTE170M), \
    /* VS alt 1 */ \
    TUD_VIDEO_DESC_STD_VS(_itf_num_video_streaming, 1, 1, _stridx), \
    /* EP */ \
    TUD_VIDEO_DESC_EP_ISO(_epin, _epsize, 1)

// CIRCUITPY-CHANGE: this took the interface numbers from nowhere -- it used
// _itf_num_video_control and _itf_num_video_streaming without taking them as
// parameters, so it could not compile -- and it advertised a frame buffer of
// width * height * 16 / 8, the size of the uncompressed picture. A JPEG frame
// is a fraction of that, but the figure is a maximum, so it is now the
// generous one byte a pixel that the encoder is given room for.
#define TUD_VIDEO_CAPTURE_DESCRIPTOR_MJPEG(_stridx, _epin, _width, _height, _fps, _epsize, _itf_num_video_control, _itf_num_video_streaming) \
    TUD_VIDEO_DESC_IAD(_itf_num_video_control, /* 2 Interfaces */ 0x02, _stridx), \
    /* Video control 0 */ \
    TUD_VIDEO_DESC_STD_VC(_itf_num_video_control, 0, _stridx), \
    TUD_VIDEO_DESC_CS_VC(/* UVC 1.5*/ 0x0150, \
    /* wTotalLength - bLength */ \
    TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN, \
    UVC_CLOCK_FREQUENCY, _itf_num_video_streaming), \
    TUD_VIDEO_DESC_CAMERA_TERM(UVC_ENTITY_CAP_INPUT_TERMINAL, 0, 0, \
    /*wObjectiveFocalLengthMin*/ 0, /*wObjectiveFocalLengthMax*/ 0, \
    /*wObjectiveFocalLength*/ 0, /*bmControls*/ 0), \
    TUD_VIDEO_DESC_OUTPUT_TERM(UVC_ENTITY_CAP_OUTPUT_TERMINAL, VIDEO_TT_STREAMING, 0, 1, 0), \
    /* Video stream alt. 0 */ \
    TUD_VIDEO_DESC_STD_VS(_itf_num_video_streaming, 0, 0, _stridx), \
    /* Video stream header for without still image capture */ \
    TUD_VIDEO_DESC_CS_VS_INPUT(/*bNumFormats*/ 1, \
    /*wTotalLength - bLength */ \
    TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN \
    + TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN \
    + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN, \
    _epin, /*bmInfo*/ 0, /*bTerminalLink*/ UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
    /*bStillCaptureMethod*/ 0, /*bTriggerSupport*/ 0, /*bTriggerUsage*/ 0, \
    /*bmaControls(1)*/ 0), \
    /* Video stream format */ \
    TUD_VIDEO_DESC_CS_VS_FMT_MJPEG(/*bFormatIndex*/ 1, /*bNumFrameDescriptors*/ 1, \
    /*bmFlags*/ 0, /*bDefaultFrameIndex*/ 1, 0, 0, 0, /*bCopyProtect*/ 0), \
    /* Video stream frame format */ \
    TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT(/*bFrameIndex */ 1, 0, _width, _height, \
    /*dwMinBitRate*/ _width * _height, /*dwMaxBitRate*/ _width * _height * 8 * _fps, \
    /*dwMaxVideoFrameBufferSize*/ USB_VIDEO_JPEG_FRAME_SIZE(_width, _height), \
    (10000000 / _fps), (10000000 / _fps), (10000000 / _fps) * _fps, (10000000 / _fps)), \
    TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING(VIDEO_COLOR_PRIMARIES_BT709, VIDEO_COLOR_XFER_CH_BT709, VIDEO_COLOR_COEF_SMPTE170M), \
    /* VS alt 1 */ \
    TUD_VIDEO_DESC_STD_VS(_itf_num_video_streaming, 1, 1, _stridx), \
    /* EP */ \
    TUD_VIDEO_DESC_EP_ISO(_epin, _epsize, 1)


#define TUD_VIDEO_CAPTURE_DESCRIPTOR_UNCOMPR_BULK(_stridx, _epin, _width, _height, _fps, _epsize, _itf_num_video_control, _itf_num_video_streaming) \
    TUD_VIDEO_DESC_IAD(_itf_num_video_control, /* 2 Interfaces */ 0x02, _stridx), \
    /* Video control 0 */ \
    TUD_VIDEO_DESC_STD_VC(_itf_num_video_control, 0, _stridx), \
    TUD_VIDEO_DESC_CS_VC(/* UVC 1.5*/ 0x0150, \
    /* wTotalLength - bLength */ \
    TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN, \
    UVC_CLOCK_FREQUENCY, _itf_num_video_streaming), \
    TUD_VIDEO_DESC_CAMERA_TERM(UVC_ENTITY_CAP_INPUT_TERMINAL, 0, 0, \
    /*wObjectiveFocalLengthMin*/ 0, /*wObjectiveFocalLengthMax*/ 0, \
    /*wObjectiveFocalLength*/ 0, /*bmControls*/ 0), \
    TUD_VIDEO_DESC_OUTPUT_TERM(UVC_ENTITY_CAP_OUTPUT_TERMINAL, VIDEO_TT_STREAMING, 0, 1, 0), \
    /* Video stream alt. 0 */ \
    TUD_VIDEO_DESC_STD_VS(_itf_num_video_streaming, 0, 1, _stridx), \
    /* Video stream header for without still image capture */ \
    TUD_VIDEO_DESC_CS_VS_INPUT(/*bNumFormats*/ 1, \
    /*wTotalLength - bLength */ \
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN \
    + TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT_LEN \
    + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN, \
    _epin, /*bmInfo*/ 0, /*bTerminalLink*/ UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
    /*bStillCaptureMethod*/ 0, /*bTriggerSupport*/ 0, /*bTriggerUsage*/ 0, \
    /*bmaControls(1)*/ 0), \
    /* Video stream format */ \
    TUD_VIDEO_DESC_CS_VS_FMT_YUY2(/*bFormatIndex*/ 1, /*bNumFrameDescriptors*/ 1, \
    /*bDefaultFrameIndex*/ 1, 0, 0, 0, /*bCopyProtect*/ 0), \
    /* Video stream frame format */ \
    TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT(/*bFrameIndex */ 1, 0, _width, _height, \
    _width * _height * 16, _width * _height * 16 * _fps, \
    _width * _height * 16, \
    (10000000 / _fps), (10000000 / _fps), (10000000 / _fps) * _fps, (10000000 / _fps)), \
    TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING(VIDEO_COLOR_PRIMARIES_BT709, VIDEO_COLOR_XFER_CH_BT709, VIDEO_COLOR_COEF_SMPTE170M), \
    TUD_VIDEO_DESC_EP_BULK(_epin, _epsize, 1)

// CIRCUITPY-CHANGE: interface numbers taken as parameters, as in the
// isochronous macro above, and the same frame buffer size.
#define TUD_VIDEO_CAPTURE_DESCRIPTOR_MJPEG_BULK(_stridx, _epin, _width, _height, _fps, _epsize, _itf_num_video_control, _itf_num_video_streaming) \
    TUD_VIDEO_DESC_IAD(_itf_num_video_control, /* 2 Interfaces */ 0x02, _stridx), \
    /* Video control 0 */ \
    TUD_VIDEO_DESC_STD_VC(_itf_num_video_control, 0, _stridx), \
    TUD_VIDEO_DESC_CS_VC(/* UVC 1.5*/ 0x0150, \
    /* wTotalLength - bLength */ \
    TUD_VIDEO_DESC_CAMERA_TERM_LEN + TUD_VIDEO_DESC_OUTPUT_TERM_LEN, \
    UVC_CLOCK_FREQUENCY, _itf_num_video_streaming), \
    TUD_VIDEO_DESC_CAMERA_TERM(UVC_ENTITY_CAP_INPUT_TERMINAL, 0, 0, \
    /*wObjectiveFocalLengthMin*/ 0, /*wObjectiveFocalLengthMax*/ 0, \
    /*wObjectiveFocalLength*/ 0, /*bmControls*/ 0), \
    TUD_VIDEO_DESC_OUTPUT_TERM(UVC_ENTITY_CAP_OUTPUT_TERMINAL, VIDEO_TT_STREAMING, 0, 1, 0), \
    /* Video stream alt. 0 */ \
    TUD_VIDEO_DESC_STD_VS(_itf_num_video_streaming, 0, 1, _stridx), \
    /* Video stream header for without still image capture */ \
    TUD_VIDEO_DESC_CS_VS_INPUT(/*bNumFormats*/ 1, \
    /*wTotalLength - bLength */ \
    TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN \
    + TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN \
    + TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN, \
    _epin, /*bmInfo*/ 0, /*bTerminalLink*/ UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
    /*bStillCaptureMethod*/ 0, /*bTriggerSupport*/ 0, /*bTriggerUsage*/ 0, \
    /*bmaControls(1)*/ 0), \
    /* Video stream format */ \
    TUD_VIDEO_DESC_CS_VS_FMT_MJPEG(/*bFormatIndex*/ 1, /*bNumFrameDescriptors*/ 1, \
    /*bmFlags*/ 0, /*bDefaultFrameIndex*/ 1, 0, 0, 0, /*bCopyProtect*/ 0), \
    /* Video stream frame format */ \
    TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT(/*bFrameIndex */ 1, 0, _width, _height, \
    /*dwMinBitRate*/ _width * _height, /*dwMaxBitRate*/ _width * _height * 8 * _fps, \
    /*dwMaxVideoFrameBufferSize*/ USB_VIDEO_JPEG_FRAME_SIZE(_width, _height), \
    (10000000 / _fps), (10000000 / _fps), (10000000 / _fps) * _fps, (10000000 / _fps)), \
    TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING(VIDEO_COLOR_PRIMARIES_BT709, VIDEO_COLOR_XFER_CH_BT709, VIDEO_COLOR_COEF_SMPTE170M), \
    /* EP */ \
    TUD_VIDEO_DESC_EP_BULK(_epin, _epsize, 1)
