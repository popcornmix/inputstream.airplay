# Entry point for the AirPlay service.
#
# Deliberately nothing but this: Kodi expects an add-on's entry point to be
# short, and everything it does lives in resources/lib where it can be read and
# imported on its own.
#
# SPDX-License-Identifier: GPL-2.0-or-later

from resources.lib.service import main

if __name__ == '__main__':
    main()
