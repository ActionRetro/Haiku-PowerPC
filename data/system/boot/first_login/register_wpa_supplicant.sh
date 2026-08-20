#!/bin/sh

# Register the cross-built wpa_supplicant's app signature so the net server
# can launch it (be_roster->Launch) when joining a WPA/WPA2 network.
mimeset -F /boot/system/non-packaged/bin/wpa_supplicant
