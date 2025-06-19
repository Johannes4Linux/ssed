all: ssed.dtbo

clean:
	$(RM) ssed.dtbo

ssed.dtbo: ssed.dts
	dtc -I dts -O dtb ssed.dts -o ssed.dtbo
