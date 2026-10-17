#!/bin/bash
# Build a .rpm from source (usage: packaging/build-rpm.sh [version])
# RPMBUILD_ENV can hold extra rpmbuild arguments (e.g. "--nodeps" when building on a non-RPM host).
set -e
cd "$(dirname "$0")/.."
VER=${1:-1.4.1}
TOP=$PWD/build/rpm
rm -rf "$TOP"
mkdir -p "$TOP"/{BUILD,RPMS,SOURCES,SPECS,SRPMS} dist
tar --transform "s,^,5chviewer-$VER/," -czf "$TOP/SOURCES/5chviewer-$VER.tar.gz" \
    Makefile README.md LICENSE app.rc src packaging/5chviewer.desktop packaging/5chviewer.svg packaging/THIRD_PARTY_NOTICES.txt
sed "s/^Version:.*/Version:        $VER/" packaging/5chviewer.spec > "$TOP/SPECS/5chviewer.spec"
${RPMBUILD:-rpmbuild} -bb --define "_topdir $TOP" $RPMBUILD_ENV "$TOP/SPECS/5chviewer.spec"
cp "$TOP"/RPMS/*/*.rpm dist/
ls dist/*.rpm
