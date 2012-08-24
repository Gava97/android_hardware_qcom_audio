# hardware/libaudio-alsa/Android.mk
#
# Copyright 2008 Wind River Systems
#

ifeq ($(strip $(BOARD_USES_ALSA_AUDIO)),true)

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_ARM_MODE := arm
LOCAL_CFLAGS := -D_POSIX_SOURCE
LOCAL_CFLAGS += -DQCOM_ACDB_ENABLED
LOCAL_CFLAGS += -DQCOM_ANC_HEADSET_ENABLED
LOCAL_CFLAGS += -DQCOM_CSDCLIENT_ENABLED
LOCAL_CFLAGS += -DQCOM_FM_TX_ENABLED
LOCAL_CFLAGS += -DQCOM_FM_ENABLED
LOCAL_CFLAGS += -DQCOM_PROXY_DEVICE_ENABLED
LOCAL_CFLAGS += -DQCOM_QCHAT_ENABLED
LOCAL_CFLAGS += -DQCOM_SSR_ENABLED

ifneq ($(ALSA_DEFAULT_SAMPLE_RATE),)
    LOCAL_CFLAGS += -DALSA_DEFAULT_SAMPLE_RATE=$(ALSA_DEFAULT_SAMPLE_RATE)
endif

LOCAL_SRC_FILES := \
  AudioHardwareALSA.cpp         \
  AudioStreamOutALSA.cpp        \
  AudioStreamInALSA.cpp         \
  ALSAStreamOps.cpp             \
  audio_hw_hal.cpp              \
  AudioUsbALSA.cpp              \
  AudioSessionOut.cpp           \
  ALSADevice.cpp

LOCAL_STATIC_LIBRARIES := \
    libmedia_helper \
    libaudiohw_legacy \
    libaudiopolicy_legacy \

LOCAL_SHARED_LIBRARIES := \
    libacdbloader \
    libcutils \
    libutils \
    libmedia \
    libhardware \
    libc        \
    libpower    \
    libalsa-intf \
    libcsd-client \
    libsurround_proc

LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/audio-alsa
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/audcal
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/audio-acdb-util
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/libalsa-intf
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/audio-qmi/csd-client
LOCAL_C_INCLUDES += $(TARGET_OUT_HEADERS)/mm-audio/surround_sound/
LOCAL_C_INCLUDES += hardware/libhardware/include
LOCAL_C_INCLUDES += hardware/libhardware_legacy/include
LOCAL_C_INCLUDES += frameworks/base/include
LOCAL_C_INCLUDES += system/core/include

LOCAL_C_INCLUDES += $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr/include
LOCAL_ADDITIONAL_DEPENDENCIES := $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr

ifeq ($(call is-board-platform,msm8974),true)
  LOCAL_MODULE := audio.primary.msm8974
endif

ifeq ($(call is-board-platform,msm8960),true)
  LOCAL_MODULE := audio.primary.msm8960
endif

LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

include $(CLEAR_VARS)

LOCAL_CFLAGS := -D_POSIX_SOURCE
LOCAL_CFLAGS += -DQCOM_ACDB_ENABLED
LOCAL_CFLAGS += -DQCOM_ANC_HEADSET_ENABLED
LOCAL_CFLAGS += -DQCOM_CSDCLIENT_ENABLED
LOCAL_CFLAGS += -DQCOM_FM_TX_ENABLED
LOCAL_CFLAGS += -DQCOM_FM_ENABLED
LOCAL_CFLAGS += -DQCOM_PROXY_DEVICE_ENABLED
LOCAL_CFLAGS += -DQCOM_QCHAT_ENABLED
LOCAL_CFLAGS += -DQCOM_SSR_ENABLED


ifeq ($(BOARD_HAVE_BLUETOOTH),true)
  LOCAL_CFLAGS += -DWITH_A2DP
endif

LOCAL_SRC_FILES := \
    audio_policy_hal.cpp \
    AudioPolicyManagerALSA.cpp


ifeq ($(call is-board-platform,msm8974),true)
  LOCAL_MODULE := audio_policy.msm8974
endif

ifeq ($(call is-board-platform,msm8960),true)
  LOCAL_MODULE := audio_policy.msm8960
endif

LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE_TAGS := optional

LOCAL_STATIC_LIBRARIES := \
    libmedia_helper \
    libaudiohw_legacy \
    libaudiopolicy_legacy

LOCAL_SHARED_LIBRARIES := \
    libcutils \
    libutils \
    libmedia \
    libaudioparameter

LOCAL_C_INCLUDES += hardware/libhardware_legacy/audio

include $(BUILD_SHARED_LIBRARY)

# Load audio_policy.conf to system/etc/
include $(CLEAR_VARS)
LOCAL_MODULE       := audio_policy.conf
LOCAL_MODULE_TAGS  := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH  := $(TARGET_OUT_ETC)/
LOCAL_SRC_FILES    := audio_policy.conf
include $(BUILD_PREBUILT)
endif
