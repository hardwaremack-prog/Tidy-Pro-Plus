#!/usr/bin/env python3
"""TidyOS Pro: turn the build outputs into the files the web flasher and the updaters use.

  firmware/pro-<b>-boot.bin        0x0 .. 0x9000   bootloader + partition table
  firmware/pro-<b>-main.bin        0xE000 ..       boot selection (blank), boot menu, TidyOS Pro
  firmware/pro-<b>-dual-main.bin   0xE000 ..       the same + The Shelf + its web app (dual boot)
  firmware/tidyos-pro-<b>.bin      TidyOS Pro, field build (the field tools + everyday apps)
  firmware/tidyos-pro-<b>-full.bin TidyOS Pro, full build (+ every TidyOS-ESP32 app: games, Doodle, Latch...)
                                   The one-click images and the Wi-Fi updater use the full build when there is one.
  manifest-pro-<b>[-dual].json     esp-web-tools manifests (the install buttons)
  manifest-pro[-r8].json           what TidyOS Pro's updater reads
  shelf/ota/shelf-<b>-dual.*       what The Shelf's own updater reads on a dual-boot board

<b> is v4 (ESP32-S3R2), v4-r8 (ESP32-S3R8), v3 (ESP32-S3FN8: 8 MB flash, no PSRAM; TidyOS Pro only,
partitions-v3.csv) or xiao (Seeed XIAO ESP32S3: ESP32-S3R8, 8 MB flash; TidyOS Pro only, partitions-xiao.csv, VGA out). The V3's install manifest is manifest-pro-v3-install.json and its updater reads
manifest-pro-v3.json. The settings area (0x9000-0xE000: Wi-Fi etc.) is
never written by an install, so reinstalling keeps them.

usage: package.py VERSION BUILD_DIR SHELF_WWW_V4 SHELF_WWW_R8
  BUILD_DIR holds b-s3, b-boot, b-shelf (V4), b-s3r8, b-bootr8, b-shelfr8 (V4-R8) and b-s3v3, b-bootv3 (V3), b-s3xiao, b-bootxiao (XIAO),
  plus b-s3full / b-s3r8full (built with -DTIDY_ALL_APPS=1)
"""
import sys, os, json, gzip, hashlib, re

ver, bd, www_v4, www_r8 = sys.argv[1:5]
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
fw = os.path.join(root, 'firmware'); os.makedirs(fw, exist_ok=True)
ota = os.path.join(root, 'shelf', 'ota'); os.makedirs(ota, exist_ok=True)

MENU, TIDY, SHELF, WWW, END_DUAL = 0x10000, 0x80000, 0x380000, 0x580000, 0x780000

def rd(p): return open(p, 'rb').read()
def place(img, off, data, base):
    img[off - base: off - base + len(data)] = data

def shelf_version():
    s = open(os.path.join(root, 'source', 'ShelfDual', 'ShelfV4', 'board.h')).read()
    return re.search(r'#define FW_VERSION "([^"]+)"', s).group(1)

NAMES = {'v4': 'Heltec V4', 'v4-r8': 'Heltec V4-R8', 'v3': 'Heltec V3', 'xiao': 'Seeed XIAO ESP32S3'}
ONLY_TIDY = ('v3', 'xiao')                         # 8 MB boards: TidyOS Pro only, no Shelf
for b, sfx in (('v4', ''), ('v4-r8', 'r8'), ('v3', 'v3'), ('xiao', 'xiao')):
    s3, boot, shelf = (os.path.join(bd, 'b-s3' + sfx), os.path.join(bd, 'b-boot' + sfx), os.path.join(bd, 'b-shelf' + sfx))
    if not os.path.exists(os.path.join(s3, 'TidyOSPro.ino.bin')):
        print('skipping', b, '(not built)'); continue
    bl = rd(os.path.join(s3, 'TidyOSPro.ino.bootloader.bin'))
    pt = rd(os.path.join(s3, 'TidyOSPro.ino.partitions.bin'))
    menu = rd(os.path.join(boot, 'TidyBoot.ino.bin'))
    app = rd(os.path.join(s3, 'TidyOSPro.ino.bin'))
    open(os.path.join(fw, f'tidyos-pro-{b}.bin'), 'wb').write(app)          # field build
    fp = os.path.join(bd, 'b-s3' + sfx + 'full', 'TidyOSPro.ino.bin')
    if os.path.exists(fp):
        app = rd(fp); open(os.path.join(fw, f'tidyos-pro-{b}-full.bin'), 'wb').write(app)
    assert len(menu) <= TIDY - MENU, 'boot menu too big'
    slot = 0x400000 - TIDY if b == 'xiao' else SHELF - TIDY     # partitions-xiao.csv: a 3.5 MB TidyOS Pro slot
    assert len(app) <= slot, 'TidyOS Pro too big'
    bootimg = bytearray(b'\xff' * 0x9000); bootimg[0:len(bl)] = bl; bootimg[0x8000:0x8000 + len(pt)] = pt
    open(os.path.join(fw, f'pro-{b}-boot.bin'), 'wb').write(bootimg)
    main = bytearray(b'\xff' * (TIDY + len(app) - 0xE000))
    place(main, MENU, menu, 0xE000); place(main, TIDY, app, 0xE000)
    open(os.path.join(fw, f'pro-{b}-main.bin'), 'wb').write(main)
    parts = lambda m: [{'path': f'firmware/pro-{b}-boot.bin', 'offset': 0}, {'path': f'firmware/{m}', 'offset': 0xE000}]
    name = 'TidyOS Pro for ' + NAMES[b]
    json.dump({'name': name, 'version': ver, 'new_install_prompt_erase': True,
               'builds': [{'chipFamily': 'ESP32-S3', 'parts': parts(f'pro-{b}-main.bin')}]},
              open(os.path.join(root, f'manifest-pro-{b}' + ('-install' if b in ONLY_TIDY else '') + '.json'), 'w'), indent=2)
    # TidyOS Pro's own updater
    json.dump({'name': name, 'version': ver,
               'tidyos': {'version': ver, 'app': f'firmware/tidyos-pro-{b}' + ('-full' if os.path.exists(fp) else '') + '.bin', 'size': len(app), 'sha256': hashlib.sha256(app).hexdigest()}},
              open(os.path.join(root, {'v4': 'manifest-pro.json', 'v4-r8': 'manifest-pro-r8.json', 'v3': 'manifest-pro-v3.json', 'xiao': 'manifest-pro-xiao.json'}[b]), 'w'), indent=2)
    if b in ONLY_TIDY:                              # no Shelf on the V3 or the XIAO
        print(f'{b}: menu {len(menu)//1024} KB, TidyOS Pro {len(app)//1024} KB of {slot//1024} KB'); continue
    # dual boot: + The Shelf and its web app
    sp = os.path.join(shelf, 'ShelfV4.ino.bin')
    wp = www_v4 if b == 'v4' else www_r8
    old = os.path.join(fw, f'pro-{b}-dual-main.bin')
    reuse = not os.path.exists(sp) and os.path.exists(old)   # The Shelf not rebuilt: take it from the image already here
    if reuse:
        o = rd(old)
        sapp = o[SHELF - 0xE000:WWW - 0xE000].rstrip(b'\xff'); sapp += b'\xff' * (-len(sapp) % 16)
        www = o[WWW - 0xE000:END_DUAL - 0xE000]
    if reuse or (os.path.exists(sp) and os.path.exists(wp)):
        if not reuse: sapp = rd(sp); www = gzip.open(wp).read() if wp.endswith('.gz') else rd(wp)
        assert len(sapp) <= WWW - SHELF and len(www) <= END_DUAL - WWW
        dual = bytearray(b'\xff' * (END_DUAL - 0xE000))
        place(dual, MENU, menu, 0xE000); place(dual, TIDY, app, 0xE000); place(dual, SHELF, sapp, 0xE000); place(dual, WWW, www, 0xE000)
        open(os.path.join(fw, f'pro-{b}-dual-main.bin'), 'wb').write(dual)
        json.dump({'name': name + ' + The Shelf (dual boot)', 'version': ver, 'new_install_prompt_erase': True,
                   'builds': [{'chipFamily': 'ESP32-S3', 'parts': parts(f'pro-{b}-dual-main.bin')}]},
                  open(os.path.join(root, f'manifest-pro-{b}-dual.json'), 'w'), indent=2)
        sid = f'shelf-{b}-dual'
        if reuse: print(f'{b}: The Shelf reused from the existing dual-boot image')
        if not reuse: open(os.path.join(ota, sid + '-app.bin.gz'), 'wb').write(gzip.compress(sapp, 9, mtime=0))
        if not reuse: open(os.path.join(ota, sid + '-www.bin.gz'), 'wb').write(gzip.compress(www, 9, mtime=0))
        if not reuse: json.dump({'version': shelf_version(), 'app': sid + '-app.bin.gz', 'appSize': len(sapp), 'www': sid + '-www.bin.gz', 'wwwSize': len(www)},
                  open(os.path.join(ota, sid + '.json'), 'w'))
    print(f'{b}: menu {len(menu)//1024} KB, TidyOS Pro {len(app)//1024} KB of {(SHELF-TIDY)//1024} KB')
