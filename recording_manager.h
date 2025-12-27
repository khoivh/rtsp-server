#ifndef RECORDING_MANAGER_H
#define RECORDING_MANAGER_H

#include <gst/gst.h>
#include <glib.h>

#define RECORD_BASE_PATH "/home/oryza/Oryza/recordings"
#define RECORD_HI_QUALITY "hi_quality"
#define RECORD_LOW_QUALITY "low_quality"
#define MAX_FILE_DURATION_NS 120000000000ULL

typedef enum {
    STREAM_MAIN,
    STREAM_SUB
} StreamType;

typedef struct {
    gchar *camera_name;
    gchar *rtsp_url;
    StreamType stream_type;
    GstElement *pipeline;
    GstElement *source;
    GstElement *depay;
    GstElement *parser;
    GstElement *tee;
    GstElement *splitmux;
    gboolean is_h265;
    gboolean is_running;
    GThread *thread;
    GMainLoop *loop;
} RecordingPipeline;

typedef struct {
    RecordingPipeline *pipelines;
    gint count;
    gint capacity;
} RecordingManager;

/* Khởi tạo recording manager */
RecordingManager* recording_manager_new();

/* Thêm camera để record */
gboolean recording_manager_add_camera(RecordingManager *manager,
                                      const gchar *camera_name,
                                      const gchar *rtsp_url_main,
                                      const gchar *rtsp_url_sub,
                                      gboolean is_h265_main,
                                      gboolean is_h265_sub);

/* Bắt đầu record tất cả cameras */
void recording_manager_start_all(RecordingManager *manager);

/* Dừng record tất cả cameras */
void recording_manager_stop_all(RecordingManager *manager);

/* Giải phóng resources */
void recording_manager_free(RecordingManager *manager);

/* Tạo đường dẫn thư mục theo thời gian */
gchar* generate_recording_path(const gchar *camera_name, StreamType stream_type);

/* Kiểm tra và tạo thư mục */
gboolean ensure_recording_directory(const gchar *path);

/* Xóa file cũ khi disk đầy */
void cleanup_old_recordings(const gchar *base_path, guint64 max_size_gb);

#endif // RECORDING_MANAGER_H
