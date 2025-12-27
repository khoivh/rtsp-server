#include "camera_media_factory.h"
#include <gst/gst.h>
#include <string.h>
#include <time.h>
#include "playback_factory.h"
#include "recording_manager.h"

/* Seek parameters structure */
typedef struct {
    gint64 seek_offset;
    gint64 duration_limit;
    gboolean seek_pending;
} SeekParams;

/* Parse query parameter */
static gchar* parse_query_param(const gchar *query, const gchar *param) {
    if (!query) return NULL;
    gchar *search = g_strdup_printf("%s=", param);
    const gchar *pos = strstr(query, search);
    g_free(search);
    if (!pos) return NULL;
    pos += strlen(param) + 1;
    const gchar *end = strchr(pos, '&');
    if (end) return g_strndup(pos, end - pos);
    return g_strdup(pos);
}

struct _CameraMediaFactory {
    GstRTSPMediaFactory parent;
    CameraConfig *camera;
};

G_DEFINE_TYPE(CameraMediaFactory, camera_media_factory, GST_TYPE_RTSP_MEDIA_FACTORY)

/* Forward declarations */
static void on_media_prepared(GstRTSPMedia *media, gpointer user_data);
static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data);

static void media_configure_cb(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data) {
    gst_rtsp_media_set_latency(media, 200);
    gst_rtsp_media_set_transport_mode(media, GST_RTSP_TRANSPORT_MODE_PLAY);
    gst_rtsp_media_set_profiles(media, GST_RTSP_PROFILE_AVP);
    gst_rtsp_media_set_protocols(media, GST_RTSP_LOWER_TRANS_TCP | GST_RTSP_LOWER_TRANS_UDP);

    /* Get seek parameters from factory */
    CameraMediaFactory *cam_factory = CAMERA_MEDIA_FACTORY(user_data);
    SeekParams *params = g_object_get_data(G_OBJECT(cam_factory), "seek-params");

    if (params) {
        g_print("media_configure_cb: offset=%ld ns, duration=%ld ns\n",
                params->seek_offset, params->duration_limit);

        /* For playback with seek/duration, enable eos-shutdown */
        if (params->seek_offset > 0 || params->duration_limit > 0) {
            gst_rtsp_media_set_eos_shutdown(media, TRUE);

            /* Store params in media */
            SeekParams *media_params = g_new0(SeekParams, 1);
            media_params->seek_offset = params->seek_offset;
            media_params->duration_limit = params->duration_limit;
            media_params->seek_pending = TRUE;

            g_object_set_data_full(G_OBJECT(media), "seek-params", media_params, g_free);

            /* Connect to prepared signal */
            g_signal_connect(media, "prepared", G_CALLBACK(on_media_prepared), NULL);
        } else {
            /* Live stream - no eos shutdown */
            gst_rtsp_media_set_eos_shutdown(media, FALSE);
        }

        /* Clear params after use */
        g_object_set_data(G_OBJECT(cam_factory), "seek-params", NULL);
    } else {
        /* Default for live streams */
        gst_rtsp_media_set_eos_shutdown(media, FALSE);
    }
}

static void on_media_prepared(GstRTSPMedia *media, gpointer user_data) {
    SeekParams *params = g_object_get_data(G_OBJECT(media), "seek-params");
    if (!params) {
        g_print("No seek params found in media\n");
        return;
    }

    gint64 seek_offset = params->seek_offset;
    gint64 duration_limit = params->duration_limit;

    GstElement *pipeline = gst_rtsp_media_get_element(media);
    if (!pipeline) return;

    g_print("Media prepared - setting up playback with offset=%ld sec, duration=%ld sec\n",
            seek_offset / GST_SECOND, duration_limit / GST_SECOND);

    /* Store params in pipeline */
    SeekParams *pipeline_params = g_new0(SeekParams, 1);
    pipeline_params->seek_offset = seek_offset;
    pipeline_params->duration_limit = duration_limit;
    pipeline_params->seek_pending = TRUE;
    g_object_set_data_full(G_OBJECT(pipeline), "seek-params", pipeline_params, g_free);
    g_object_set_data(G_OBJECT(pipeline), "rtsp-media", media);

    /* Add bus watch to handle state changes and perform seek when PLAYING */
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    if (bus) {
        gst_bus_add_watch(bus, (GstBusFunc)on_bus_message, pipeline);
        gst_object_unref(bus);
    }

    gst_object_unref(pipeline);
}

static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data) {
    GstElement *pipeline = GST_ELEMENT(user_data);

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                GstState old_state, new_state, pending;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);

                g_print("Pipeline state: %s -> %s\n",
                        gst_element_state_get_name(old_state),
                        gst_element_state_get_name(new_state));

                /* Perform seek when pipeline reaches PLAYING state */
                if (new_state == GST_STATE_PLAYING) {
                    SeekParams *params = g_object_get_data(G_OBJECT(pipeline), "seek-params");
                    if (params && params->seek_pending) {
                        params->seek_pending = FALSE;

                        g_print("Pipeline PLAYING - performing seek now...\n");

                        gint64 seek_offset = params->seek_offset;
                        gint64 duration_limit = params->duration_limit;

                        /* Wait a moment for pipeline to stabilize */
                        g_usleep(200000); // 200ms

                        gboolean seek_result = FALSE;

                        if (seek_offset > 0 && duration_limit > 0) {
                            g_print("Seeking: start=%ld sec, end=%ld sec (duration=%ld sec)\n",
                                    seek_offset / GST_SECOND,
                                    (seek_offset + duration_limit) / GST_SECOND,
                                    duration_limit / GST_SECOND);

                            seek_result = gst_element_seek(pipeline, 1.0, GST_FORMAT_TIME,
                                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE | GST_SEEK_FLAG_SEGMENT,
                                GST_SEEK_TYPE_SET, seek_offset,
                                GST_SEEK_TYPE_SET, seek_offset + duration_limit);
                        } else if (seek_offset > 0) {
                            g_print("Seeking to offset=%ld sec\n", seek_offset / GST_SECOND);

                            seek_result = gst_element_seek(pipeline, 1.0, GST_FORMAT_TIME,
                                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                                GST_SEEK_TYPE_SET, seek_offset,
                                GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
                        } else if (duration_limit > 0) {
                            g_print("Setting duration limit=%ld sec from start\n",
                                    duration_limit / GST_SECOND);

                            seek_result = gst_element_seek(pipeline, 1.0, GST_FORMAT_TIME,
                                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE | GST_SEEK_FLAG_SEGMENT,
                                GST_SEEK_TYPE_SET, 0,
                                GST_SEEK_TYPE_SET, duration_limit);
                        }

                        if (seek_result) {
                            g_print("✓ Seek successful\n");
                        } else {
                            g_printerr("✗ Seek FAILED\n");
                        }
                    }
                }
            }
            break;
        }
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("Pipeline error: %s\n", err->message);
            if (debug) {
                g_printerr("Debug: %s\n", debug);
            }
            g_error_free(err);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_SEGMENT_DONE: {
            g_print("✓ Segment done (duration reached) - sending EOS\n");
            gst_element_send_event(pipeline, gst_event_new_eos());
            return FALSE;
        }
        case GST_MESSAGE_EOS: {
            g_print("✓ End of stream\n");
            return FALSE;
        }
        case GST_MESSAGE_ASYNC_DONE: {
            g_print("✓ Async operations completed\n");
            break;
        }
        default:
            break;
    }

    return TRUE;
}

static gboolean path_to_timestamp(const gchar *path, time_t *out_ts) {
    const gchar *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    long ts = 0;
    if (sscanf(base, "%ld", &ts) == 1 &&
        ts > 1500000000 && ts < 2000000000) {
        *out_ts = (time_t)ts;
        return TRUE;
    }

    const gchar *p = path;
    gint year, mon, day, hour;

    while ((p = strchr(p, '/')) != NULL) {
        p++;
        if (sscanf(p, "%4d/%2d/%2d/%2d",
                   &year, &mon, &day, &hour) == 4) {
            struct tm tm = {0};
            tm.tm_year = year - 1900;
            tm.tm_mon  = mon - 1;
            tm.tm_mday = day;
            tm.tm_hour = hour;
            tm.tm_isdst = -1;
            *out_ts = mktime(&tm);
            return TRUE;
        }
    }
    return FALSE;
}

static void scan_recordings_recursive(const gchar *base_dir, GList **files) {
    GDir *dir = g_dir_open(base_dir, 0, NULL);
    if (!dir) return;

    const gchar *name;
    while ((name = g_dir_read_name(dir))) {
        gchar *full = g_build_filename(base_dir, name, NULL);

        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            scan_recordings_recursive(full, files);
            g_free(full);
            continue;
        }

        if (!g_str_has_suffix(name, ".mp4") && !g_str_has_suffix(name, ".mkv")) {
            g_free(full);
            continue;
        }

        time_t file_ts = 0;
        if (!path_to_timestamp(full, &file_ts)) {
            g_free(full);
            continue;
        }

        *files = g_list_insert_sorted(*files, full, (GCompareFunc)g_strcmp0);
    }

    g_dir_close(dir);
}

static GList* get_recording_files_from_timestamp(const gchar *camera_name,
                                                  gint64 start_ts,
                                                  StreamType stream_type,
                                                  gint64 duration) {
    GList *all_files = NULL;
    GList *result = NULL;

    gchar *base_dir = g_strdup_printf(
        "/home/oryza/Oryza/recordings/%s/%s",
        stream_type == STREAM_MAIN ? "hi_quality" : "low_quality",
        camera_name);

    g_print("DEBUG: Scanning for files at timestamp %ld (%s)\n", (long)start_ts,
            g_date_time_format(g_date_time_new_from_unix_local(start_ts), "%Y-%m-%d %H:%M:%S"));

    scan_recordings_recursive(base_dir, &all_files);

    if (!all_files) {
        g_print("ERROR: No recording files found in %s\n", base_dir);
        g_free(base_dir);
        return NULL;
    }

    g_print("DEBUG: Found %d total files\n", g_list_length(all_files));

    all_files = g_list_sort(all_files, (GCompareFunc)g_strcmp0);

    GList *start_file = NULL;
    time_t best_ts = 0;

    for (GList *l = all_files; l != NULL; l = l->next) {
        gchar *file = (gchar *)l->data;
        time_t file_ts = 0;

        if (!path_to_timestamp(file, &file_ts)) continue;

        g_print("  File: %s (ts=%ld, time=%s)\n",
                file, (long)file_ts,
                g_date_time_format(g_date_time_new_from_unix_local(file_ts), "%H:%M:%S"));

        if (file_ts <= start_ts) {
            if (file_ts > best_ts || best_ts == 0) {
                best_ts = file_ts;
                start_file = l;
            }
        }
    }

    if (!start_file) {
        g_print("ERROR: No file found with timestamp <= %ld\n", (long)start_ts);
        g_list_free_full(all_files, g_free);
        g_free(base_dir);
        return NULL;
    }

    g_print("  ✓ START FILE: %s (ts=%ld)\n", (gchar *)start_file->data, (long)best_ts);

    /* Only include files needed for the duration */
    gint64 end_ts = start_ts + duration;
    gboolean found_end = FALSE;

    for (GList *l = start_file; l != NULL; l = l->next) {
        gchar *file = (gchar *)l->data;
        result = g_list_append(result, g_strdup(file));

        /* If duration specified, stop when we have enough files */
        if (duration > 0) {
            time_t file_ts = 0;
            if (path_to_timestamp(file, &file_ts) && file_ts >= end_ts) {
                found_end = TRUE;
                break;
            }
        }
    }

    g_list_free_full(all_files, g_free);
    g_free(base_dir);

    g_print("Selected %d files for playback", g_list_length(result));
    if (duration > 0) {
        g_print(" (covering %ld seconds)\n", (long)duration);
    } else {
        g_print("\n");
    }

    return result;
}

static GstElement* create_playback_pipeline(GList *files, gint64 start_ts, gint64 duration, SeekParams **out_params) {
    if (!files) return NULL;

    SeekParams *params = g_new0(SeekParams, 1);

    if (g_list_length(files) == 1) {
        gchar *file = (gchar *)files->data;

        time_t file_ts = 0;
        gint64 offset_ns = 0;
        if (path_to_timestamp(file, &file_ts) && start_ts > file_ts) {
            offset_ns = (start_ts - file_ts) * GST_SECOND;
            g_print("Single file - seek offset: %ld seconds from file start\n",
                    offset_ns / GST_SECOND);
        }

        gchar *launch_str = g_strdup_printf(
            "filesrc location=\"%s\" ! "
            "matroskademux ! "
            "h264parse ! "
            "queue max-size-time=5000000000 max-size-bytes=0 max-size-buffers=0 ! "
            "rtph264pay name=pay0 pt=96 config-interval=-1 mtu=1400",
            file);

        GError *error = NULL;
        GstElement *pipeline = gst_parse_launch(launch_str, &error);
        g_free(launch_str);

        if (error) {
            g_printerr("Pipeline error: %s\n", error->message);
            g_error_free(error);
            g_free(params);
            return NULL;
        }

        params->seek_offset = offset_ns;
        params->duration_limit = duration > 0 ? duration * GST_SECOND : 0;
        *out_params = params;

        return pipeline;
    }

    /* Multiple files - use concat */
    GString *concat_str = g_string_new("");
    guint file_count = 0;

    for (GList *l = files; l != NULL; l = l->next) {
        gchar *file = (gchar *)l->data;

        if (file_count > 0) {
            g_string_append(concat_str, " ");
        }

        g_string_append_printf(concat_str,
            "filesrc location=\"%s\" ! "
            "matroskademux ! h264parse ! "
            "queue max-size-time=3000000000 name=q%d "
            "q%d. ! concat. ",
            file, file_count, file_count);

        file_count++;
    }

    gchar *launch_str = g_strdup_printf(
        "%s "
        "concat name=concat ! "
        "queue max-size-time=5000000000 max-size-bytes=0 max-size-buffers=0 ! "
        "h264parse ! "
        "rtph264pay name=pay0 pt=96 config-interval=-1 mtu=1400",
        concat_str->str);

    g_string_free(concat_str, TRUE);

    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(launch_str, &error);
    g_free(launch_str);

    if (error) {
        g_printerr("Pipeline error: %s\n", error->message);
        g_error_free(error);
        g_free(params);
        return NULL;
    }

    time_t first_file_ts = 0;
    gint64 offset_ns = 0;
    if (path_to_timestamp((gchar *)files->data, &first_file_ts) && start_ts > first_file_ts) {
        offset_ns = (start_ts - first_file_ts) * GST_SECOND;
        g_print("Concat - seek offset: %ld seconds from first file\n",
                offset_ns / GST_SECOND);
    }

    params->seek_offset = offset_ns;
    params->duration_limit = duration > 0 ? duration * GST_SECOND : 0;
    *out_params = params;

    return pipeline;
}

static GstElement* camera_media_factory_create_element(GstRTSPMediaFactory *factory,
                                                        const GstRTSPUrl *url) {
    CameraMediaFactory *cam_factory = CAMERA_MEDIA_FACTORY(factory);
    CameraConfig *cam = cam_factory->camera;

    const gchar *query = url->query;
    gchar *stream_id = parse_query_param(query, "stream");
    gchar *timestamp_str = parse_query_param(query, "timestamp");
    gchar *duration_str = parse_query_param(query, "duration");

    const gchar *rtsp_url;
    CodecType codec;
    gboolean is_main_stream = TRUE;

    if (stream_id && g_strcmp0(stream_id, "1") == 0) {
        rtsp_url = cam->rtsp_url_sub;
        codec = cam->codec_sub;
        is_main_stream = FALSE;
    } else {
        rtsp_url = cam->rtsp_url_main;
        codec = cam->codec_main;
    }

    if (timestamp_str) {
        gint64 start_ts = g_ascii_strtoll(timestamp_str, NULL, 10);
        gint64 duration = duration_str ? g_ascii_strtoll(duration_str, NULL, 10) : 0;

        g_print("\n=== PLAYBACK REQUEST ===\n");
        g_print("Camera: %s\n", cam->name);
        g_print("Start timestamp: %ld (%s)\n", (long)start_ts,
                g_date_time_format(g_date_time_new_from_unix_local(start_ts), "%Y-%m-%d %H:%M:%S"));
        if (duration > 0) {
            g_print("Duration: %ld seconds\n", (long)duration);
        }

        GList *files = get_recording_files_from_timestamp(cam->name, start_ts,
                                                          is_main_stream ? STREAM_MAIN : STREAM_SUB,
                                                          duration);

        if (!files) {
            g_printerr("ERROR: No playback files found\n");
            g_free(stream_id);
            g_free(timestamp_str);
            g_free(duration_str);
            return NULL;
        }

        g_print("\nPlayback files:\n");
        for (GList *l = files; l != NULL; l = l->next) {
            g_print("  - %s\n", (gchar *)l->data);
        }
        g_print("========================\n\n");

        SeekParams *seek_params = NULL;
        GstElement *pipeline = create_playback_pipeline(files, start_ts, duration, &seek_params);

        g_list_free_full(files, g_free);
        g_free(stream_id);
        g_free(timestamp_str);
        g_free(duration_str);

        if (pipeline && seek_params) {
            g_object_set_data_full(G_OBJECT(factory), "seek-params", seek_params, g_free);
        }

        return pipeline;
    }

    /* Live streaming pipeline */
    gchar *launch_str = NULL;

    if (codec == CODEC_H265) {
        if (is_main_stream && cam->is_recording && cam->current_record_file_main) {
            launch_str = g_strdup_printf(
                "rtspsrc location=%s protocols=tcp latency=200 buffer-mode=auto ! "
                "rtph265depay ! h265parse config-interval=-1 ! tee name=t "
                "t. ! queue max-size-buffers=3 leaky=downstream ! "
                "rtph265pay name=pay0 pt=96 config-interval=-1 mtu=1400 "
                "t. ! queue ! mp4mux ! filesink location=%s",
                rtsp_url, cam->current_record_file_main);
        } else {
            launch_str = g_strdup_printf(
                "rtspsrc location=%s protocols=tcp latency=200 buffer-mode=auto ! "
                "rtph265depay ! h265parse config-interval=-1 ! "
                "rtph265pay name=pay0 pt=96 config-interval=-1 mtu=1400",
                rtsp_url);
        }
    } else if (codec == CODEC_AUTO) {
        if (is_main_stream && cam->is_recording && cam->current_record_file_main) {
            launch_str = g_strdup_printf(
                "rtspsrc location=%s protocols=tcp latency=200 buffer-mode=auto ! "
                "decodebin ! tee name=t "
                "t. ! queue ! x264enc tune=zerolatency speed-preset=ultrafast ! "
                "h264parse config-interval=-1 ! "
                "rtph264pay name=pay0 pt=96 config-interval=-1 mtu=1400 "
                "t. ! queue ! x264enc ! h264parse ! mp4mux ! filesink location=%s",
                rtsp_url, cam->current_record_file_main);
        } else {
            launch_str = g_strdup_printf(
                "rtspsrc location=%s protocols=tcp latency=200 buffer-mode=auto ! "
                "decodebin ! x264enc tune=zerolatency speed-preset=ultrafast ! "
                "h264parse config-interval=-1 ! "
                "rtph264pay name=pay0 pt=96 config-interval=-1 mtu=1400",
                rtsp_url);
        }
    } else {
        if (is_main_stream && cam->is_recording && cam->current_record_file_main) {
            launch_str = g_strdup_printf(
                "rtspsrc location=%s protocols=tcp latency=200 buffer-mode=auto ! "
                "rtph264depay ! h264parse config-interval=-1 ! tee name=t "
                "t. ! queue max-size-buffers=3 leaky=downstream ! "
                "rtph264pay name=pay0 pt=96 config-interval=-1 mtu=1400 "
                "t. ! queue ! mp4mux ! filesink location=%s",
                rtsp_url, cam->current_record_file_main);
        } else {
            launch_str = g_strdup_printf(
                "rtspsrc location=%s protocols=tcp latency=200 buffer-mode=auto ! "
                "rtph264depay ! h264parse config-interval=-1 ! "
                "rtph264pay name=pay0 pt=96 config-interval=-1 mtu=1400",
                rtsp_url);
        }
    }

    GError *error = NULL;
    GstElement *pipeline = gst_parse_launch(launch_str, &error);
    if (error) {
        g_printerr("Pipeline error: %s\n", error->message);
        g_error_free(error);
        g_free(launch_str);
        return NULL;
    }

    g_free(launch_str);
    g_free(stream_id);
    return pipeline;
}

static void camera_media_factory_class_init(CameraMediaFactoryClass *klass) {
    GstRTSPMediaFactoryClass *factory_class = GST_RTSP_MEDIA_FACTORY_CLASS(klass);
    factory_class->create_element = camera_media_factory_create_element;
}

static void camera_media_factory_init(CameraMediaFactory *factory) {
    GstRTSPMediaFactory *base_factory = GST_RTSP_MEDIA_FACTORY(factory);

    gst_rtsp_media_factory_set_shared(base_factory, FALSE);
    gst_rtsp_media_factory_set_protocols(base_factory,
                                         GST_RTSP_LOWER_TRANS_TCP | GST_RTSP_LOWER_TRANS_UDP);
    gst_rtsp_media_factory_set_profiles(base_factory, GST_RTSP_PROFILE_AVP);
    gst_rtsp_media_factory_set_enable_rtcp(base_factory, TRUE);
    gst_rtsp_media_factory_set_suspend_mode(base_factory, GST_RTSP_SUSPEND_MODE_NONE);

    g_signal_connect(factory, "media-configure", G_CALLBACK(media_configure_cb), factory);
}

CameraMediaFactory* camera_media_factory_new(CameraConfig *cam) {
    CameraMediaFactory *factory = g_object_new(TYPE_CAMERA_MEDIA_FACTORY, NULL);
    factory->camera = cam;
    return factory;
}
