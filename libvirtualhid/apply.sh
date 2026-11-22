#!/usr/bin/env bash
# Prepare a libvirtualhid checkout with the DS5 USB/IP backend groundwork.
#
# Usage: apply.sh <libvirtualhid-dir>
#
# 1. Copies ../src/usbip/ into <dir>/src/platform/linux/usbip/ when that source directory exists.
# 2. Applies every patches/*.patch in sorted order, aborting on the first one that does not apply.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $(basename "$0") <libvirtualhid-dir>" >&2
  exit 2
fi

lvh_dir="$1"
script_dir="$(cd "$(dirname "$0")" && pwd)"
usbip_src="${script_dir}/../src/usbip"
usbip_dst="${lvh_dir}/src/platform/linux/usbip"
patch_dir="${script_dir}/patches"

if [[ ! -d "${lvh_dir}/.git" && ! -f "${lvh_dir}/.git" ]]; then
  echo "FATAL: ${lvh_dir} is not a git checkout" >&2
  exit 1
fi

if [[ -d "${usbip_src}" ]]; then
  mkdir -p "${usbip_dst}"
  cp -r "${usbip_src}/." "${usbip_dst}/"
  echo "copied ${usbip_src} -> ${usbip_dst}"
else
  echo "skip: ${usbip_src} does not exist yet"
fi

shopt -s nullglob
patches=("${patch_dir}"/*.patch)
shopt -u nullglob

if [[ ${#patches[@]} -eq 0 ]]; then
  echo "no patches in ${patch_dir}"
  exit 0
fi

# Sorted order: glob expansion is already lexicographic, sort explicitly for locale independence.
mapfile -t patches < <(printf '%s\n' "${patches[@]}" | LC_ALL=C sort)

for patch in "${patches[@]}"; do
  name="$(basename "$patch")"
  if ! git -C "${lvh_dir}" apply --check "${patch}"; then
    echo "FATAL: patch ${name} does not apply to libvirtualhid $(git -C "${lvh_dir}" rev-parse --short HEAD)" >&2
    exit 1
  fi
  git -C "${lvh_dir}" apply "${patch}"
  echo "applied ${name}"
done
