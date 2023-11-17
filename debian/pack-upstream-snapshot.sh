#!/bin/sh


set -eu

BRANCH=upstream-git/B3_0_Release

HEAD=$(git rev-parse $BRANCH)
DATE=$(git show -s --format=%ci $HEAD | awk '{print $1; exit;}'|sed 's/-//g')

echo "upstream-git head is $HEAD from $DATE"

if [ -n "${1:-}" ];
then
    echo Syntax: $0
    exit 1
fi

upstream_ver() {
    COMPONENT=$1
    local VER_FILE=src/jrd/build_no.h
    RES=`git show $BRANCH:$VER_FILE | awk "/^#define $COMPONENT / { print \\$3 }" | sed 's,\",,g'`
    if [ -z "$RES" ]; then
        echo "FB_$COMPONENT not found in $BRANCH:$VER_FILE:"
        git show $BRANCH:$VER_FILE
        exit 1
    else
        echo $RES
    fi
}

VER=$(upstream_ver PRODUCT_VER_STRING)

echo "Version is $VER"

TAR="firebird3.0-$VER"
DS=$(perl -F, -anwE'for (@F) { say($_), exit if s/^repacksuffix=([^"]+).*/$1/ }' debian/watch)

git archive --format=tar "--prefix=$TAR/" "$HEAD" | pigz -9 -n > "../$TAR.tar.gz"

mk-origtargz --force-repack --repack-suffix="$DS" --compression=xz --version "$VER" "../$TAR.tar.gz"
