#!/bin/bash
# Tool invoked by Gemini CLI (see generate-changelog-summary.sh) to fetch the
# changelog diff for one named package. OLD_CHANGELOG/NEW_CHANGELOG are set
# by the caller, not the model, so this can only ever diff those two files.
set -eo pipefail
package=$1
python3 "$(dirname "$0")/changelog_pkg_diff.py" diff "$OLD_CHANGELOG" "$NEW_CHANGELOG" "$package"
