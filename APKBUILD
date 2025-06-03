# Maintainer: Your Name <rekudyu@protonmail.com>
pkgname=tt-kmd
pkgver=1.34
pkgrel=0
pkgdesc=" Tenstorrent Kernel Module "
url="https://github.com/tenstorrent/tt-kmd"
arch="all"
license="GPL-2.0"
depends=""
makedepends="akms"
source="https://github.com/tenstorrent/tt-kmd/archive/refs/tags/ttkmd-{$pkgver}.tar.gz"
builddir="$srcdir/tt-kmd-akms-${pkgver}"

build() {
    :
}

package() {
    install -d "/usr/src/tt-kmd/$pkgname"
    tar xzf "$srcdir/tenstorrent-${pkgver}.zip" -c "/usr/src/$pkgname" --strip-components=1
    cd "/usr/src/$pkgname"
    akms install .
    modeprobe tenstorrent
}