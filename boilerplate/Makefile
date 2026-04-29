CC     = gcc
CFLAGS = -Wall -Wextra -g -pthread
KDIR   = /lib/modules/$(shell uname -r)/build

.PHONY: all clean

all: engine workload_cpu workload_io monitor.ko

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o engine engine.c -lm

workload_cpu: workload_cpu.c
	$(CC) $(CFLAGS) -o workload_cpu workload_cpu.c -lm

workload_io: workload_io.c
	$(CC) $(CFLAGS) -o workload_io workload_io.c

obj-m += monitor.o

monitor.ko: monitor.c monitor_ioctl.h
	make -C $(KDIR) M=$(PWD) modules

clean:
	rm -f engine workload_cpu workload_io
	make -C $(KDIR) M=$(PWD) clean
