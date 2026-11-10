#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Pack one Mi 5 U-Boot Android v0 image with a panel-neutral input DTB."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

JDI = 'jdi,fhd-r63452'
LGD = 'lgd,fhd-td4322'


def common_input(source, target, logpath):
    data = source.read_bytes()
    if (len(data) < 40 or data[:4] != b'\xd0\x0d\xfe\xed' or
            struct.unpack_from('>I', data, 4)[0] != len(data)):
        raise ValueError('Expected exactly one complete input DTB')
    compatible = subprocess.check_output(
        ['fdtget', '-t', 's', str(source), '/', 'compatible'], text=True).split()
    if not {'xiaomi,gemini', 'qcom,msm8996'} <= set(compatible):
        raise ValueError('Input DTB must describe Xiaomi Mi 5 / MSM8996')
    result = subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts', str(source)],
                            capture_output=True, text=True, check=True)
    dts = result.stdout
    warnings = result.stderr
    declarations = [f'compatible = "{panel}";' for panel in (JDI, LGD)]
    count = sum(dts.count(declaration) for declaration in declarations)
    marker = 'u-boot,gemini-input = "universal-v1";'
    if count == 1:
        if 'u-boot,gemini-input' in dts:
            raise ValueError('Panel-specific tree must not claim the common format')
        for declaration in declarations:
            dts = dts.replace(declaration, 'compatible = "u-boot,gemini-panel";')
        dts, count = re.subn(r'^\tmodel = "[^"\n]*";$',
                            '\tmodel = "Xiaomi Mi 5 (universal U-Boot input)";\n\t' + marker,
                            dts, flags=re.M)
        if count != 1:
            raise ValueError('Missing or ambiguous root model')
    elif count == 0:
        marker_value = subprocess.run(
            ['fdtget', '-t', 's', str(source), '/', 'u-boot,gemini-input'],
            capture_output=True, text=True)
        if marker_value.returncode or marker_value.stdout.strip() != 'universal-v1':
            raise ValueError('Input is neither a supported panel tree nor common input')
    else:
        raise ValueError('Ambiguous panel declarations in input DTB')
    result = subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(target), '-'],
                            input=dts, capture_output=True, text=True, check=True)
    logpath.write_text(warnings + result.stderr)
    common = target.read_bytes()
    if any(panel.encode() in common for panel in (JDI, LGD)):
        raise ValueError('Common input still declares a Linux panel')
    return common


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--uboot', type=Path, required=True)
    parser.add_argument('--input-dtb', type=Path, required=True,
                        help='Known-good Mi 5 JDI/LGD input DTB, or generated common DTB')
    parser.add_argument('--mkbootimg', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    blob = args.uboot.read_bytes()
    if (len(blob) < 64 or blob[56:60] != b'ARMd' or
            struct.unpack_from('<Q', blob, 8)[0] != 0):
        raise ValueError('Expected an ARM64 U-Boot Image with text_offset=0')
    args.output.mkdir(parents=True, exist_ok=True)
    common_path = args.output/'gemini-common.dtb'
    if args.input_dtb.resolve() == common_path.resolve():
        raise ValueError('Use a separate output directory; do not overwrite the input DTB')
    common = common_input(args.input_dtb, common_path, args.output/'input-dtc.log')
    payload = args.output/'u-boot-common.kernel-dtb'
    payload.write_bytes(gzip.compress(blob, mtime=0) + common)
    target = args.output/'u-boot-mi5.img'
    subprocess.run([sys.executable, str(args.mkbootimg), '--header_version', '0',
                    '--base', '0x80000000', '--kernel_offset', '0x8000',
                    '--ramdisk_offset', '0x1000000', '--tags_offset', '0x100',
                    '--second_offset', '0xf00000', '--pagesize', '4096',
                    '--kernel', str(payload), '--cmdline', 'console=tty0 loglevel=7',
                    '--output', str(target)], check=True)
    data = target.read_bytes()
    images = {target.name: dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                               panel='firmware-auto', input_format='universal-v1',
                               dtb_sha256=hashlib.sha256(common).hexdigest(),
                               jdi_verified_on_device=False, lgd_verified_on_device=False)}
    (args.output/'images.json').write_text(json.dumps(images, indent=2) + '\n')
    print(json.dumps(images, indent=2))


if __name__ == '__main__':
    main()
