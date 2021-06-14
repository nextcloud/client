#!/bin/bash
# Share file/directory via Nextcloud public link.
# Link URL is put into system clipboard.
#
# Requires a running nextcloud desktop client.
#
# Usage:
#  copy_public_nextcloud_link.sh FILE_OR_DIRECTORY_TO_SHARE
#

if [ -z "$1" ]
then
  echo "Usage: $0 FILE_OR_DIR_TO_SHARE"
  exit 1
fi

file_to_share=$(realpath "$1")

if [ ! -f "${file_to_share}" -a ! -d "${file_to_share}" ]
then
  echo "\"$file_to_share\" is not a regular file or directory or does not exist"
  exit 1
fi

id=$(id -u)
socket="/run/user/${id}/Nextcloud/socket"
if [ $(find "${socket}" -type s | wc -l) -ne 1 ]
then
  echo "Nextcloud socket ($socket) not found, is nextcloud client running?"
  exit 1
fi

register_path=$(echo | socat - "${socket}" | cut -d: -f 2-)
if [ -z "${register_path}" ]
then
  echo "Could not determine Nextcloud's root - sorry, cannot continue without this."
  exit 1
fi

echo "${file_to_share}" | grep -q '^'"${register_path}"
if [ $? -ne 0 ]
then
  echo "File/directory to share must be inside Nextcloud's root (${register_path})!"
  exit 1
fi

status=$(echo "RETRIEVE_FILE_STATUS:${file_to_share}" | socat - "${socket}" | \
 awk -F: '/STATUS:/{print $2}')

if [ "${status}" == "SYNC" ]
then
  echo "File/dir $1 is not yet synced, retry later"
  exit 1
fi

if [ "${status}" != "OK" -a "${status}" != "OK+SWM" ]
then
  echo "Status of ${file_to_share} is not OK ($status), maybe the file/dir is not synced"
  exit 1
fi

echo "COPY_PUBLIC_LINK:${file_to_share}" | socat - "${socket}" >/dev/null
