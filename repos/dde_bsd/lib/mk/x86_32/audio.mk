INC_DIR += $(LIB_INC_DIR)/x86_32 $(LIB_INC_DIR)/x86

include $(REP_DIR)/lib/mk/audio.inc

vpath %.S $(LIB_DIR)/x86_32
