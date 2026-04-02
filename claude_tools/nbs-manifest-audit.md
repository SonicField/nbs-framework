---
description: "Audit MANIFEST.honest against the actual repo and fix gaps"
---

# NBS Manifest Audit

Audit `MANIFEST.honest` against the actual filesystem to find missing entries, stale paths, and quality gaps.

## Procedure

### Step 1: Validate manifest parses
```bash
honest-parse MANIFEST.honest
```
If this fails, fix syntax errors before proceeding.

### Step 2: Check for stale paths
For every entry in MANIFEST.honest, verify the `path` field points to a file that exists:
```bash
honest-get MANIFEST.honest manifest entries.N.path
```
Report entries whose paths no longer exist.

### Step 3: Find missing tools
Scan `bin/` for executables not in the manifest:
```bash
for tool in bin/nbs-*; do
    name=$(basename "$tool")
    grep -q "name : '$name'" MANIFEST.honest || echo "MISSING: $name"
done
```

### Step 4: Find missing skills
Scan `claude_tools/*.md` for skill files not in the manifest:
```bash
for skill in claude_tools/nbs-*.md; do
    name=$(basename "$skill" .md)
    grep -q "name : '$name'" MANIFEST.honest || echo "MISSING: $name"
done
```

### Step 5: Assess summary quality
For each entry, check:
- Summary is one sentence (not empty, not a paragraph)
- `when_to_use` is actionable (starts with "When" or describes a trigger)
- Keywords include the tool name and at least 3 domain terms

Flag entries with vague summaries like "Tool for X" or empty when_to_use.

### Step 6: Generate fixes
For each missing entry found in steps 3-4:
1. Read the source file or documentation
2. Write a one-sentence summary
3. Write a one-sentence when_to_use
4. Pick 5-8 keywords
5. Generate the Honest entry in the correct format

For stale entries found in step 2:
- If the file was renamed, update the path
- If the file was deleted, remove the entry

### Step 7: Validate
After all fixes, run `honest-parse MANIFEST.honest` again to confirm the manifest still parses.

## Output Format

```
=== MANIFEST AUDIT ===

Stale paths (entry exists, file does not):
  - <name>: <path> (NOT FOUND)

Missing tools (bin/ tool, no manifest entry):
  - <name>

Missing skills (claude_tools/ skill, no manifest entry):
  - <name>

Summary quality issues:
  - <name>: <issue description>

Entries: N total (N tools, N skills, N documents)
Stale: N
Missing: N
Quality issues: N
```
