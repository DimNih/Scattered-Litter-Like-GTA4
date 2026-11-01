LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_MODULE := RubbishGtaIV64
LOCAL_SRC_FILES := main.cpp Rubbish.cpp Paper.cpp Rat.cpp Puddle.cpp PuddleReflection.cpp mod/logger.cpp mod/config.cpp mod/local_diagnostics.cpp
LOCAL_CFLAGS += -O2 -DNDEBUG -std=c++17
LOCAL_CPPFLAGS += -std=c++17
LOCAL_C_INCLUDES += $(LOCAL_PATH) $(LOCAL_PATH)/include $(LOCAL_PATH)/aml-psdk
LOCAL_LDLIBS += -llog -lOpenSLES -lEGL -lGLESv2 -lGLESv3 -lz
include $(BUILD_SHARED_LIBRARY)
