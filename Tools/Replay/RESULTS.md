# Layer 1 headless verification, 2026-09-27

Working-tree base: a4efad6abc1f5705f926db46caedf0aaa7003b00. Protected live
files were only read; their owners continued editing during this task. This
layer adds no production includes/hooks or changed live behavior versus 25fa030d.

Final extraction: 32 worker processes, 243 files, 1,594,192 normalized rows,
zero malformed input rows. Additional capture folders appeared while other
jobs ran; the final extraction includes the files present when enumerated.
The regression subset remained the same recorded cart/Ralof episode, with
244 gap samples and one camera-coverage record.

Command (from the repository root):

```powershell
& 'C:/Users/mwalt/AppData/Local/Programs/Python/Python312/python.exe' `
  Code/tests/run_replay_checks.py --prove `
  --out C:/Tools/skyrim_re/agent/replay-layer1/checks
```

Result:

```text
Extractor tests: 4 passed; source rows, hashes and values verified
TPTests [replay]: 429 assertions, 9 cases passed
Regression run: exit 0, 0.218 s
Observed acceptance run: exit 7, 0.023 s
CartStepMetric: 152.715 units, target <=30, FAIL
PlanNpcWorn: recorded 0 / owner 2; no-supply 0; complete-plan 2
GapMetric: cart-window median 55.9602; horse-window max 15.0495; target <5
CameraPitch: MISSING rendered pitch / intent / tolerance
RagdollBoneError: MISSING paired body transforms / rotations / settled state
DebrisGap: MISSING paired body transforms / settled state
```

Seven acceptance assertions fail in exactly four expected cases: cart step,
Ralof worn equality, the cart/horse gap comparison, and incomplete camera/body
coverage. The proof driver validates JUnit test identities, not merely a
nonzero process exit. Compilation/linking took about 2.7 s separately. The
cached MSVC environment emitted a vswhere lookup warning but initialized the
compiler successfully; no required tool was blocked.

Logs, JUnit XML and `report.json` (compiled-source and fixture hashes):
`C:/Tools/skyrim_re/agent/replay-layer1/checks/`. Full streams and capture
manifest: `C:/Tools/skyrim_re/agent/replay-layer1/`.

This proves that headless replay detects real recorded failures and that a
complete worn presentation plan differs from the omitted-supply counterfactual.
It does not prove a native inventory/physics/rendering diagnosis or game fix.
No Skyrim process, remote PC, xmake build, deployment or commit was invoked.
COMMON.md waived the local independent reviewers for this job; none reviewed it.

See README.md FOLLOW-UP for owned-file extraction and missing-data adapters.
For the next coordinator-owned game run, preserve the existing cart/worn logs,
look for owner/local worn equality after application without duplicated stock,
and collect same-tick body/presentation and camera-intent data before marking
the remaining acceptance targets proven.
