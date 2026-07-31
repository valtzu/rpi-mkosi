#!/bin/bash
# Summarizes the package changelog diff produced by mkosi.postoutput using
# Gemini CLI. Runs outside mkosi's build sandbox (which has no network access
# even with WithNetwork=yes) as a separate CI step.
#
# Rather than feeding Gemini the whole (potentially huge) unified diff in one
# request, this gives it the list of packages whose changelog changed and a
# single scoped tool (get-package-changelog-diff.sh, restricted by
# gemini-policy.toml) to pull one package's diff at a time. That keeps each
# request small regardless of how large the overall diff is.
set -eo pipefail

diff_path=$1
old_changelog=$2
new_changelog=$3
script_dir=$(dirname "$0")

# mkosi.postoutput already writes diff_path.md for the no-changes case.
if [ -e "$diff_path.md" ] ; then
  exit 0
fi

if [ -z "$GEMINI_API_KEY" ] ; then
  >&2 echo "GEMINI_API_KEY not set, skipping changelog summary"
  cp "$diff_path" "$diff_path.md"
  exit 0
fi

if ! [ -s "$old_changelog" ] ; then
  >&2 echo "No previous changelog to compare against, skipping changelog summary"
  cp "$diff_path" "$diff_path.md"
  exit 0
fi

changed=$(python3 "$script_dir/changelog_pkg_diff.py" list "$old_changelog" "$new_changelog")
if [ -z "$changed" ] ; then
  cp "$diff_path" "$diff_path.md"
  exit 0
fi

mkdir -p ~/.gemini/policies
cp "$script_dir/gemini-policy.toml" ~/.gemini/policies/ci.toml

# Explicitly trust this checkout, same as the interactive folder-trust
# dialog would persist, rather than bypassing the trust check entirely.
jq -n --arg d "$(pwd)" '{($d): "TRUST_FOLDER"}' > ~/.gemini/trustedFolders.json

export OLD_CHANGELOG=$old_changelog
export NEW_CHANGELOG=$new_changelog

prompt=$(cat <<EOF
The following Debian packages had their changelog change between releases:
$changed

For EACH package listed above, call this exact tool once to see what changed:
  bash .github/workflows/get-package-changelog-diff.sh <package>

After reviewing all of them, write a concise summary of the changes for release
notes. Do not list the same package name multiple times, instead, list changes
under the same title that mentions the previous version and the new version.
Exclude changes that seem internal to the package (packaging-only changes,
typo fixes, etc).
EOF
)

echo -n "Generating changelog summary with AI..."
gemini_stderr=$(mktemp)
if output=$(npx -y @google/gemini-cli@latest -p "$prompt" --approval-mode=default --model gemini-2.5-flash --output-format json 2>"$gemini_stderr") \
   && summary=$(echo "$output" | jq -re '.response') \
   && [ -n "$summary" ] ; then
  echo "$summary" > "$diff_path.md"
  echo " done."
else
  >&2 echo " failed, falling back to raw diff."
  >&2 cat "$gemini_stderr"
  cp "$diff_path" "$diff_path.md"
fi
rm -f "$gemini_stderr"
