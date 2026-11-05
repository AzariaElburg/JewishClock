# PlatformIO pre-script: uploadfs/buildfs build the clock's file system from
# Data/ as the clock gets it - the page files gzipped (utils/web_files.py) -
# staged outside the project folder (local_dirs.FS_STAGE), not from Data/
# itself. Every other target: nothing.

Import("env")
import os
import sys

if any(t in ("buildfs", "uploadfs") for t in COMMAND_LINE_TARGETS):
    sys.path.insert(0, os.path.join(env.get("PROJECT_DIR"), "utils"))
    from local_dirs import FS_STAGE
    from web_files import stage

    stage(os.path.join(env.get("PROJECT_DIR"), "Data"), FS_STAGE)
    env.Replace(PROJECT_DATA_DIR=str(FS_STAGE))
    print("File system from %s (Data/, page files gzipped)" % FS_STAGE)
