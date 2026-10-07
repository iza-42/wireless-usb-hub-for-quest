LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := questusbip
LOCAL_SRC_FILES := usbip.c
LOCAL_LDLIBS := -llog
LOCAL_CFLAGS := -O2 -Wall
include $(BUILD_SHARED_LIBRARY)
