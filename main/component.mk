COMPONENT_SRCDIRS := . config network provision rf security trigger vibrator
COMPONENT_OBJS := main.o \
    config/device_config.o \
    network/wifi.o \
    network/mqtt.o \
    provision/http_server.o \
    rf/rf_data.o \
    rf/rf_receiver.o \
    security/device_identity.o \
    trigger/rf_supervisor.o \
    trigger/rf_trigger.o \
    vibrator/vibrator.o
COMPONENT_ADD_INCLUDEDIRS := . config network provision rf security trigger vibrator
COMPONENT_EMBED_TXTFILES := network/certs/mqtt_ca.pem
COMPONENT_EMBED_FILES := provision/index.html.gz
