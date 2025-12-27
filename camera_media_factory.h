#ifndef CAMERA_MEDIA_FACTORY_H
#define CAMERA_MEDIA_FACTORY_H

#include <gst/rtsp-server/rtsp-server.h>
#include "camera_config.h"

#define TYPE_CAMERA_MEDIA_FACTORY (camera_media_factory_get_type())
G_DECLARE_FINAL_TYPE(CameraMediaFactory, camera_media_factory, CAMERA, MEDIA_FACTORY, GstRTSPMediaFactory)

CameraMediaFactory* camera_media_factory_new(CameraConfig *cam);

#endif
