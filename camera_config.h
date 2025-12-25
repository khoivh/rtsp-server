#ifndef CAMERA_CONFIG_H
#define CAMERA_CONFIG_H

#include <glib.h>

typedef enum {
    CODEC_H264,
    CODEC_H265,
    CODEC_AUTO
} CodecType;

typedef struct {
    gchar *name;
    gchar *rtsp_url_main;
    gchar *rtsp_url_sub;
    CodecType codec_main;
    CodecType codec_sub;
    gboolean is_recording;
    gchar *current_record_file_main;
    gchar *current_record_file_sub;
} CameraConfig;

#endif
