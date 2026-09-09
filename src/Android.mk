LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE := vtouchmerge
LOCAL_SRC_FILES := vtouchmerge.c
LOCAL_CFLAGS := -O2 -Wall -Wextra -Werror -D_GNU_SOURCE
LOCAL_LDLIBS :=
include $(BUILD_EXECUTABLE)
include $(CLEAR_VARS)
LOCAL_MODULE := vtouchd
LOCAL_SRC_FILES := vtouchd.c
LOCAL_CFLAGS := -O2 -Wall -Wextra -Werror -D_GNU_SOURCE
LOCAL_LDLIBS :=
include $(BUILD_EXECUTABLE)
