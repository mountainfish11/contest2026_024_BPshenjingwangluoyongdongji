#!/usr/bin/env bash

set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
workspace_dir=${OPENVELA_WORKSPACE:-$(CDPATH= cd -- "$repo_dir/.." && pwd)}

apply_series()
{
  local name=$1
  local relative_path=$2
  local base_commit=$3
  local final_commit=$4
  local patch_dir=$5
  local target_dir="$workspace_dir/$relative_path"

  echo "[BlindBadge] checking $name"

  if ! git -C "$target_dir" rev-parse --git-dir >/dev/null 2>&1; then
    echo "error: missing Git repository: $target_dir" >&2
    return 1
  fi

  if git -C "$target_dir" merge-base --is-ancestor "$final_commit" HEAD 2>/dev/null; then
    echo "[BlindBadge] $name already contains $final_commit; skip"
    return 0
  fi

  if [ -n "$(git -C "$target_dir" status --porcelain)" ]; then
    echo "error: $name worktree is not clean; preserve local changes and stop" >&2
    return 1
  fi

  if [ "$(git -C "$target_dir" rev-parse HEAD)" != "$base_commit" ]; then
    echo "error: $name HEAD does not match expected base $base_commit" >&2
    echo "actual: $(git -C "$target_dir" rev-parse HEAD)" >&2
    return 1
  fi

  echo "[BlindBadge] applying $name patch series"
  if ! git -C "$target_dir" am "$repo_dir/$patch_dir"/*.patch; then
    echo "error: git am stopped in $name; inspect conflicts there" >&2
    echo "after inspection use 'git am --continue' or 'git am --abort'" >&2
    return 1
  fi
}

apply_series \
  nuttx \
  nuttx \
  861540f7d52c90d6d28f814e6f0b6088c8ecf525 \
  792b2d2d68d \
  patches/openvela/nuttx

apply_series \
  packages_ai_agent \
  packages/ai_agent \
  964117f \
  c463b96 \
  patches/openvela/packages_ai_agent

apply_series \
  apps \
  apps \
  c90b4e6c5 \
  3fed86b66 \
  patches/openvela/apps

echo "[BlindBadge] all openvela patch series are present"
