TARGET=mailbox_test

ifeq (,$(TOOLCHAIN_PREFIX))
$(error TOOLCHAIN_PREFIX is not set)
endif

ifeq (,$(CFLAGS))
$(error CFLAGS is not set)
endif

ifeq (,$(LDFLAGS))
$(error LDFLAGS is not set)
endif

CC = $(TOOLCHAIN_PREFIX)gcc

# Define the source files and their corresponding targets
PINGPONG_SRC = mailbox_pingpong_test.c
PINGPONG_TARGET = mailbox_pingpong
PINGPONG_OBJ = $(PINGPONG_SRC:.c=.o) # This generates mailbox_pingpong_test.o

LED_SRC = mailbox_led_test.c
LED_TARGET = mailbox_led
LED_OBJ = $(LED_SRC:.c=.o) # This generates mailbox_led_test.o

RTT_SRC = rpmsg_rtt_bench.c
RTT_TARGET = rpmsg_rtt_bench
RTT_OBJ = $(RTT_SRC:.c=.o)             # rpmsg_rtt_bench.o


RTT_LITTLE_SRC = rpmsg_little_core_cost.c
RTT_LITTLE_TARGET = rpmsg_little_core_cost
RTT_LITTLE_OBJ = $(RTT_LITTLE_SRC:.c=.o)             # rpmsg_little_core_cost.o

RTT_decomp_4_SRC = rpmsg_rtt_decomp4.c
RTT_decomp_4_TARGET = rpmsg_rtt_decomp4
RTT_decomp_4_OBJ = $(RTT_decomp_4_SRC:.c=.o)             # rpmsg_rtt_decomp4.o


# Define an 'all' target to build both by default
.PHONY: all
all: $(PINGPONG_TARGET) $(LED_TARGET) $(RTT_TARGET) $(RTT_LITTLE_TARGET) $(RTT_decomp_4_TARGET)

# Rule to build the pingpong test executable
$(PINGPONG_TARGET): $(PINGPONG_OBJ)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# Rule to build the led test executable
$(LED_TARGET): $(LED_OBJ)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(RTT_TARGET): $(RTT_OBJ)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(RTT_LITTLE_TARGET): $(RTT_LITTLE_OBJ)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(RTT_decomp_4_TARGET): $(RTT_decomp_4_OBJ)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)


# Generic rule to compile .c files into .o files
# This rule will apply to both pingpong and led source files
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: clean
clean:
	@rm -f $(PINGPONG_OBJ) $(LED_OBJ)
	@rm -f $(PINGPONG_TARGET) $(LED_TARGET)

