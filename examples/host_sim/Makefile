# Host simulation build — pure gcc, no MCU toolchain.
#
# -fshort-enums is REQUIRED: fw_info.h guards the on-flash layout with
# _Static_assert(sizeof(FW_Info_t)==8 / Bank_MetaInfo_t==16), which only holds
# when enums are 1 byte. Building without it fails the static assert (by design).

CC      ?= gcc
ROOT     = ../..
CFLAGS   = -std=c11 -Wall -Wextra -Werror -fshort-enums -I$(ROOT) -I.

LIB_SRC  = $(ROOT)/flash_service.c \
           $(ROOT)/ota_scheduler.c \
           $(ROOT)/ota_forwarder.c \
           $(ROOT)/ota_entry_core.c
SIM_SRC  = main.c mock_flash.c

BIN      = host_sim

$(BIN): $(LIB_SRC) $(SIM_SRC)
	$(CC) $(CFLAGS) -o $@ $(LIB_SRC) $(SIM_SRC)

.PHONY: run clean
run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN) $(BIN).exe
