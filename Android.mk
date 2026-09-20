LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE        := RedmptionGUI
LOCAL_SRC_FILES     := redemption_gui.cpp

# Link against Android system libs (no external deps)
LOCAL_LDLIBS        := -llog -landroid -ldl

# Build flags: release-optimized, hidden visibility (no export leaking)
LOCAL_CPPFLAGS      := \
    -std=c++17          \
    -O3                 \
    -fvisibility=hidden \
    -fno-stack-protector \
    -fno-exceptions     \
    -fno-rtti           \
    -DREDEMPTION_GUI_VERSION=\"1.0\" \
    -DREDEMPTION_AUTHOR=\"Vr4se_GrandpaJoe\"

# Strip all debug symbols from release build
LOCAL_STRIP_MODE    := all

include $(BUILD_SHARED_LIBRARY)
