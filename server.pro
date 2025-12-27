#-------------------------------------------------
#
# Project created by QtCreator
#
#-------------------------------------------------

QT       += core gui

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

TARGET = VideoPlayer_2
TEMPLATE = app


SOURCES += \
    camera_media_factory.c \
    main.c \
    playback_factory.c \
    recording_manager.c \
    server_context.c


INCLUDEPATH += /usr/include/
LIBS += -L/usr/lib/aarch64-linux-gnu -lpthread -lavcodec -lavformat -lavutil -lswscale -lswresample -lavfilter

INCLUDEPATH += /usr/include/gstreamer-1.0 \
               /usr/include/glib-2.0 \
               /usr/lib/x86_64-linux-gnu/glib-2.0/include


LIBS += -L/usr/lib/x86_64-linux-gnu \
        -lgstrtspserver-1.0 -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0

HEADERS += \
    camera_config.h \
    camera_media_factory.h \
    playback_factory.h \
    recording_manager.h \
    server_context.h
