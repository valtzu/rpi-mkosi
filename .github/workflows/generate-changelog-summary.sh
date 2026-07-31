#!/bin/bash
# Summarizes the package changelog diff produced by mkosi.postoutput using
# Gemini. Runs outside mkosi's build sandbox (which has no network access
# even with WithNetwork=yes) as a separate CI step.
set -eo pipefail

diff_path=$1

# mkosi.postoutput already writes diff_path.md for the no-changes case.
if [ -e "$diff_path.md" ] ; then
  exit 0
fi

if [ -z "$GEMINI_API_KEY" ] ; then
  >&2 echo "GEMINI_API_KEY not set, skipping changelog summary"
  cp "$diff_path" "$diff_path.md"
  exit 0
fi

echo -n "Generating changelog summary with AI..."
request_path=$(mktemp)
jq -sRcn '{system_instruction:{parts:[{text:"Summarize the package diff for release notes. Do not list the same package name multiple times, instead, list changes under the same title that mentions the previous version and the new version. Exclude changes that seem internal to the package."}]},contents:{parts:[{text:input}]}}' "$diff_path" > "$request_path"
if ! curl -sfL "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent" \
  -H "x-goog-api-key: $GEMINI_API_KEY" \
  --json @"$request_path" \
  | jq -re '.candidates[].content.parts[].text' > "$diff_path.md" ; then
  >&2 echo " failed, falling back to raw diff."
  cp "$diff_path" "$diff_path.md"
else
  echo " done."
fi
rm -f "$request_path"
