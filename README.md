XBEE Net
========

This is a driver to convert an XBEE in API mode into a Linux network device for use with other networking layers.

This is loosely based on https://github.com/robbles/xbee-driver but is written from scratch.

It makes a tap device to transport the traffic.

Overview
========

This driver encapsulates higher level packets into XBEE API frames. It does this by using the xbee api to transmit to specific target devices by mac address.

Quick Start
==========

Replace 115200 with your baudrate and /dev/ttyUSB0 with your TTY for your device.

```
make -j4
./xbee_netdev /dev/ttyUSB0 115200
```

You should see a new netdev named `xbeeUSB0`.

Initialization
==============

The module cannot automatically detect xbee devices as they appear as standard serial ttys. As such, it relies on a user-space component to take ownership of the tty, set the baud rate, and then pass control to the kernel module by setting the "line discipline" ID 17.

By setting the line discipline the kernel will pass the tty device to the xbee kernel module, which will then set up a new network interface to represent the device.

As a general rule the tty name is used to create the device name. A tty called "ttyUSB0" will become the network device "xbeeUSB0".

When the user-space component attempts to set the line discipline, the module will command the xbee to enter AT configuration mode. In this sequence the module will verify that the remote device is indeed a compatible xbee with the correct baudrate to communicate, and it will also configure the xbee's API mode, and query the MAC address of the xbee. This address will be used to set the network device MAC correctly. Note, **this is a todo and has not yet been implemented**.

Since the xbees use 64 bit mac addresses, there will be two MAC addresses shown on the network device. The ipv6 addr will be the true 64 bit MAC of the xbee. The ipv4 mac address will be a psuedo-address calculated by taking the last 48 bits of the 64 bit mac address.

Setting mac addresses on the interface is NOT supported as we cannot change the XBEE mac. Any calls to set_mac_address will return an error.

Broadcast packets are supported. Broadcast packets will be sent to the 0x000000000000FFFF mac address range in the XBEE network.


Radio Configuration
===================

This device will automatically set your device in API 1 mode with AO enabled. Make sure your device is running a firmware with API mode included!

MAC Addresses
=============

Ethernet protocol only supports `ETH_ALEN`  - 48 bit or 6 byte MAC addresses. XBEE uses 64 bit or 8 byte MAC addresses.

To bridge the gap, this driver sets the netdev address to the first 48 bits of the XBEE MAC address.

When receiving, this works fine, it's a bit of lost data. But when transmitting, you need to know the extra bits.

This driver uses the XBEE WPAN discovery mechanism to discover peers. It keeps a table of remote peers and remote peer information. It then uses this table to translate 48 bit MAC addresses into full 64 bit XBEE addresses.
