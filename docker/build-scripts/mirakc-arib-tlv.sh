set -eu

BASEDIR=$(cd $(dirname $0); pwd)
BUILDPLATFORM=$1
TARGETPLATFORM=$2

. $BASEDIR/vars.sh

MIRAKC_ARIB_TLV_VERSION='v0.1.0'
MIRAKC_ARIB_TLV_GIT_URL='https://github.com/yuchi0531/mirakc-arib-tlv.git'

git clone --depth=1 --branch=$MIRAKC_ARIB_TLV_VERSION $MIRAKC_ARIB_TLV_GIT_URL .

TRIPLE=$(echo "$RUST_TARGET_TRIPLE" | tr '-' '_' | tr [:lower:] [:upper:])

# Enforce to use the cross linker, as mirakc.sh does.
export CARGO_TARGET_${TRIPLE}_LINKER="$GCC"

cargo build -v --release --locked --target $RUST_TARGET_TRIPLE
$STRIP target/$RUST_TARGET_TRIPLE/release/mirakc-arib-tlv
cp target/$RUST_TARGET_TRIPLE/release/mirakc-arib-tlv /usr/local/bin/
