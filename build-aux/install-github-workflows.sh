#!/usr/bin/env bash
# Copies CI workflow YAMLs into .github/workflows.
# Needed once because the repo OAuth token lacks the `workflow` scope.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$root/.github/workflows"
cp -v "$root"/build-aux/github-workflows/*.yaml "$root/.github/workflows/"
echo "Workflows installed. Commit and push with a token that has the workflow scope:"
echo "  gh auth refresh -h github.com -s repo,workflow"
echo "  git add .github/workflows && git commit -m 'Add CI workflows' && git push"
