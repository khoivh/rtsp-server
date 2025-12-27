#include <gst/rtsp-server/rtsp-server.h>
#include <signal.h>
#include "server_context.h"
#include "camera_media_factory.h"
#include "playback_factory.h"
#include "recording_manager.h"

/* Global recording manager */
RecordingManager *g_recording_manager = NULL;
GMainLoop *g_main_loop = NULL;

/* Cleanup callback khi thoát */
static void cleanup_handler(int sig) {
    g_print("\n\n=== Shutting down gracefully ===\n");

    if (g_recording_manager) {
        g_print("Stopping all recordings...\n");
        recording_manager_stop_all(g_recording_manager);
        recording_manager_free(g_recording_manager);
        g_recording_manager = NULL;
    }

    if (g_main_loop) {
        g_main_loop_quit(g_main_loop);
    }
}

int main(int argc, char *argv[]) {
    ServerContext ctx = {0};

    gst_init(&argc, &argv);
    ensure_record_directory();

    /* Setup signal handlers */
    signal(SIGINT, cleanup_handler);
    signal(SIGTERM, cleanup_handler);

    global_ctx = &ctx;
    g_main_loop = g_main_loop_new(NULL, FALSE);
    ctx.server = gst_rtsp_server_new();
    gst_rtsp_server_set_service(ctx.server, "8555");

    /* Cấu hình latency để tối ưu RTSP streaming*/
//    setup_server_latency_profile(ctx.server);

    /* ==== KHỞI TẠO RECORDING MANAGER ==== */
    g_print("\n=== Initializing Recording Manager ===\n");
    g_recording_manager = recording_manager_new();

    /* ==== CẤU HÌNH CAMERA ==== */
    g_print("\n=== Configuring Cameras ===\n");

    /* Camera 1 */
    const gchar *cam1_name = "cam_1";
    const gchar *cam1_main = "rtsp://admin1:Oryza%40123@192.168.104.230:554/cam/realmonitor?channel=1&subtype=0&unicast=true&proto=Onvif";
    const gchar *cam1_sub = "rtsp://admin1:Oryza%40123@192.168.104.230:554/cam/realmonitor?channel=1&subtype=1&unicast=true&proto=Onvif";

    // Add to streaming server
    add_camera(&ctx, cam1_name, cam1_main, cam1_sub, CODEC_H264, CODEC_H264, FALSE);

    // Add to recording manager
    recording_manager_add_camera(g_recording_manager, cam1_name, cam1_main, cam1_sub, FALSE, FALSE);

    /* Camera 2 */
    const gchar *cam2_name = "cam_2";
    const gchar *cam2_main = "rtsp://admin:Oryza%40123@192.168.104.206:554/cam/realmonitor?channel=1&subtype=0&unicast=true&proto=Onvif";
    const gchar *cam2_sub = "rtsp://admin:Oryza%40123@192.168.104.206:554/cam/realmonitor?channel=1&subtype=1&unicast=true&proto=Onvif";

    // Add to streaming server
    add_camera(&ctx, cam2_name, cam2_main, cam2_sub, CODEC_H264, CODEC_H265, FALSE);

    // Add to recording manager (cam2 main=H264, sub=H265)
    recording_manager_add_camera(g_recording_manager, cam2_name, cam2_main, cam2_sub, FALSE, TRUE);

    /* Mount cameras cho streaming */
    g_print("\n=== Mounting RTSP Endpoints ===\n");
    remount_all_cameras(&ctx);

    /* Mount playback endpoints nếu cần */
    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(ctx.server);
    /* Ví dụ mount playback:
     * mount_playback_endpoint(mounts, "cam_1", 1731556800);
     */
    g_object_unref(mounts);

    /* Attach server */
    if (gst_rtsp_server_attach(ctx.server, NULL) == 0) {
        g_printerr("Failed to attach RTSP server\n");
        return -1;
    }

//    /* ==== BẮT ĐẦU RECORDING ==== */
//    g_print("\n=== Starting Continuous Recording ===\n");
//    recording_manager_start_all(g_recording_manager);

//    /* Cleanup disk space mỗi giờ (nếu dưới 50GB) */
//    g_timeout_add_seconds(3600, (GSourceFunc)cleanup_old_recordings,
//                         (gpointer)RECORD_BASE_PATH);

    /* ==== THÔNG TIN SERVER ==== */
    g_print("\n");
    g_print("╔════════════════════════════════════════════════════════════╗\n");
    g_print("║           RTSP SERVER WITH CONTINUOUS RECORDING            ║\n");
    g_print("╠════════════════════════════════════════════════════════════╣\n");
    g_print("║ Server Port: 8554                                          ║\n");
    g_print("╠════════════════════════════════════════════════════════════╣\n");
    g_print("║ Streaming URLs:                                            ║\n");
    g_print("║   cam_1 Main: rtsp://localhost:8554/cam_1                  ║\n");
    g_print("║   cam_1 Sub:  rtsp://localhost:8554/cam_1?stream=1         ║\n");
    g_print("║   cam_2 Main: rtsp://localhost:8554/cam_2                  ║\n");
    g_print("║   cam_2 Sub:  rtsp://localhost:8554/cam_2?stream=1         ║\n");
    g_print("╠════════════════════════════════════════════════════════════╣\n");
    g_print("║ Recording:                                                 ║\n");
    g_print("║   Path: %s                        ║\n", RECORD_BASE_PATH);
    g_print("║   Duration: 2 minutes per file                             ║\n");
    g_print("║   Format: MKV (Matroska)                                   ║\n");
    g_print("║   Structure: /quality/camera/Y/m/d/H/timestamp_seg.mkv     ║\n");
    g_print("╠════════════════════════════════════════════════════════════╣\n");
    g_print("║ Controls:                                                  ║\n");
    g_print("║   Press Ctrl+C to stop gracefully                          ║\n");
    g_print("╚════════════════════════════════════════════════════════════╝\n");
    g_print("\n");

    /* Run main loop */
    g_main_loop_run(g_main_loop);

    /* Cleanup */
    g_print("\n=== Cleaning up resources ===\n");

    if (g_recording_manager) {
        recording_manager_stop_all(g_recording_manager);
        recording_manager_free(g_recording_manager);
    }

    for (gint i = 0; i < ctx.camera_count; i++) {
        g_free(ctx.cameras[i].name);
        g_free(ctx.cameras[i].rtsp_url_main);
        g_free(ctx.cameras[i].rtsp_url_sub);
        g_free(ctx.cameras[i].current_record_file_main);
    }

    g_main_loop_unref(g_main_loop);

    g_print("Server stopped successfully.\n");
    return 0;
}
