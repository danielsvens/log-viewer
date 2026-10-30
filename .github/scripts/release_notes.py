"""Plan a semantic version and release notes from the tested commit's history."""

import argparse
import re
import subprocess
from pathlib import Path

VERSION = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")


def git(*arguments):
    return subprocess.check_output(["git", *arguments], text=True).strip()


def version(tag):
    match = VERSION.fullmatch(tag)
    return tuple(map(int, match.groups())) if match else None


def commits(commit, previous=None, first_parent=False):
    span = f"{previous}..{commit}" if previous else commit
    arguments = ["log", "--reverse", "--format=%H%x00%s%x00%b%x00", span]
    if first_parent:
        arguments.insert(1, "--first-parent")
    fields = git(*arguments).split("\0")
    return [tuple(fields[i:i + 3]) for i in range(0, len(fields) - 2, 3)]


def plan(repository, commit):
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repository):
        raise ValueError("Expected repository in owner/name form")
    commit = git("rev-parse", f"{commit}^{{commit}}")
    tags = git("tag", "--merged", commit).splitlines()
    semantic = sorted((tag for tag in tags if version(tag)), key=version)
    current = [tag for tag in semantic if git("rev-parse", f"{tag}^{{commit}}") == commit]
    earlier = [tag for tag in semantic if tag not in current]
    previous = earlier[-1] if earlier else None

    # Keep the migration notes limited to changes after the old build-N release.
    if previous is None:
        legacy = [tag for tag in tags if re.fullmatch(r"build-[0-9]+", tag)
                  and git("rev-parse", f"{tag}^{{commit}}") != commit]
        if legacy:
            previous = max(legacy, key=lambda tag: int(tag.removeprefix("build-")))

    changes = commits(commit, previous)
    if current:
        tag = current[-1]
    elif semantic:
        major, minor, patch = version(semantic[-1])
        messages = [f"{subject}\n{body}" for _, subject, body in changes]
        breaking = any(re.match(r"[\w-]+(?:\([^\n)]+\))?!:", message)
                       or re.search(r"^BREAKING[ -]CHANGE:", message, re.MULTILINE)
                       for message in messages)
        feature = any(re.match(r"feat(?:\([^\n)]+\))?:", message) for message in messages)
        tag = f"v{major + 1}.0.0" if breaking else f"v{major}.{minor + 1}.0" if feature else f"v{major}.{minor}.{patch + 1}"
    else:
        tag = "v0.1.0"

    url = f"https://github.com/{repository}"
    lines = ["## Changes", ""]
    for sha, subject, body in commits(commit, previous, first_parent=True):
        sha = sha.strip()
        pr = re.search(r"^Merge pull request #(\d+)\b|\(#(\d+)\)$", subject)
        if pr:
            number = next(group for group in pr.groups() if group)
            if subject.startswith("Merge pull request"):
                subject = next((line.strip() for line in body.splitlines() if line.strip()), subject)
            else:
                subject = re.sub(r"\s*\(#\d+\)$", "", subject)
            reference = f"[#{number}]({url}/pull/{number})"
        else:
            reference = f"[{sha[:7]}]({url}/commit/{sha})"
        lines.append(f"- {subject} ({reference})")

    if len(lines) == 2:
        lines.append("- No changes since the previous release.")
    changelog = f"{url}/compare/{previous}...{tag}" if previous else f"{url}/commits/{tag}"
    lines.extend(["", f"[Full changelog]({changelog})", ""])
    return tag, "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--commit", default="HEAD")
    parser.add_argument("--notes", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    tag, notes = plan(arguments.repository, arguments.commit)
    arguments.notes.write_text(notes)
    if arguments.output:
        with arguments.output.open("a") as output:
            output.write(f"tag={tag}\n")
    print(tag)


if __name__ == "__main__":
    main()
