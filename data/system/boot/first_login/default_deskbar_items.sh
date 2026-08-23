#!/bin/sh

# ppc bring-up markers. On the iMac G3 this script hangs, which wedges Deskbar
# and starves every first-login script behind it (set_tabby_wallpaper.sh among
# them). Which of these four commands blocks cannot be told from the syslog, so
# each one announces itself first. /dev/dprintf is the kernel log - the only
# channel that survives when the desktop is the thing that is stuck.
mark() { echo "first-login/deskbar: $1" > /dev/dprintf 2>/dev/null; }

mark "start"

# install ProcessController, NetworkStatus, PowerStatus & volume control in the Deskbar
mark "ProcessController"
/boot/system/apps/ProcessController -deskbar
mark "NetworkStatus"
/boot/system/apps/NetworkStatus --deskbar
mark "PowerStatus"
# PowerStatus blocks forever on a machine with no battery - it is what hangs
# the iMac G3, and with it Deskbar and every first-login script queued behind
# this one. Only add the battery monitor where there is actually a power
# device. (PowerStatus itself should fail rather than block; that is a
# separate fix, and this guard is right either way - a desktop has no reason
# to show a battery.)
# Backgrounded on purpose. PowerStatus hung here on the iMac G3 and took
# Deskbar and every first-login script behind this one with it. The driver-side
# fix (battery_open refusing when there is no battery) should stop that at
# source - but NO single Deskbar item should ever be able to hold up first
# login, and that property should not depend on any one diagnosis being right.
/boot/system/apps/PowerStatus --deskbar &
mark "desklink --add-volume"
/boot/system/bin/desklink --add-volume
mark "desklink returned"

# install KeymapSwitcher for certain locales
if [[ `locale -l` =~ ^(ru|uk|be)$ ]]; then
   /boot/system/preferences/KeymapSwitcher --deskbar
fi

mark "done"
