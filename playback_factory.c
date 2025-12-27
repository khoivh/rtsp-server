#include "playback_factory.h"
#include <glib.h>
#include <dirent.h>
#include <string.h>
#include "recording_manager.h"

/* Recursive search for a recording file that contains the given timestamp and camera name
 * We search under RECORD_BASE_PATH (used by recording_manager) for any file that contains
 * the timestamp string in its filename and belongs to the camera directory.
 */
static gchar* search_recordings_recursive(const gchar *dirpath,
                                         const gchar *camera_name,
                                         const gchar *timestamp_str) {
    GDir *dir = g_dir_open(dirpath, 0, NULL);
    if (!dir) return NULL;

    const gchar *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        gchar *child = g_build_filename(dirpath, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
            gchar *res = search_recordings_recursive(child, camera_name, timestamp_str);
            g_free(child);
            if (res) {
                g_dir_close(dir);
                return res;
            }
        } else {
            /* Check file name contains timestamp */
            if (strstr(name, timestamp_str)) {
                /* Ensure camera_name appears in the path (quick heuristic) */
                if (strstr(dirpath, camera_name) || strstr(child, camera_name)) {
                    gchar *found = g_strdup(child);
                    g_free(child);
                    g_dir_close(dir);
                    return found;
                }
            }
            g_free(child);
        }
    }

    g_dir_close(dir);
    return NULL;
}

gchar* find_recording_file(const gchar *camera_name,
                           gint64 timestamp,
                           gint stream_type) {
    gchar tbuf[64];
    g_snprintf(tbuf, sizeof(tbuf), "%ld", (long)timestamp);

    /* Search under RECORD_BASE_PATH */
    return search_recordings_recursive(RECORD_BASE_PATH, camera_name, tbuf);
}

/* Create a GstRTSPMediaFactory that serves the file at file_path.
 * We build a launch pipeline that uses filesrc + decodebin + x264enc + h264parse + rtph264pay
 * so the resulting stream is H264 RTP. Using filesrc + decodebin keeps the pipeline seekable
 * so the RTSP server can handle Range (seek) and rate changes.
 */
GstRTSPMediaFactory* create_playback_factory(const gchar *camera_name,
                                             gint64 timestamp,
                                             gint stream_type) {
    gchar *file_path = find_recording_file(camera_name, timestamp, stream_type);
    if (!file_path) {
        g_printerr("Playback file not found for %s @ %ld\n", camera_name, (long)timestamp);
        return NULL;
    }

    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    /* Use decode -> re-encode to H264 then payloader. This is more compatible across containers/codecs
     * and keeps the pipeline seekable for gst_rtsp_server to perform seeks/rate changes.
     */
    gchar *launch_str = g_strdup_printf(
        "( filesrc location=\"%s\" ! qtdemux name=demux demux.video_0 ! queue ! decodebin ! videoconvert ! "
        "x264enc tune=zerolatency speed-preset=superfast bitrate=800 ! h264parse config-interval=1 ! "
        "rtph264pay name=pay0 pt=96 )",
        file_path);

    gst_rtsp_media_factory_set_launch(factory, launch_str);
    gst_rtsp_media_factory_set_shared(factory, FALSE);

    g_free(launch_str);
    g_free(file_path);
    return factory;
}
