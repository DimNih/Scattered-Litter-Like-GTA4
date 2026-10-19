LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_MODULE := RubbishGtaIV64
LOCAL_SRC_FILES := main.cpp Rubbish.cpp Paper.cpp mod/logger.cpp mod/config.cpp
LOCAL_CFLAGS += -O2 -DNDEBUG -std=c++17
LOCAL_CPPFLAGS += -std=c++17
LOCAL_C_INCLUDES += $(LOCAL_PATH) $(LOCAL_PATH)/include $(LOCAL_PATH)/aml-psdk
LOCAL_LDLIBS += -llog
include $(BUILD_SHARED_LIBRARY)
