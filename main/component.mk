COMPONENT_SRCDIRS := . config pair rf trigger vibrator
COMPONENT_OBJS := main.o \
    config/device_config.o \
    pair/espnow_pair.o \
    rf/rf_data.o \
    rf/rf_receiver.o \
    trigger/rf_supervisor.o \
    trigger/rf_trigger.o \
    vibrator/vibrator.o
COMPONENT_ADD_INCLUDEDIRS := . config pair rf trigger vibrator
