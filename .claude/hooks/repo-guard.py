#!/usr/bin/env python3
"""Claude Code guard for the geromet/MCVR fork."""

from __future__ import annotations

import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

EXPECTED_ORIGIN = "geromet/MCVR"
UPSTREAM_OWNER = "Minecraft-Radiance"


def decision(reason: str) -> None:
    print(json.dumps({"hookSpecificOutput": {
        "hookEventName": "PreToolUse",
        "permissionDecision": "deny",
        "permissionDecisionReason": reason,
    }}))


def git_output(cwd: str, *args: str) -> str | None:
    try:
        p = subprocess.run(
            ["git", "-C", cwd, *args],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=2,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return p.stdout.strip() if p.returncode == 0 else None


def normalize_repo(url: str | None) -> str | None:
    if not url:
        return None
    for pattern in (
        r"^git@github\.com:(?P<repo>[^/]+/[^/]+?)(?:\.git)?$",
        r"^ssh://git@github\.com/(?P<repo>[^/]+/[^/]+?)(?:\.git)?$",
        r"^https?://github\.com/(?P<repo>[^/]+/[^/]+?)(?:\.git)?/?$",
        r"^git://github\.com/(?P<repo>[^/]+/[^/]+?)(?:\.git)?/?$",
    ):
        m = re.match(pattern, url.strip(), flags=re.IGNORECASE)
        if m:
            return m.group("repo").removesuffix(".git")
    return None


def origin_is_expected(cwd: str) -> bool:
    return normalize_repo(git_output(cwd, "remote", "get-url", "origin")) == EXPECTED_ORIGIN


def current_and_default(cwd: str) -> tuple[str | None, str]:
    current = git_output(cwd, "branch", "--show-current")
    origin_head = git_output(cwd, "symbolic-ref", "--quiet", "--short", "refs/remotes/origin/HEAD")
    default = origin_head.removeprefix("origin/") if origin_head else "main"
    return current or None, default


def on_default(cwd: str) -> bool:
    current, default = current_and_default(cwd)
    return current is not None and current == default


def path_inside(path_value: str | None, project_dir: str) -> bool:
    if not path_value:
        return True
    try:
        p = Path(path_value)
        if not p.is_absolute():
            p = Path(project_dir) / p
        p.resolve().relative_to(Path(project_dir).resolve())
        return True
    except (OSError, ValueError):
        return False


def push_remote(command: str) -> str | None:
    try:
        tokens = shlex.split(command)
    except ValueError:
        return None
    for i, token in enumerate(tokens):
        if token == "git" and i + 1 < len(tokens) and tokens[i + 1] == "push":
            j = i + 2
            while j < len(tokens):
                t = tokens[j]
                if t == "--":
                    return tokens[j + 1] if j + 1 < len(tokens) else None
                if t.startswith("-"):
                    j += 2 if t in {"--repo", "--receive-pack", "--exec", "-o", "--push-option"} else 1
                    continue
                return t
            return None
    return None


def push_targets_origin(cwd: str, command: str) -> bool:
    if re.search(r"github\.com[:/]+Minecraft-Radiance/", command, flags=re.IGNORECASE):
        return False
    remote = push_remote(command)
    if remote is None:
        current, _ = current_and_default(cwd)
        remote = git_output(cwd, "config", "--get", f"branch.{current}.remote") if current else None
        remote = remote or "origin"
    if remote == "origin":
        return origin_is_expected(cwd)
    remote_url = git_output(cwd, "remote", "get-url", "--push", remote) or git_output(cwd, "remote", "get-url", remote)
    return normalize_repo(remote_url) == EXPECTED_ORIGIN


def upstream_reference(command: str) -> bool:
    return bool(re.search(
        rf"(?:github\.com[:/]+|api\.github\.com/repos/|(?:^|\s)repos/|(?:-R|--repo)\s+){re.escape(UPSTREAM_OWNER)}/",
        command,
        flags=re.IGNORECASE,
    ))


def upstream_write(command: str) -> bool:
    if not upstream_reference(command):
        return False

    if re.search(r"\bgh\s+(?:"
                 r"pr\s+(?:create|edit|close|reopen|merge|comment|review|ready|update-branch)|"
                 r"issue\s+(?:create|edit|close|reopen|comment|lock|unlock|pin|unpin|transfer)|"
                 r"repo\s+(?:edit|archive|rename|delete)|"
                 r"release\s+(?:create|edit|delete|upload)|"
                 r"workflow\s+(?:run|enable|disable)|"
                 r"label\s+(?:create|edit|delete)|"
                 r"secret\s+(?:set|delete)|"
                 r"variable\s+(?:set|delete)"
                 r")\b", command, flags=re.IGNORECASE):
        return True

    if re.search(r"\bgh\s+api\b", command, flags=re.IGNORECASE):
        if re.search(r"(?:^|\s)(?:-f|-F|--field|--raw-field|--input)(?:\s|=)", command):
            return True
        method = re.search(r"(?:^|\s)(?:-X|--method)(?:\s+|=)([A-Za-z]+)", command)
        if method and method.group(1).upper() not in {"GET", "HEAD"}:
            return True

    if re.search(r"api\.github\.com/repos/Minecraft-Radiance/", command, flags=re.IGNORECASE):
        if re.search(r"(?:^|\s)(?:-X|--request)\s*(?:POST|PUT|PATCH|DELETE)\b|"
                     r"(?:^|\s)(?:-d|--data|--data-raw|--data-binary|--form|-F|--upload-file|-T)(?:\s|=)",
                     command, flags=re.IGNORECASE):
            return True

    return False


def default_command_allowed(command: str) -> bool:
    if re.search(r"(?:&&|\|\||[;\n]|(?<!\|)\|(?!\|))", command):
        return False
    compact = " ".join(command.split())
    safe = (
        r"^(?:pwd|ls(?:\s|$)|cat\s|head\s|tail\s|grep\s|rg\s)",
        r"^git\s+(?:status|log|diff|show|branch(?:\s|$)|remote(?:\s|$)|rev-parse|ls-files|ls-tree|grep|tag(?:\s|$)|fetch(?:\s|$))",
        r"^git\s+(?:switch\s+-c|checkout\s+-b)\s+",
        r"^gh\s+(?:repo\s+view|pr\s+(?:view|list|checks|diff)|issue\s+(?:view|list|status)|run\s+(?:view|list)|workflow\s+(?:view|list)|release\s+(?:view|list))\b",
    )
    return any(re.search(p, compact, flags=re.IGNORECASE) for p in safe)


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except Exception:
        decision("Repository guard could not parse the tool request; failing closed.")
        return 0

    tool = str(payload.get("tool_name", ""))
    tool_input = payload.get("tool_input") or {}
    cwd = str(payload.get("cwd") or os.getcwd())
    project_dir = str(os.environ.get("CLAUDE_PROJECT_DIR") or cwd)

    if tool in {"Write", "Edit", "NotebookEdit", "Bash", "PowerShell"} and not origin_is_expected(cwd):
        decision(f"This Claude workspace is bound to {EXPECTED_ORIGIN}; current origin does not match.")
        return 0

    if tool in {"Write", "Edit", "NotebookEdit"}:
        target = tool_input.get("file_path") or tool_input.get("notebook_path") or tool_input.get("path")
        if not path_inside(str(target) if target else None, project_dir):
            decision("File mutation outside the current repository is not allowed.")
            return 0
        if on_default(cwd):
            decision("Direct file mutation on the configured default branch is forbidden. Create/switch to a dedicated non-default branch first.")
        return 0

    if tool not in {"Bash", "PowerShell"}:
        return 0

    command = " ".join(str(tool_input.get("command", "")).split())

    hard_blocks = (
        (r"\bgit\s+push\b[^\n;&|]*(?:--force(?:-with-lease|-if-includes)?\b|(?:^|\s)-f(?:\s|$))", "force-push is not allowed"),
        (r"\bgit\s+reset\s+--hard\b", "hard reset can discard unfamiliar work"),
        (r"\bgit\s+clean\b", "git clean can discard unfamiliar/untracked work"),
        (r"\bgit\s+branch\s+-D\b", "forced branch deletion is destructive"),
        (r"--no-verify\b", "bypassing repository checks/hooks is prohibited"),
    )
    for pattern, reason in hard_blocks:
        if re.search(pattern, command, flags=re.IGNORECASE):
            decision(f"Repository guard blocked command: {reason}.")
            return 0

    if re.search(r"\bgit\s+push\b", command, flags=re.IGNORECASE) and not push_targets_origin(cwd, command):
        decision(f"Git pushes are restricted to origin={EXPECTED_ORIGIN}; upstream and other repository pushes are blocked.")
        return 0

    if upstream_write(command):
        decision("Minecraft-Radiance upstream is read-only for this Claude workspace. Upstream GitHub/API mutations are blocked.")
        return 0

    if on_default(cwd) and not default_command_allowed(command):
        decision("The configured default branch is read-only for Claude. Use read-only inspection commands or create/switch to a dedicated non-default branch first.")
        return 0

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
