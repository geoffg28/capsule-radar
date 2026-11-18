# PlatformIO post-script for env:esp32-p4-c6-update.
#
# esptool refuses to write the C6 image to a P4 ("is not an ESP32-P4 image"). Here it
# is raw data the sketch reads back, so pass write-flash's --force. upload_flags can't
# do this: they land before the write-flash subcommand, where --force is not valid.
Import("env")  # noqa: F821

env.Append(UPLOADERFLAGS=["--force"])  # noqa: F821  (UPLOADERFLAGS ends inside write-flash)
