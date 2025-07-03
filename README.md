# Simple SPI Ethernet Device (SSED) Driver

This repository contains the sources for a Linux Network Driver for the *Simple SPI Ethernet Device* (SSED). The purpose of the driver is to give a simple example how to program a network driver for Linux. A WIZnet Surf 5 board or a custom PCB can be used to act as the SSED.

## Used Hard- and Firmware

Any board with the [WIZnet W7500](https://docs.wiznet.io/Product/iMCU/W7500/overview) microcontroller can be used to act as the SSED. The following hardware boards can be used for the SSED:

- [Custom PCBs](https://github.com/Johannes4Linux/Ethernet_PCBs) (MAC and PHY card)
- [WIZnet Surf 5](https://docs.wiznet.io/Product/Open-Source-Hardware/surf5)

A ST-Link programmer can be used to flash the firmware on the WIZnet W7500 microcontroller. The firmware can be found [here](https://github.com/Johannes4Linux/w7500-spi2eth)

## Connecting SSED

In theory the SSED can be connected to any single board computer exposing an SPI interface and a GPIO pin. I am using a Raspberry Pi to connect the SSED to it. The two officially supported boards must be connected in the following way to the Raspberry Pi.

### MAC card

TBD

### Surf 5

TBD

## Structure of the repository

The following files can be found in this repository:

- This README
- ssed.dts: Device Tree Source File for a Device Tree overlay which adds the SSED
- ssed.c: Sources for the SSED driver
- Makefile: Makefile to build the driver and the Device Tree Overlay

## Using the driver

- Build the driver and the Device Tree overlay:
  ~~~
  make
  ~~~
- Apply the Device Tree overlay (e.g. on a Raspberry Pi with Raspberry Pi OS):
  ~~~
  sudo dtoverlay ssed.dtbo
  ~~~
- Load the driver
  ~~~
  sudo insmod ssed.ko
  ~~~

## Support my work

If you want to support me, you can buy me a coffee [buymeacoffee.com/johannes4linux](https://www.buymeacoffee.com/johannes4linux).
