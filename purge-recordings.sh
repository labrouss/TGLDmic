#!/bin/bash
# Remove the microphone recordings from ALL history of labrouss/TGLDmic and force-push.
# Needs git-filter-repo:  brew install git-filter-repo   (or: pip3 install git-filter-repo)
set -euo pipefail
REPO="${1:-https://github.com/labrouss/TGLDmic.git}"
command -v git-filter-repo >/dev/null || { echo "git-filter-repo not found (brew install git-filter-repo)"; exit 1; }
W="$(mktemp -d)/TGLDmic"
git clone --no-local "$REPO" "$W"          # normal clone: no refs/pull/* (GitHub rejects pushing those)
cd "$W"
git fetch origin '+refs/heads/*:refs/heads/*' '+refs/tags/*:refs/tags/*' 2>/dev/null || true
wavs() { git log --all --format= --name-only | grep -E '\.wav$' | sort -u || true; }
echo "== Recordings in history before:"; wavs
git filter-repo --force --invert-paths --path-glob '*.wav'
echo "== After (must be empty):"; [ -z "$(wavs)" ] && echo "(none)" || { wavs; echo "still present, aborting"; exit 1; }
git remote add origin "$REPO"              # filter-repo removes the remote on purpose
read -r -p "Force-push rewritten branches and tags to $REPO ? [y/N] " a
[ "$a" = y ] || { echo "Not pushed. Rewritten copy: $W"; exit 0; }
git push --force origin --all
git push --force origin --tags
echo "Done. Re-clone every local copy; old clones still contain the files."
