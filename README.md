XBEE Net
========

This is a driver to convert an XBEE in API mode into a Linux network device for use with other networking layers.

This is loosely based on https://github.com/robbles/xbee-driver but is written from scratch.

Overview
========

This driver encapsulates higher level packets into XBEE API frames. It does this by using the xbee api to transmit to specific target devices by mac address.

Initialization
==============

The module cannot automatically detect xbee devices as they appear as standard serial ttys. As such, it relies on a user-space component to take ownership of the tty, set the baud rate, and then pass control to the kernel module by setting the "line discipline" ID 17.

By setting the line discipline the kernel will pass the tty device to the xbee kernel module, which will then set up a new network interface to represent the device.

As a general rule the tty name is used to create the device name. A tty called "ttyUSB0" will become the network device "xbeeUSB0".

When the user-space component attempts to set the line discipline, the module will command the xbee to enter AT configuration mode. In this sequence the module will verify that the remote device is indeed a compatible xbee with the correct baudrate to communicate, and it will also configure the xbee's API mode, and query the MAC address of the xbee. This address will be used to set the network device MAC correctly. Note, **this is a todo and has not yet been implemented**.

Since the xbees use 64 bit mac addresses, there will be two MAC addresses shown on the network device. The ipv6 addr will be the true 64 bit MAC of the xbee. The ipv4 mac address will be a psuedo-address calculated by taking the first 48 bits of the 64 bit mac address.

Setting mac addresses on the interface is NOT supported as we cannot change the XBEE mac. Any calls to set_mac_address will return an error.

Broadcast packets are supported. Broadcast packets will be sent to the 0x000000000000FFFF mac address range in the XBEE network.

MTU Notes
=========

The MTU for an xbee is quite small (72 bytes?) and as an ethernet device the xbee network device can't exactly set the MTU to anything less than 1500 without problems. Other protocols might require that to be increased with user-space tools to over 1530 bytes.

As a result, fragmentation is implemented in the driver. The MTU can be set to anything, but it is set on default to whatever the actual MTU of the device is to avoid unnecessary fragmentation. It can be changed by user-space tools. Changing this value by user-space tools will engage fragmentation in the driver.

Radio Configuration
===================

There is some effort to automatically configure the xbee when the line discipline is set, but as of now, you will need to do it manually.

First follow these to set up your device properly: https://github.com/digidotcom/xbee_ansic_library#xbee-firmware

There are a few extra changes required by this driver, and these will be described here soon.

Fragmentation
=============

The fragmentation in this driver is implemented as such.

For sending:

 - When sending a packet, the FULL packet is built first. A determination is made that the packet can or cant fit under the standard xbee mtu.
 - If the packet fits in the xbee mtu, it will be immediately transmitted as such.
 - If not, the driver will decide the maximum size of each packet payload (including the header this driver prepends), and splits the data into these fragments.
 - A unique ID is generated for this fragmentation (a unsigned short, 0-255).
 - An initial fragmentation start marker is sent in its own packet. This contains the fragmentation ID, the message fragment size, and the number of fragment messages.
 - Each individual fragment is sent out with its own order (short, maximum of 255 fragments) number.

For receiving:

 - The initial fragment marker is received. If a fragmentation context exists from this sender (identified by remote MAC address) it will be dropped.
 - Some limits are put on the number of fragmentation messages and the individual fragment size, to prevent intentional overflows
 - The driver will allocate a fragmentation context for this individual fragment, including enough space to hold the full message.
 - As the driver receives each fragment, it slots the fragment by its ID into each of the fragment slots in the context.
 - If a fragment is received with the wrong "fragment sequence ID" it will be dropped.
 - If a fragment is received twice, it will be dropped.
 - If the fragment sequence times out (more than 0.5 second) the entire context will be dropped.
 - Once all fragments are received, the message is assembled and passed up the control chain. The context is dropped.

For the future:

 - Allocating a fragment context could take time, this could be optimized by re-using existing fragmentation contexts (create one for each remote device and only free it after it hasn't been used for some time)
