#include "recording_manager.h"
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <dirent.h>
#include <string.h>

/* Tạo đường dẫn thư mục theo thời gian */
static gchar* get_recording_directory(const gchar *camera_name, StreamType stream_type) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    const gchar *quality = (stream_type == STREAM_MAIN) ? RECORD_HI_QUALITY : RECORD_LOW_QUALITY;

    return g_strdup_printf("%s/%s/%s/%04d/%02d/%02d/%02d",
                          RECORD_BASE_PATH, quality, camera_name,
                          tm_info->tm_year + 1900,
                          tm_info->tm_mon + 1,
                          tm_info->tm_mday,
                          tm_info->tm_hour);
}

/* Tạo đường dẫn file với timestamp và pattern cho segment */
static gchar* generate_recording_filename(const gchar *camera_name, StreamType stream_type) {
    time_t now = time(NULL);
    gchar *dir = get_recording_directory(camera_name, stream_type);
    gchar *filename = g_strdup_printf("%s/%ld_%%05d.mkv", dir, now);
    g_free(dir);
    return filename;
}

gchar* generate_recording_path(const gchar *camera_name, StreamType stream_type) {
    return generate_recording_filename(camera_name, stream_type);
}

gboolean ensure_recording_directory(const gchar *path) {
    gchar *dir_path = g_path_get_dirname(path);

    gchar *temp = g_strdup(dir_path);
    gchar *p = temp + 1;

    while ((p = strchr(p, '/'))) {
        *p = '\0';
        mkdir(temp, 0755);
        *p = '/';
        p++;
    }
    mkdir(temp, 0755);

    g_free(temp);
    g_free(dir_path);
    return TRUE;
}

/* Bus message handler */
static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    RecordingPipeline *rec = (RecordingPipeline *)data;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS: {
            g_print("[%s-%s] Got EOS - this shouldn't happen during rotation\n",
                   rec->camera_name,
                   rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");
            break;
        }

        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("[%s-%s] Error: %s\n",
                      rec->camera_name,
                      rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB",
                      err->message);
            if (debug) {
                g_printerr("Debug: %s\n", debug);
            }

            /* Check if this is a connection error */
            if (strstr(err->message, "Could not read") ||
                strstr(err->message, "Connection") ||
                strstr(err->message, "resource")) {
                g_print("[%s-%s] Connection lost, will retry on next rotation\n",
                       rec->camera_name,
                       rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");
            }

            g_error_free(err);
            g_free(debug);
            break;
        }

        case GST_MESSAGE_WARNING: {
            GError *err;
            gchar *debug;
            gst_message_parse_warning(msg, &err, &debug);

            /* Chỉ log warning quan trọng */
            if (!strstr(err->message, "Could not read")) {
                g_print("[%s-%s] Warning: %s\n",
                       rec->camera_name,
                       rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB",
                       err->message);
            }

            g_error_free(err);
            g_free(debug);
            break;
        }

        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(rec->pipeline)) {
                GstState old, new, pending;
                gst_message_parse_state_changed(msg, &old, &new, &pending);
                if (new == GST_STATE_PLAYING && old != GST_STATE_PLAYING) {
                    g_print("[%s-%s] Recording PLAYING\n",
                           rec->camera_name,
                           rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");
                }
            }
            break;

        default:
            break;
    }

    return TRUE;
}

/* Tạo recording pipeline - Version 4: splitmuxsink FIXED */
static gboolean create_recording_pipeline(RecordingPipeline *rec) {
    GstBus *bus;

    g_print("[DEBUG] Creating pipeline for %s-%s\n",
            rec->camera_name,
            rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");

    rec->pipeline = gst_pipeline_new(NULL);
    if (!rec->pipeline) {
        g_printerr("Failed to create pipeline\n");
        return FALSE;
    }

    /* Elements */
    rec->source = gst_element_factory_make("rtspsrc", NULL);
    rec->depay = gst_element_factory_make(rec->is_h265 ? "rtph265depay" : "rtph264depay", NULL);
    rec->parser = gst_element_factory_make(rec->is_h265 ? "h265parse" : "h264parse", NULL);
    GstElement *queue = gst_element_factory_make("queue", NULL);
    GstElement *muxer = gst_element_factory_make("matroskamux", NULL);
    GstElement *filesink = gst_element_factory_make("filesink", NULL);

    if (!rec->source || !rec->depay || !rec->parser || !queue || !muxer || !filesink) {
        g_printerr("Failed to create elements\n");
        goto error;
    }

    /* Cấu hình rtspsrc */
    g_object_set(rec->source,
                 "location", rec->rtsp_url,
                 "protocols", 0x00000004,
                 "latency", 200,
                 "buffer-mode", 3,
                 "retry", 5,
                 "timeout", 5000000,
                 "tcp-timeout", 5000000,
                 "do-rtcp", FALSE,
                 "drop-on-latency", TRUE,
                 NULL);

    /* Cấu hình parser */
    g_object_set(rec->parser,
                 "config-interval", -1,
                 NULL);

    /* Cấu hình queue */
    g_object_set(queue,
                 "max-size-buffers", 200,
                 "max-size-bytes", 10485760,
                 "max-size-time", 3000000000ULL,
                 "leaky", 2,
                 NULL);

    /* Cấu hình muxer */
    g_object_set(muxer,
                 "streamable", TRUE,
                 "writing-app", "RTSP Recorder",
                 NULL);

    /* Tạo thư mục và file đầu tiên */
    gchar *dir = get_recording_directory(rec->camera_name, rec->stream_type);
    gchar *temp = g_strdup(dir);
    gchar *p = temp;

    while ((p = strchr(p + 1, '/'))) {
        *p = '\0';
        g_mkdir(temp, 0755);
        *p = '/';
    }
    g_mkdir(temp, 0755);
    g_free(temp);

    time_t now = time(NULL);
    gchar *filename = g_strdup_printf("%s/%ld.mkv", dir, now);

    /* Cấu hình filesink */
    g_object_set(filesink,
                 "location", filename,
                 "async", FALSE,
                 "sync", FALSE,
                 NULL);

    g_print("[%s-%s] Recording to: %s\n",
           rec->camera_name,
           rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB",
           filename);

    /* THÊM: Log pipeline state */
    g_print("[DEBUG] Pipeline created: %p\n", rec->pipeline);
    g_print("[DEBUG] Source: %p, Depay: %p, Parser: %p\n",
            rec->source, rec->depay, rec->parser);

    g_free(dir);
    g_free(filename);

    /* Add elements */
    gst_bin_add_many(GST_BIN(rec->pipeline),
                     rec->source, rec->depay, rec->parser, queue, muxer, filesink,
                     NULL);

    /* Link: depay -> parser -> queue -> muxer -> filesink */
    if (!gst_element_link_many(rec->depay, rec->parser, queue, muxer, filesink, NULL)) {
        g_printerr("Failed to link elements\n");
        goto error;
    }

    /* Lưu muxer và filesink để rotation */
    rec->splitmux = filesink;  // Tái sử dụng biến
    rec->tee = muxer;          // Tái sử dụng biến

    bus = gst_pipeline_get_bus(GST_PIPELINE(rec->pipeline));
    gst_bus_add_watch(bus, bus_call, rec);
    gst_object_unref(bus);

    return TRUE;

error:
    if (rec->pipeline) {
        gst_object_unref(rec->pipeline);
        rec->pipeline = NULL;
    }
    return FALSE;
}

/* Callback khi rtspsrc có pad mới */
static void on_pad_added(GstElement *src, GstPad *new_pad, gpointer data) {
    RecordingPipeline *rec = (RecordingPipeline *)data;
    GstPad *sink_pad = gst_element_get_static_pad(rec->depay, "sink");
    GstPadLinkReturn ret;
    GstCaps *new_pad_caps;
    GstStructure *new_pad_struct;
    const gchar *new_pad_type;

    if (gst_pad_is_linked(sink_pad)) {
        g_object_unref(sink_pad);
        return;
    }

    new_pad_caps = gst_pad_get_current_caps(new_pad);
    if (!new_pad_caps) {
        new_pad_caps = gst_pad_query_caps(new_pad, NULL);
    }

    new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    new_pad_type = gst_structure_get_name(new_pad_struct);

    /* Chỉ link video pad */
    if (!g_str_has_prefix(new_pad_type, "application/x-rtp")) {
        gst_caps_unref(new_pad_caps);
        g_object_unref(sink_pad);
        return;
    }

    ret = gst_pad_link(new_pad, sink_pad);
    if (GST_PAD_LINK_FAILED(ret)) {
        g_printerr("[%s-%s] Failed to link pads: %d\n",
                  rec->camera_name,
                  rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB",
                  ret);
    } else {
        g_print("[%s-%s] Pads linked successfully\n",
               rec->camera_name,
               rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");
    }

    gst_caps_unref(new_pad_caps);
    g_object_unref(sink_pad);
}

/* Rotate recording file bằng cách recreate pipeline */
static gboolean rotate_recording_pipeline(gpointer user_data) {
    RecordingPipeline *rec = (RecordingPipeline *)user_data;

    if (!rec->is_running || !rec->pipeline) {
        return G_SOURCE_REMOVE;
    }

    g_print("[%s-%s] Rotating recording file...\n",
           rec->camera_name,
           rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");

    /* Kiểm tra file hiện tại có data không */
        GstElement *old_filesink = rec->splitmux;
        gchar *old_location = NULL;
        g_object_get(old_filesink, "location", &old_location, NULL);

        if (old_location) {
            struct stat st;
            if (stat(old_location, &st) == 0) {
                g_print("[%s-%s] Completed file: %s (%.2f MB)\n",
                       rec->camera_name,
                       rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB",
                       old_location,
                       st.st_size / (1024.0 * 1024.0));
            }
            g_free(old_location);
        }

    /* ===== QUAN TRỌNG: Remove bus watch trước ===== */
    GstBus *old_bus = gst_pipeline_get_bus(GST_PIPELINE(rec->pipeline));
    gst_bus_remove_watch(old_bus);
    gst_object_unref(old_bus);

    /* Stop current pipeline - KHÔNG gửi EOS */
    gst_element_set_state(rec->pipeline, GST_STATE_NULL);

    /* Wait for NULL state */
    gst_element_get_state(rec->pipeline, NULL, NULL, 2 * GST_SECOND);

    /* Unref pipeline */
    gst_object_unref(rec->pipeline);
    rec->pipeline = NULL;

    /* Small delay để đảm bảo cleanup hoàn toàn */
    g_usleep(100000);  // 100ms

    /* Tạo pipeline mới */
    if (!create_recording_pipeline(rec)) {
        g_printerr("[%s-%s] Failed to recreate pipeline\n",
                  rec->camera_name,
                  rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");
        rec->is_running = FALSE;
        return G_SOURCE_REMOVE;
    }

    /* Connect pad-added signal */
    g_signal_connect(rec->source, "pad-added", G_CALLBACK(on_pad_added), rec);

    /* Start new pipeline */
    GstStateChangeReturn ret = gst_element_set_state(rec->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[%s-%s] Failed to start new pipeline\n",
                  rec->camera_name,
                  rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");
        gst_object_unref(rec->pipeline);
        rec->pipeline = NULL;
        rec->is_running = FALSE;
        return G_SOURCE_REMOVE;
    }

    /* Wait for PLAYING state */
    ret = gst_element_get_state(rec->pipeline, NULL, NULL, 5 * GST_SECOND);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[%s-%s] Pipeline failed to reach PLAYING state after rotation\n",
                  rec->camera_name,
                  rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");
        gst_element_set_state(rec->pipeline, GST_STATE_NULL);
        gst_object_unref(rec->pipeline);
        rec->pipeline = NULL;
        rec->is_running = FALSE;
        return G_SOURCE_REMOVE;
    }

    g_print("[%s-%s] File rotated successfully\n",
           rec->camera_name,
           rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");

    return G_SOURCE_CONTINUE;
}

static gpointer recording_thread_func(gpointer data) {
    RecordingPipeline *rec = (RecordingPipeline *)data;

    g_print("[%s-%s] Starting recording thread...\n",
           rec->camera_name,
           rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");

    /* Create pipeline */
    if (!create_recording_pipeline(rec)) {
        g_printerr("[%s-%s] Failed to create recording pipeline\n",
                  rec->camera_name,
                  rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");
        rec->is_running = FALSE;
        return NULL;
    }

    /* Connect pad-added signal */
    g_signal_connect(rec->source, "pad-added", G_CALLBACK(on_pad_added), rec);

    /* Start pipeline */
    g_print("[%s-%s] Setting pipeline to PLAYING...\n",
           rec->camera_name,
           rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");

    GstStateChangeReturn ret = gst_element_set_state(rec->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[%s-%s] Failed to start recording pipeline\n",
                  rec->camera_name,
                  rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");
        gst_object_unref(rec->pipeline);
        rec->is_running = FALSE;
        return NULL;
    }

    /* Wait for state change */
    GstState state;
    ret = gst_element_get_state(rec->pipeline, &state, NULL, 5 * GST_SECOND);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("[%s-%s] Pipeline failed to reach PLAYING state\n",
                  rec->camera_name,
                  rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");
        gst_element_set_state(rec->pipeline, GST_STATE_NULL);
        gst_object_unref(rec->pipeline);
        rec->is_running = FALSE;
        return NULL;
    }

    /* Create main loop */
    rec->loop = g_main_loop_new(NULL, FALSE);

    /* Thêm timer để rotate file mỗi 2 phút (120 giây) */
    g_timeout_add_seconds(80, rotate_recording_pipeline, rec);

    g_print("[%s-%s] Recording loop started successfully\n",
           rec->camera_name,
           rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");

    g_main_loop_run(rec->loop);

    /* Cleanup */
    g_print("[%s-%s] Stopping recording...\n",
           rec->camera_name,
           rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");

    if (rec->pipeline) {
        gst_element_set_state(rec->pipeline, GST_STATE_NULL);
        gst_object_unref(rec->pipeline);
    }

    g_main_loop_unref(rec->loop);

    rec->is_running = FALSE;
    return NULL;
}
/* ===== PUBLIC API ===== */

RecordingManager* recording_manager_new() {
    RecordingManager *manager = g_new0(RecordingManager, 1);
    manager->capacity = 10;
    manager->pipelines = g_new0(RecordingPipeline, manager->capacity);
    manager->count = 0;
    return manager;
}

gboolean recording_manager_add_camera(RecordingManager *manager,
                                      const gchar *camera_name,
                                      const gchar *rtsp_url_main,
                                      const gchar *rtsp_url_sub,
                                      gboolean is_h265_main,
                                      gboolean is_h265_sub) {
    if (manager->count + 2 > manager->capacity) {
        manager->capacity *= 2;
        manager->pipelines = g_renew(RecordingPipeline, manager->pipelines, manager->capacity);
    }

    /* Main stream */
    RecordingPipeline *main_rec = &manager->pipelines[manager->count++];
    memset(main_rec, 0, sizeof(RecordingPipeline));
    main_rec->camera_name = g_strdup(camera_name);
    main_rec->rtsp_url = g_strdup(rtsp_url_main);
    main_rec->stream_type = STREAM_MAIN;
    main_rec->is_h265 = is_h265_main;

    /* Sub stream */
    RecordingPipeline *sub_rec = &manager->pipelines[manager->count++];
    memset(sub_rec, 0, sizeof(RecordingPipeline));
    sub_rec->camera_name = g_strdup(camera_name);
    sub_rec->rtsp_url = g_strdup(rtsp_url_sub);
    sub_rec->stream_type = STREAM_SUB;
    sub_rec->is_h265 = is_h265_sub;

    g_print("Added camera: %s (Main: %s, Sub: %s)\n",
            camera_name,
            is_h265_main ? "H265" : "H264",
            is_h265_sub ? "H265" : "H264");

    return TRUE;
}

void recording_manager_start_all(RecordingManager *manager) {
    for (gint i = 0; i < manager->count; i++) {
        RecordingPipeline *rec = &manager->pipelines[i];
        if (!rec->is_running) {
            rec->is_running = TRUE;
            rec->thread = g_thread_new(NULL, recording_thread_func, rec);
            g_print("Started recording: %s (%s)\n",
                    rec->camera_name,
                    rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");
        }
    }
}

void recording_manager_stop_all(RecordingManager *manager) {
    for (gint i = 0; i < manager->count; i++) {
        RecordingPipeline *rec = &manager->pipelines[i];
        if (rec->is_running && rec->loop) {
            g_main_loop_quit(rec->loop);
            g_thread_join(rec->thread);
            g_print("Stopped recording: %s (%s)\n",
                    rec->camera_name,
                    rec->stream_type == STREAM_MAIN ? "MAIN" : "SUB");
        }
    }
}

void recording_manager_free(RecordingManager *manager) {
    recording_manager_stop_all(manager);

    for (gint i = 0; i < manager->count; i++) {
        RecordingPipeline *rec = &manager->pipelines[i];
        g_free(rec->camera_name);
        g_free(rec->rtsp_url);
    }

    g_free(manager->pipelines);
    g_free(manager);
}

/* Cleanup old recordings */
void cleanup_old_recordings(const gchar *base_path, guint64 max_size_gb) {
    struct statvfs stat;
    if (statvfs(base_path, &stat) != 0) {
        return;
    }

    guint64 available_gb = (stat.f_bavail * stat.f_frsize) / (1024 * 1024 * 1024);

    /* Nếu còn nhiều dung lượng, không cần xóa */
    if (available_gb > max_size_gb) {
        return;
    }

    g_print("Low disk space (%lu GB), cleaning old recordings...\n", available_gb);

    /* TODO: Implement logic xóa file cũ nhất
     * - Duyệt thư mục đệ quy
     * - Sắp xếp theo thời gian
     * - Xóa file cũ nhất cho đến khi đủ dung lượng
     */
}
