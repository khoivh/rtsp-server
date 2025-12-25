#include "server_context.h"
#include "camera_media_factory.h"
#include "playback_factory.h"
#include "recording_manager.h"
#include <sys/stat.h>
#include <string.h>
#include <time.h>

ServerContext *global_ctx = NULL;

void ensure_record_directory() {
    struct stat st = {0};
    if (stat(RECORD_PATH, &st) == -1) {
        mkdir(RECORD_PATH, 0755);
    }
}

gchar* generate_record_filename(const gchar *camera_name) {
    time_t now = time(NULL);
    return g_strdup_printf("%s/%s_%ld.mp4", RECORD_PATH, camera_name, now);
}

void add_camera(ServerContext *ctx,
                const gchar *name,
                const gchar *rtsp_main,
                const gchar *rtsp_sub,
                CodecType codec_main,
                CodecType codec_sub,
                gboolean enable_recording) {
    if (ctx->camera_count >= MAX_CAMERAS) {
        g_printerr("Maximum cameras reached!\n");
        return;
    }

    CameraConfig *cam = &ctx->cameras[ctx->camera_count];
    cam->name = g_strdup(name);
    cam->rtsp_url_main = g_strdup(rtsp_main);
    cam->rtsp_url_sub = g_strdup(rtsp_sub);
    cam->codec_main = codec_main;
    cam->codec_sub = codec_sub;
    cam->is_recording = enable_recording;

    if (enable_recording) {
        cam->current_record_file_main = generate_record_filename(name);
        g_print("Recording to: %s\n", cam->current_record_file_main);
    } else {
        cam->current_record_file_main = NULL;
    }

    ctx->camera_count++;
}

CameraConfig* find_camera(const gchar *name) {
    if (!global_ctx) return NULL;
    for (gint i = 0; i < global_ctx->camera_count; i++) {
        if (g_strcmp0(global_ctx->cameras[i].name, name) == 0) {
            return &global_ctx->cameras[i];
        }
    }
    return NULL;
}

static void mount_camera(GstRTSPMountPoints *mounts, CameraConfig *cam) {
    gchar *path = g_strdup_printf("/%s", cam->name);
    CameraMediaFactory *factory = camera_media_factory_new(cam);
    gst_rtsp_mount_points_add_factory(mounts, path, GST_RTSP_MEDIA_FACTORY(factory));
    g_free(path);
}

void remount_all_cameras(ServerContext *ctx) {
    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(ctx->server);
    for (gint i = 0; i < ctx->camera_count; i++) {
        mount_camera(mounts, &ctx->cameras[i]);
    }
    g_object_unref(mounts);
}

void mount_playback_endpoint(GstRTSPMountPoints *mounts,
                             const gchar *camera_name,
                             time_t timestamp) {
    /* Use new create_playback_factory signature which needs camera_name, timestamp and stream_type.
       Default to STREAM_MAIN for this mounting helper. */
    GstRTSPMediaFactory *factory = create_playback_factory(camera_name, (gint64)timestamp, STREAM_MAIN);
    if (!factory) {
        g_printerr("Failed to create playback factory for %s @ %ld\n", camera_name, (long)timestamp);
        return;
    }
    gchar *mount_path = g_strdup_printf("/%s/playback/%ld", camera_name, timestamp);
    gst_rtsp_mount_points_add_factory(mounts, mount_path, factory);
    g_free(mount_path);
}

gboolean rotate_recording(gpointer user_data) {
    ServerContext *ctx = (ServerContext *)user_data;
    for (gint i = 0; i < ctx->camera_count; i++) {
        if (ctx->cameras[i].is_recording) {
            g_free(ctx->cameras[i].current_record_file_main);
            ctx->cameras[i].current_record_file_main = generate_record_filename(ctx->cameras[i].name);
        }
    }
    remount_all_cameras(ctx);
    return G_SOURCE_CONTINUE;
}

void setup_server_latency_profile(GstRTSPServer *server) {
    g_object_set(server, "backlog", 5, NULL);
}
