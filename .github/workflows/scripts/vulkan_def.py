# SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# Writes a module-definition file for vulkan-1.dll from the Vulkan headers, so CI can make the
# loader's import library (llvm-lib /def) without installing the Vulkan SDK. Only the functions a
# library really calls end up imported; those must be ones the loader exports (as with the SDK's
# own import library, which would otherwise fail to link).
#
# Usage: python vulkan_def.py <Vulkan-Headers include/vulkan folder> <output .def>

import pathlib
import re
import sys

headers = pathlib.Path(sys.argv[1])
names = set()
for header in ("vulkan_core.h", "vulkan_win32.h"):
    text = (headers / header).read_text(encoding="utf-8")
    names.update(re.findall(r"VKAPI_ATTR\s+\w+\s+VKAPI_CALL\s+(vk\w+)\s*\(", text))
if not names:
    sys.exit("vulkan_def.py: no Vulkan functions found")
out = pathlib.Path(sys.argv[2])
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text("LIBRARY vulkan-1\nEXPORTS\n" + "".join(f"    {n}\n" for n in sorted(names)))
print(f"vulkan_def.py: {len(names)} functions")
