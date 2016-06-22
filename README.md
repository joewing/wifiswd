WiFi Switcher for OpenBSD
=========================

This is a simple daemon to handle switching between WiFi networks
automatically in OpenBSD.  On startup and SIGHUP, it reads
/etc/wireless.conf for a prioritized list of networks.  An example:

```
    homenetwork wpakey homekey
    worknetwork wpakey workkey
    guestnetwork -wpakey
```

The first parameter is the SSID of the network and the rest of the line
provides parameters to ifconfig(8).

To start the daemon, run "wifiswd -d -i if0", where "if0" is the name of
your interface.  Without the `-d` or `-f` option, wifiswd will exit once
it connects.  Use the `-d` or `-f` option to run continuously and
reconnect when the network is lost.

Building
--------
0. `make`
1. `make install` (as root)

Usage
-----
```
usage: wifiswd [options]
options:
   -c <file> The configuration file [/etc/wireless.conf]
   -d        Run in the background
   -f        Run in foreground for debugging
   -h        Display this message
   -i <if>   The interface to use [iwm0]
```

Place in `/etc/rc.local`  to run at boot.
