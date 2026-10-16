#!/usr/bin/env python3
"""Build an AArch64 sysroot for the ARM64 test build from Arch Linux ARM packages.

    python3 -I rsc2/tools/arm64/make_sysroot.py <sysroot-dir>

Downloads the repo databases, resolves the libraries the game links against
(and their dependencies), fetches the packages from several mirrors with each
checked against the database's SHA-256, and unpacks them with bsdtar. No root,
no pacman: the result is only ever a --sysroot for clang and a -L for
qemu-aarch64. Packages are cached in <sysroot-dir>.pkgs, so a rerun only
fetches what changed.
"""
import concurrent.futures as cf
import hashlib
import os
import re
import subprocess
import sys
import tarfile
import time
import urllib.request

MIRRORS = [
    'tw.mirror.archlinuxarm.org', 'ca.us.mirror.archlinuxarm.org',
    'dk.mirror.archlinuxarm.org', 'nj.us.mirror.archlinuxarm.org',
    'fl.us.mirror.archlinuxarm.org', 'mirror.archlinuxarm.org',
]
REPOS = ('core', 'extra', 'alarm')
# What rsc2 links against, plus Mesa's software Vulkan driver (lavapipe) so
# the Vulkan renderer can run under qemu too. gcc is unpacked for its crt
# files and libgcc only, without its dependencies.
WANT = ['glibc', 'linux-api-headers', 'gcc-libs', 'sdl2', 'libepoxy', 'openssl',
        'vulkan-icd-loader', 'vulkan-headers', 'glslang', 'spirv-tools', 'vulkan-swrast']
NODEPS = ['gcc']


def read_or_give_up(r, min_rate=50_000, window=20):
    """The whole body, or None once a `window`-second stretch averages under
    `min_rate` bytes/s: some mirrors crawl, and another one will be faster."""
    chunks, got, t0 = [], 0, time.monotonic()
    while True:
        c = r.read(1 << 16)
        if not c:
            return b''.join(chunks)
        chunks.append(c)
        got += len(c)
        dt = time.monotonic() - t0
        if dt >= window:
            if got / dt < min_rate:
                return None
            got, t0 = 0, time.monotonic()


def fetch(path, dest, start=0, sha=None):
    """Download from the mirrors in turn until one gives the right bytes."""
    for k in range(len(MIRRORS) * 2):
        m = MIRRORS[(start + k) % len(MIRRORS)]
        try:
            with urllib.request.urlopen(f'http://{m}/aarch64/{path}', timeout=30) as r:
                data = read_or_give_up(r)
        except Exception:
            continue
        if data is None:
            continue
        if sha and hashlib.sha256(data).hexdigest() != sha:
            continue
        with open(dest + '.part', 'wb') as f:
            f.write(data)
        os.replace(dest + '.part', dest)
        return True
    return False


def load_db(path):
    """name -> {file, sha, deps}, and provides -> name, from one repo db."""
    pkgs, provides = {}, {}
    texts = {}
    with tarfile.open(path) as t:
        for m in t.getmembers():
            parts = m.name.rsplit('/', 1)
            if len(parts) == 2 and parts[1] in ('desc', 'depends'):
                texts[parts[0]] = texts.get(parts[0], '') + '\n' + t.extractfile(m).read().decode()
    for txt in texts.values():
        f, key = {}, None
        for line in txt.splitlines():
            if line.startswith('%') and line.endswith('%'):
                key = line[1:-1]
                f[key] = []
            elif line and key:
                f[key].append(line)
        if 'NAME' not in f:
            continue
        name = f['NAME'][0]
        pkgs[name] = dict(file=f['FILENAME'][0], sha=f.get('SHA256SUM', [None])[0],
                          deps=f.get('DEPENDS', []))
        for p in f.get('PROVIDES', []):
            provides.setdefault(re.split('[<>=]', p)[0], name)
    return pkgs, provides


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = os.path.abspath(sys.argv[1])
    cache = root + '.pkgs'
    os.makedirs(cache, exist_ok=True)
    os.makedirs(root, exist_ok=True)

    pkgs, provides = {}, {}
    for repo in REPOS:
        db = os.path.join(cache, repo + '.db')
        if not fetch(f'{repo}/{repo}.db', db):
            sys.exit(f'cannot download {repo}.db')
        p, pr = load_db(db)
        for n, v in p.items():
            v['repo'] = repo
            if n not in pkgs or repo == 'alarm':      # alarm overrides upstream
                pkgs[n] = v
        for k, v in pr.items():
            provides.setdefault(k, v)
    for n in pkgs:
        provides[n] = n

    seen, todo = set(), list(WANT + NODEPS)
    while todo:
        d = todo.pop()
        n = provides.get(re.split('[<>=]', d)[0])
        if n is None:
            print('missing:', d)
            continue
        if n in seen:
            continue
        seen.add(n)
        if n not in NODEPS:
            todo.extend(pkgs[n]['deps'])

    names = sorted(seen)
    print(f'{len(names)} packages')

    def get(i_n):
        i, n = i_n
        p = pkgs[n]
        dest = os.path.join(cache, p['file'])
        if os.path.exists(dest) and p['sha'] and \
                hashlib.sha256(open(dest, 'rb').read()).hexdigest() == p['sha']:
            return n, True
        return n, fetch(f"{p['repo']}/{p['file']}", dest, i, p['sha'])

    with cf.ThreadPoolExecutor(12) as ex:
        failed = [n for n, ok in ex.map(get, enumerate(names)) if not ok]
    if failed:
        sys.exit('download failed: ' + ' '.join(failed))

    for n in names:
        subprocess.run(['bsdtar', '-xpf', os.path.join(cache, pkgs[n]['file']), '-C', root,
                        '--exclude', '.PKGINFO', '--exclude', '.MTREE',
                        '--exclude', '.BUILDINFO', '--exclude', '.INSTALL'], check=True)
    print('sysroot ready:', root)


if __name__ == '__main__':
    main()
