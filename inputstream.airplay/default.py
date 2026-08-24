# Entry point for the AirPlay service.
#
# Deliberately nothing but dispatch: Kodi expects an add-on's entry point to be
# short, and everything it does lives in resources/lib where it can be read and
# imported on its own.
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys

from resources.lib.service import forget_devices, main

if __name__ == '__main__':
    # The settings screen runs this script again with an argument, to act on a
    # button; the service itself is started without one.
    if len(sys.argv) > 1 and sys.argv[1] == 'forget':
        forget_devices()
    else:
        main()
