#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+
"""Pack U-Boot-only Android v0 containers for stock Mi 5 firmware."""
import argparse,gzip,hashlib,json,struct,subprocess,sys
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--uboot',type=Path,required=True)
    p.add_argument('--jdi-dtb',type=Path,required=True)
    p.add_argument('--lgd-dtb',type=Path,required=True)
    p.add_argument('--mkbootimg',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();blob=a.uboot.read_bytes()
    if blob[56:60]!=b'ARMd' or struct.unpack_from('<Q',blob,8)[0]!=0:
        raise SystemExit('Expected an ARM64 U-Boot Image with text_offset=0')
    a.output.mkdir(parents=True,exist_ok=True)
    images={}
    for panel,dtbpath in [('jdi',a.jdi_dtb),('lgd',a.lgd_dtb)]:
        dtb=dtbpath.read_bytes()
        compatible={'jdi':b'jdi,fhd-r63452','lgd':b'lgd,fhd-td4322'}
        other='lgd' if panel=='jdi' else 'jdi'
        if dtb[:4]!=b'\xd0\x0d\xfe\xed' or struct.unpack_from('>I',dtb,4)[0]!=len(dtb) or compatible[panel] not in dtb or compatible[other] in dtb:
            raise SystemExit('Invalid or mismatched '+panel+' DTB')
        payload=a.output/('u-boot-'+panel+'.kernel-dtb')
        payload.write_bytes(gzip.compress(blob,mtime=0)+dtb)
        target=a.output/('u-boot-mi5-'+panel+'.img')
        subprocess.run([sys.executable,str(a.mkbootimg),'--header_version','0',
            '--base','0x80000000','--kernel_offset','0x8000',
            '--ramdisk_offset','0x1000000','--tags_offset','0x100',
            '--second_offset','0xf00000','--pagesize','4096',
            '--kernel',str(payload),'--cmdline','console=tty0 loglevel=7',
            '--output',str(target)],check=True)
        data=target.read_bytes()
        images[target.name]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
            'panel':panel,'verified_on_device':False,'dtb_sha256':hashlib.sha256(dtb).hexdigest()}
    (a.output/'images.json').write_text(json.dumps(images,indent=2)+'\n')
    print(json.dumps(images,indent=2))

if __name__=='__main__':main()
