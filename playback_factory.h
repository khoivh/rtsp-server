#ifndef PLAYBACK_FACTORY_H
#define PLAYBACK_FACTORY_H

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

/* Playback context cho mỗi client */
typedef struct {
    GstElement *pipeline;
    GstElement *filesrc;
    GstElement *demux;
    GstElement *parser;
    GstElement *pay;
    GstElement *appsink;

    gchar *camera_name;
    gchar *file_path;
    gint64 start_timestamp;
    gint stream_type;  // 0=main, 1=sub
    gdouble playback_rate;  // Tốc độ phát (0.5, 1.0, 2.0, etc)

    gboolean is_h265;
    gboolean is_playing;

    /* Seek support */
    gint64 duration;
    gint64 current_position;

    GstRTSPMedia *media;
} PlaybackContext;

/* Factory cho playback RTSP */
GstRTSPMediaFactory* create_playback_factory(const gchar *camera_name,
                                             gint64 timestamp,
                                             gint stream_type);

/* Tìm file recording gần nhất với timestamp */
gchar* find_recording_file(const gchar *camera_name,
                          gint64 timestamp,
                          gint stream_type);

/* Tìm tất cả files trong khoảng thời gian */
GList* find_recording_files_range(const gchar *camera_name,
                                  gint64 start_time,
                                  gint64 end_time,
                                  gint stream_type);

/* Seek đến vị trí cụ thể (tính bằng giây) */
gboolean playback_seek(PlaybackContext *ctx, gint64 position_sec);

/* Thay đổi tốc độ phát */
gboolean playback_set_rate(PlaybackContext *ctx, gdouble rate);

/* Lấy thông tin vị trí hiện tại */
gint64 playback_get_position(PlaybackContext *ctx);

/* Lấy tổng thời lượng */
gint64 playback_get_duration(PlaybackContext *ctx);

#endif // PLAYBACK_FACTORY_H
