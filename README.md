WiFi Switcher for OpenBSD
=========================

This is a simple daemon to handle switching between WiFi networks
automatically in OpenBSD.  On startup and SIGHUP, it reads
/etc/wireless.conf for a prioritized list of networks.  An example:

    homenetwork wpakey homekey
    worknetwork wpakey workkey

To start the daemon, run "wifiswd -i if0", where "if0" is the name of your
interface.
    

