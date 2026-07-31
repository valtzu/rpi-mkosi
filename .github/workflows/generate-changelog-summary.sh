#!/bin/bash
# Summarizes the package changelog diff produced by mkosi.postoutput using
# Gemini CLI. Runs outside mkosi's build sandbox (which has no network access
# even with WithNetwork=yes) as a separate CI step.
#
# Only the diffs for packages whose changelog actually changed are sent (see
# changelog_pkg_diff.py) rather than the whole raw diff, and this is a single
# one-shot call with no tool use -- an earlier per-package tool-calling
# design made one API round trip per changed package, and since each round
# trip resends the whole growing conversation plus Gemini CLI's own system
# prompt, that cost far more than sending everything in one request.
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

combined_path=$(mktemp)
python3 "$script_dir/changelog_pkg_diff.py" diff-all "$old_changelog" "$new_changelog" > "$combined_path"
if ! [ -s "$combined_path" ] ; then
  rm -f "$combined_path"
  cp "$diff_path" "$diff_path.md"
  exit 0
fi

# Keep the request (and therefore cost) small and bounded regardless of how
# large a given release's changes are.
max_chars=200000
if [ "$(wc -c < "$combined_path")" -gt "$max_chars" ] ; then
  head -c "$max_chars" "$combined_path" > "$combined_path.trunc"
  echo -e "\n\n[... diff truncated, too large to summarize in full ...]" >> "$combined_path.trunc"
  mv "$combined_path.trunc" "$combined_path"
fi

prompt="Summarize the following per-package changelog diff for release notes. Do not
list the same package name multiple times, instead, list changes under the
same title that mentions the previous version and the new version. Exclude
changes that seem internal to the package (packaging-only changes, typo
fixes, etc)."

echo -n "Generating changelog summary with AI..."
gemini_stderr=$(mktemp)
# --skip-trust: headless CI, not an interactive session to persist a trust
# decision for. No custom tools/policy are configured for this call, so
# there's nothing folder trust would otherwise be protecting here.
# The diff is piped via stdin rather than passed as part of -p: putting a
# large diff directly on the command line risks hitting the OS argument
# length limit (ARG_MAX), same failure mode hit earlier with curl.
if output=$(npx -y @google/gemini-cli@latest -p "$prompt" --skip-trust --approval-mode=default --model gemini-2.5-flash-lite --output-format json < "$combined_path" 2>"$gemini_stderr") \
   && summary=$(echo "$output" | jq -re '.response') \
   && [ -n "$summary" ] ; then
  echo "$summary" > "$diff_path.md"
  echo " done."
else
  >&2 echo " failed, falling back to raw diff."
  >&2 cat "$gemini_stderr"
  cp "$diff_path" "$diff_path.md"
fi
rm -f "$gemini_stderr" "$combined_path"
