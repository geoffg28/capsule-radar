# PlatformIO pre-script for env:esp32-p4-c6-update.
#
# Finds the ESP-Hosted co-processor image that ships with the Arduino core
# (framework-arduinoespressif32-libs/hosted/esp32c6-v<host version>.bin), has the
# upload write it to a spare flash region past the partition table, and passes
# its offset, size and SHA-256 to the sketch so it can verify what it reads back.
import glob
import hashlib
import os
import re

Import("env")  # noqa: F821  (provided by SCons)

C6_IMAGE_OFFSET = 0x1800000  # 24 MB: beyond default_16MB.csv, inside the 32 MB flash

platform = env.PioPlatform()  # noqa: F821
libs_dir = platform.get_package_dir("framework-arduinoespressif32-libs")
if not libs_dir:
    raise SystemExit("c6_update: framework-arduinoespressif32-libs package not found")

images = sorted(glob.glob(os.path.join(libs_dir, "hosted", "esp32c6-v*.bin")))
if len(images) != 1:
    raise SystemExit("c6_update: expected exactly one hosted/esp32c6-v*.bin, found %r" % images)
image = images[0]

with open(image, "rb") as f:
    data = f.read()
version = re.search(r"esp32c6-v([\d.]+)\.bin$", image).group(1)
sha = hashlib.sha256(data).hexdigest()

print("c6_update: %s (%d bytes, sha256 %s) -> flash 0x%X" % (os.path.basename(image), len(data), sha, C6_IMAGE_OFFSET))

env.Append(  # noqa: F821
    FLASH_EXTRA_IMAGES=[("0x%X" % C6_IMAGE_OFFSET, image)],
    CPPDEFINES=[
        ("C6_IMAGE_OFFSET", "0x%X" % C6_IMAGE_OFFSET),
        ("C6_IMAGE_SIZE", str(len(data))),
        ("C6_IMAGE_SHA256", '\\"%s\\"' % sha),
        ("C6_IMAGE_VERSION", '\\"%s\\"' % version),
    ],
)
