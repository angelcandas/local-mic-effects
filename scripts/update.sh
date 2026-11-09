#!/usr/bin/env bash
# Checks for and installs MicFX updates in a source checkout.
#
#   scripts/update.sh check      Fetch and report whether new commits are available.
#                                Exit 0 = up to date, 10 = update available, 11 = update
#                                available but local commits/changes block it, 1 = error.
#   scripts/update.sh install    Pull, update the JUCE submodule and rebuild the standalone app
#                                (the same steps as in the README).
#   scripts/update.sh relaunch PID APP
#                                Wait for process PID to quit, then open APP again.
#
# The standalone app runs these from Options -> Check for updates..., but they work from a
# terminal too. Rebuilding changes the app's signature, so macOS asks for mic access again.

set -euo pipefail

# Apps launched from Finder get a minimal PATH, so add the usual Homebrew/CMake locations.
export PATH="/opt/homebrew/bin:/usr/local/bin:/Applications/CMake.app/Contents/bin:$PATH"

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo"

fail() { echo "error: $*" >&2; exit 1; }

# The branch to update from: the current branch's upstream, or origin/main.
upstream() {
    git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || echo "origin/main"
}

fetch() {
    local remote="${1%%/*}" branch="${1#*/}"
    git fetch --quiet "$remote" "$branch" || fail "couldn't reach $remote. Check your internet connection."
}

local_changes() {
    git status --porcelain --untracked-files=no
}

cmd_check() {
    command -v git >/dev/null || fail "git isn't installed."
    git rev-parse --git-dir >/dev/null 2>&1 || fail "$repo isn't a git checkout."

    local up behind ahead
    up="$(upstream)"
    fetch "$up"
    behind="$(git rev-list --count "HEAD..$up")"
    ahead="$(git rev-list --count "$up..HEAD")"

    echo "Installed: $(git log -1 --format='%h %s (%cs)' HEAD)"

    if [[ "$behind" -eq 0 ]]; then
        echo "MicFX is up to date with $up."
        return 0
    fi

    echo "$behind new update(s) on $up:"
    git log --no-merges -n 20 --format='  • %s (%cs)' "HEAD..$up"

    if [[ "$ahead" -gt 0 ]]; then
        echo "This checkout has $ahead commit(s) that aren't on $up, so it can't be updated automatically."
        return 11
    fi
    if [[ -n "$(local_changes)" ]]; then
        echo "There are uncommitted changes. Commit or stash them before updating."
        return 11
    fi
    return 10
}

cmd_install() {
    command -v cmake >/dev/null || fail "cmake isn't installed (brew install cmake ninja)."
    command -v ninja >/dev/null || fail "ninja isn't installed (brew install cmake ninja)."
    [[ -z "$(local_changes)" ]] || fail "there are uncommitted changes. Commit or stash them, then try again."

    local up
    up="$(upstream)"

    echo "==> Pulling $up"
    fetch "$up"
    git merge --ff-only "$up" || fail "this checkout has diverged from $up. Update it with git manually."

    echo "==> Updating JUCE"
    git submodule update --init --recursive

    echo "==> Configuring"
    cmake --preset debug

    echo "==> Building the app (this takes a few minutes)"
    cmake --build --preset app

    echo "==> Done. Relaunch MicFX to use the new version."
}

cmd_relaunch() {
    local pid="${1:?pid}" app="${2:?app path}"

    # Detach so this outlives the app that started it.
    nohup bash -c '
        while kill -0 "$0" 2>/dev/null; do sleep 0.2; done
        if [[ "$(uname)" == "Darwin" ]]; then open "$1"; else "$1" & fi
    ' "$pid" "$app" >/dev/null 2>&1 &
}

case "${1:-}" in
    check)    cmd_check ;;
    install)  cmd_install ;;
    relaunch) shift; cmd_relaunch "$@" ;;
    *)        echo "usage: $0 check | install | relaunch PID APP" >&2; exit 2 ;;
esac
