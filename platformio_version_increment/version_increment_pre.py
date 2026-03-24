"""Generate build timestamp header"""
import datetime
import os

Import("env")

#  version_increment_pre.py - Build timestamp generation
#  Generates BUILD_TIMESTAMP in YYMMDD format (e.g., 260323 for March 23, 2026)
#  VERSION is set to the same value for display on screen.

VERSION_HEADER = 'Version.h'

# Generate timestamp in YYMMDD format
BUILD_TIMESTAMP = datetime.datetime.now().strftime("%y%m%d")

print(f'Build timestamp: {BUILD_TIMESTAMP}')

HEADER_FILE = f"""// AUTO GENERATED FILE, DO NOT EDIT
#ifndef VERSION
    #define VERSION "{BUILD_TIMESTAMP}"
#endif
#ifndef BUILD_TIMESTAMP
    #define BUILD_TIMESTAMP "{BUILD_TIMESTAMP}"
#endif
"""

# Determine header file location
if os.environ.get('PLATFORMIO_INCLUDE_DIR') is not None:
    VERSION_HEADER = os.environ.get('PLATFORMIO_INCLUDE_DIR') + os.sep + VERSION_HEADER
elif os.path.exists("include"):
    VERSION_HEADER = "include" + os.sep + VERSION_HEADER
else:
    PROJECT_DIR = env.subst("$PROJECT_DIR")
    if not os.path.exists(PROJECT_DIR + os.sep + "include"):
        os.mkdir(PROJECT_DIR + os.sep + "include")
    VERSION_HEADER = "include" + os.sep + VERSION_HEADER

# Write header file
with open(VERSION_HEADER, 'w+') as FILE:
    FILE.write(HEADER_FILE)
