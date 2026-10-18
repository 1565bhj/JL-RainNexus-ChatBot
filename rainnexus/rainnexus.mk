# RainNexus 分阶段接入模块

RAINNEXUS_INCLUDES := \
	-I../../../../rainnexus/include \
	-I../../../../rainnexus/src \
	-I../../../../include_lib/net/cJSON_common

RAINNEXUS_SRC_FILES := \
	$(wildcard ../../../../rainnexus/src/*.c)

RAINNEXUS_DEFINES := -DRAINNEXUS_ENABLE=1

