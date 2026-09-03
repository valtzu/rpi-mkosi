#!/bin/bash
# Prepends a summary of image/config/firmware changes (from git history since the
# previous release) to the release notes, so changes that don't show up in the
# package changelog -- updated firmware, added services, config tweaks -- aren't
# silently shipped.
#
# Walks --first-parent so each merged PR (or direct push to main) is one entry,
# and restricts to device-relevant paths: a PR that only touched CI workflows,
# the README or agent notes is TREESAME to its first parent under that pathspec
# and drops out on its own. mkosi.version / mkosi.sync are excluded too -- the
# weekly snapshot bump is already covered by the package changelog.
set -eo pipefail

md_path=$1
previous_tag=$2
current_ref=${3:-HEAD}

if [ -z "$previous_tag" ] ; then
  >&2 echo "No previous tag, skipping repo change summary"
  exit 0
fi

lines=()
while IFS= read -r -d '' record ; do
  subject=${record%%$'\x1f'*}
  body=${record#*$'\x1f'}
  if [[ $subject =~ ^Merge\ pull\ request\ #([0-9]+) ]] ; then
    num=${BASH_REMATCH[1]}
    title=${body%%$'\n'*}
    lines+=("- ${title:-$subject} (#${num})")
  else
    lines+=("- ${subject}")
  fi
done < <(git log --first-parent -z --format='%s%x1f%b' "$previous_tag..$current_ref" -- . \
  ':(exclude).github' \
  ':(exclude).claude' \
  ':(exclude)README.md' \
  ':(exclude)AGENTS.md' \
  ':(exclude)CLAUDE.md' \
  ':(exclude)TODO.md' \
  ':(exclude)docs' \
  ':(exclude)Vagrantfile' \
  ':(exclude).gitignore' \
  ':(exclude)LICENSE' \
  ':(exclude)mkosi.version' \
  ':(exclude)mkosi.sync')

if [ ${#lines[@]} -eq 0 ] ; then
  >&2 echo "No image-affecting commits since $previous_tag"
  exit 0
fi

section=$(mktemp)
{
  echo "## Image and configuration changes"
  echo
  printf '%s\n' "${lines[@]}"
  echo
} > "$section"

if [ -e "$md_path" ] ; then
  cat "$md_path" >> "$section"
fi
mv "$section" "$md_path"
