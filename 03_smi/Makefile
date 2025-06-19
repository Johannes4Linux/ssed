obj-m += ssed.o

all: ssed.dtbo
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) modules
clean:
	make -C /lib/modules/$(shell uname -r)/build/ M=$(PWD) clean
	$(RM) ssed.dtbo

ssed.dtbo: ssed.dts
	dtc -I dts -O dtb ssed.dts -o ssed.dtbo
