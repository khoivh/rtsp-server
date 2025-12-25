#ifndef SERVER_CONTEXT_H
#define SERVER_CONTEXT_H

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include "camera_config.h"

#define MAX_CAMERAS 10
#define RECORD_PATH "/home/oryza/Oryza/recordings"

typedef struct {
    GstRTSPServer *server;
    CameraConfig cameras[MAX_CAMERAS];
    gint camera_count;
} ServerContext;

extern ServerContext *global_ctx;

/* Server context utils */
ServerContext* server_context_new();
void server_context_free(ServerContext *ctx);

/* Camera management */
void add_camera(ServerContext *ctx,
                const gchar *name,
                const gchar *rtsp_main,
                const gchar *rtsp_sub,
                CodecType codec_main,
                CodecType codec_sub,
                gboolean enable_recording);

CameraConfig* find_camera(const gchar *name);

/* Mount endpoints */
void remount_all_cameras(ServerContext *ctx);
void mount_playback_endpoint(GstRTSPMountPoints *mounts,
                             const gchar *camera_name,
                             time_t timestamp);

/* Recording rotation */
gboolean rotate_recording(gpointer user_data);

/* Setup server optimizations */
void setup_server_latency_profile(GstRTSPServer *server);

/* Utils */
void ensure_record_directory();
gchar* generate_record_filename(const gchar *camera_name);

#endif
